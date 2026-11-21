/* AxiomOS - Aurora design system: code editor (IDE) application.
 *
 * A self contained text editor model plus renderer.  The module deliberately
 * avoids dynamic allocation so that it can run from the boot time GUI pool
 * before the heap is fully warmed up; every buffer is statically sized.
 *
 * Model summary:
 *   - up to AXIDE_MAX_LINES logical lines, each AXIDE_MAX_COLS characters
 *   - a caret (line, column) and an anchor for selections
 *   - per line classification used for syntax colouring and for the minimap
 *   - a scroll origin in pixels, a horizontal scroll offset in characters
 *   - up to AXIDE_MAX_TABS open documents (title + dirty flag only)
 *
 * The editor is a "code first" editor: it understands comments, string
 * literals, numeric literals, keywords and preprocessor lines of the C family
 * of languages.  That is enough for the kernel sources of this project and for
 * the handful of other languages shipped with the system.
 */

#include "gui.h"

#define AXIDE_MAX_LINES      512
#define AXIDE_MAX_COLS       160
#define AXIDE_MAX_TABS       8
#define AXIDE_TAB_WIDTH      4
#define AXIDE_LINE_HEIGHT    18
#define AXIDE_GUTTER_WIDTH   56
#define AXIDE_STATUS_HEIGHT  24
#define AXIDE_TABSTRIP_HEIGHT 32
#define AXIDE_MINIMAP_WIDTH  64
#define AXIDE_MAX_UNDO       24
#define AXIDE_MAX_SEARCH     40

/* Palette: deep slate background, warm syntax colours.  This is the Aurora
 * "midnight" editing surface and is intentionally distinct from any other
 * vendor's editor theme. */
#define AXIDE_C_BG          GUI_RGB(0x1A, 0x1B, 0x21)
#define AXIDE_C_BG_SOFT     GUI_RGB(0x21, 0x22, 0x2A)
#define AXIDE_C_GUTTER      GUI_RGB(0x16, 0x17, 0x1C)
#define AXIDE_C_TEXT        GUI_RGB(0xD6, 0xD8, 0xE0)
#define AXIDE_C_LINENO      GUI_RGB(0x52, 0x55, 0x62)
#define AXIDE_C_LINENO_CUR  GUI_RGB(0x9A, 0x9E, 0xB0)
#define AXIDE_C_KEYWORD     GUI_RGB(0xC4, 0x8E, 0xF0)
#define AXIDE_C_TYPE        GUI_RGB(0x6F, 0xC6, 0xD8)
#define AXIDE_C_STRING      GUI_RGB(0xD8, 0xB2, 0x6A)
#define AXIDE_C_NUMBER      GUI_RGB(0xE0, 0x8A, 0x6E)
#define AXIDE_C_COMMENT     GUI_RGB(0x5C, 0x64, 0x72)
#define AXIDE_C_PREPROC     GUI_RGB(0x7E, 0xA8, 0xD8)
#define AXIDE_C_CARET       GUI_RGB(0xE8, 0xEA, 0xF2)
#define AXIDE_C_SELECT      GUI_RGB(0x33, 0x3F, 0x5C)
#define AXIDE_C_CURRENT     GUI_RGB(0x22, 0x24, 0x2C)
#define AXIDE_C_ACCENT      GUI_RGB(0x59, 0xA8, 0xFF)
#define AXIDE_C_ERROR       GUI_RGB(0xE0, 0x60, 0x60)
#define AXIDE_C_HINT        GUI_RGB(0x7A, 0x80, 0x90)

typedef struct axide_tab
{
    char     title[32];
    u8       dirty;
    u8       language;      /* 0 = C, 1 = Rust, 2 = NASM, 3 = plain */
    u32      checksum;
} axide_tab_t;

typedef struct axide_line
{
    char     text[AXIDE_MAX_COLS];
    u16      length;
    u16      indent;
} axide_line_t;

typedef struct axide_undo
{
    u16      line;
    u16      column;
    u8       removed;
    char     previous[16];
} axide_undo_t;

typedef struct axide_editor
{
    axide_line_t lines[AXIDE_MAX_LINES];
    u16          line_count;
    u16          caret_line;
    u16          caret_column;
    u16          anchor_line;
    u16          anchor_column;
    bool_t       has_selection;
    i32          scroll_y;
    i32          scroll_x;
    i32          view_height;
    i32          view_width;
    bool_t       read_only;
    bool_t       wrap;
    bool_t       show_minimap;
    bool_t       show_whitespace;
    bool_t       auto_indent;
    u16          desired_column;
    char         status[64];
    char         search[AXIDE_MAX_SEARCH];
    u16          search_length;
    u16          search_hit_line;
    u16          search_hit_column;
    bool_t       search_active;
    u32          caret_blink;
    axide_tab_t  tabs[AXIDE_MAX_TABS];
    u16          tab_count;
    u16          tab_active;
    axide_undo_t undo[AXIDE_MAX_UNDO];
    u16          undo_count;
    u16          undo_top;
    bool_t       dirty;
    u32          edits;
    u32          keystrokes;
} axide_editor_t;

/* ---------------------------------------------------------------- keywords */

static const char *const axide_keywords[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "return", "goto", "sizeof", "typedef", "struct",
    "union", "enum", "static", "extern", "const", "volatile", "register",
    "inline", "unsigned", "signed", "void", "char", "short", "long",
    "int", "float", "double", "auto", "restrict", "_Bool",
    "let", "mut", "fn", "impl", "trait", "match", "loop", "pub", "crate",
    "mod", "use", "where", "move", "ref", "self", "Self", "as", "dyn",
    "unsafe", "async", "await", "yield", "type", "enum", "const",
    "section", "global", "extern", "bits", "db", "dw", "dd", "dq",
    "resb", "resw", "resd", "resq", "equ", "times"
};

/* Token classes used both by the renderer and by the minimap tint. */
enum axide_token
{
    AXIDE_TOK_PLAIN = 0,
    AXIDE_TOK_KEYWORD,
    AXIDE_TOK_TYPE,
    AXIDE_TOK_STRING,
    AXIDE_TOK_NUMBER,
    AXIDE_TOK_COMMENT,
    AXIDE_TOK_PREPROC,
    AXIDE_TOK_PUNCT,
    AXIDE_TOK_IDENT
};

static i32  axide_text_width(const char *text);
void        axide_add_tab(axide_editor_t *ed, const char *title, u8 language);
void        axide_load_text(axide_editor_t *ed, const char *text);
u16         axide_line_count(const axide_editor_t *ed);
static bool_t axide_is_ident_start(char c);
static bool_t axide_is_ident_char(char c);
static bool_t axide_is_digit(char c);
static bool_t axide_is_space(char c);
static u16  axide_word_length(const char *text, u16 max);
static bool_t axide_word_matches(const char *text, u16 length, const char *word);
static u8   axide_classify_word(const char *text, u16 length);
static void axide_zero_line(axide_line_t *line);
static void axide_copy_text(axide_line_t *line, const char *text);
static void axide_recount_indent(axide_line_t *line);
static void axide_push_undo(axide_editor_t *ed, u16 line, u16 column, char removed);
static bool_t axide_pop_undo(axide_editor_t *ed, axide_undo_t *out);
static bool_t axide_shift_right(axide_line_t *line, u16 at, u16 count);
static bool_t axide_shift_left(axide_line_t *line, u16 at, u16 count);
static bool_t axide_insert_char(axide_line_t *line, u16 at, char c);
static bool_t axide_delete_char(axide_line_t *line, u16 at);
static bool_t axide_split_line(axide_editor_t *ed, u16 at);
static bool_t axide_join_line(axide_editor_t *ed, u16 at);
static bool_t axide_selection_empty(const axide_editor_t *ed);
static void axide_selection_bounds(const axide_editor_t *ed, u16 *l0, u16 *c0,
                                   u16 *l1, u16 *c1);
static void axide_clamp_caret(axide_editor_t *ed);
static void axide_scroll_into_view(axide_editor_t *ed);
static void axide_ensure_visible(axide_editor_t *ed);
static i32  axide_visible_lines(const axide_editor_t *ed);
static i32  axide_line_pixel_y(const axide_editor_t *ed, u16 line);
static u16  axide_line_at_pixel(const axide_editor_t *ed, i32 y);
static u16  axide_column_at_pixel(const axide_editor_t *ed, i32 x);
static void axide_move_home(axide_editor_t *ed);
static void axide_move_end(axide_editor_t *ed);
static void axide_move_word(axide_editor_t *ed, i32 direction);
static void axide_move_vertical(axide_editor_t *ed, i32 delta);
static void axide_select_all(axide_editor_t *ed);
static void axide_delete_selection(axide_editor_t *ed);
static void axide_insert_text(axide_editor_t *ed, const char *text);
static void axide_newline(axide_editor_t *ed);
static void axide_backspace(axide_editor_t *ed);
static void axide_delete_forward(axide_editor_t *ed);
static void axide_undo(axide_editor_t *ed);
static void axide_toggle_comment(axide_editor_t *ed);
static void axide_indent_selection(axide_editor_t *ed, bool_t outward);
static u16  axide_find(axide_editor_t *ed, u16 from_line, const char *needle);
static bool_t axide_match_at(const axide_line_t *line, u16 at, const char *needle);
static void axide_paint_token_run(gui_surface_t *surface, i32 x, i32 y,
                                  const char *text, u16 length, u32 colour, u8 scale);
static u32  axide_token_colour(u8 token);
static void axide_paint_gutter(gui_surface_t *surface, const axide_editor_t *ed,
                               gui_rect_t bounds);
static void axide_paint_minimap(gui_surface_t *surface, const axide_editor_t *ed,
                                gui_rect_t bounds);
static void axide_paint_tabstrip(gui_surface_t *surface, const axide_editor_t *ed,
                                 gui_rect_t bounds);
static void axide_paint_status(gui_surface_t *surface, const axide_editor_t *ed,
                               gui_rect_t bounds);
static void axide_paint_code(gui_surface_t *surface, const axide_editor_t *ed,
                             gui_rect_t bounds);
static void axide_paint_caret(gui_surface_t *surface, const axide_editor_t *ed,
                              gui_rect_t bounds);
static void axide_paint_minimap_viewport(gui_surface_t *surface,
                                         const axide_editor_t *ed,
                                         gui_rect_t bounds);
static void axide_paint_indent_guides(gui_surface_t *surface,
                                      const axide_editor_t *ed,
                                      u16 line_index, i32 y, i32 origin_x);
static void axide_paint_whitespace_marks(gui_surface_t *surface,
                                         const axide_editor_t *ed,
                                         u16 line_index, i32 y, i32 origin_x);

static i32 axide_text_width(const char *text)
{
    return gui_text_width(text, 1);
}

static bool_t axide_is_ident_start(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return true;
    if (c == '_' || c == '$')
        return true;
    return false;
}

static bool_t axide_is_ident_char(char c)
{
    if (axide_is_ident_start(c))
        return true;
    if (c >= '0' && c <= '9')
        return true;
    return false;
}

static bool_t axide_is_digit(char c)
{
    return (bool_t)(c >= '0' && c <= '9');
}

static bool_t axide_is_space(char c)
{
    return (bool_t)(c == ' ' || c == '\t');
}

static u16 axide_word_length(const char *text, u16 max)
{
    u16 length = 0;
    while (length < max && text[length] != 0)
        length++;
    return length;
}

static bool_t axide_word_matches(const char *text, u16 length, const char *word)
{
    u16 i = 0;
    if (length != axide_word_length(word, 32))
        return false;
    while (i < length)
    {
        if (text[i] != word[i])
            return false;
        i++;
    }
    return true;
}

static u8 axide_classify_word(const char *text, u16 length)
{
    u32 i;
    for (i = 0; i < AXIOM_ARRAY_LEN(axide_keywords); i++)
    {
        if (axide_word_matches(text, length, axide_keywords[i]))
        {
            if (length >= 3 && text[length - 1] == 't' && text[0] == 'u')
                return AXIDE_TOK_TYPE;
            if (length == 3 && text[0] == 'i' && text[1] == 'n' && text[2] == 't')
                return AXIDE_TOK_TYPE;
            return AXIDE_TOK_KEYWORD;
        }
    }
    return AXIDE_TOK_IDENT;
}

/* -------------------------------------------------------------- line model */

static void axide_zero_line(axide_line_t *line)
{
    u16 i;
    for (i = 0; i < AXIDE_MAX_COLS; i++)
        line->text[i] = 0;
    line->length = 0;
    line->indent = 0;
}

static void axide_copy_text(axide_line_t *line, const char *text)
{
    u16 i = 0;
    axide_zero_line(line);
    while (text[i] != 0 && i < AXIDE_MAX_COLS - 1)
    {
        line->text[i] = text[i];
        i++;
    }
    line->text[i] = 0;
    line->length = i;
    axide_recount_indent(line);
}

static void axide_recount_indent(axide_line_t *line)
{
    u16 i = 0;
    u16 indent = 0;
    while (i < line->length && axide_is_space(line->text[i]))
    {
        if (line->text[i] == '\t')
            indent = (u16)(indent + AXIDE_TAB_WIDTH);
        else
            indent++;
        i++;
    }
    line->indent = indent;
}

static bool_t axide_shift_right(axide_line_t *line, u16 at, u16 count)
{
    u16 i;
    if (count == 0)
        return true;
    if (at > line->length)
        at = line->length;
    if (line->length + count >= AXIDE_MAX_COLS)
        return false;
    for (i = line->length; i > at; i--)
        line->text[i + count - 1] = line->text[i - 1];
    line->length = (u16)(line->length + count);
    line->text[line->length] = 0;
    return true;
}

static bool_t axide_shift_left(axide_line_t *line, u16 at, u16 count)
{
    u16 i;
    if (count == 0)
        return true;
    if (at >= line->length)
        return true;
    if (at + count > line->length)
        count = (u16)(line->length - at);
    for (i = at; i + count < line->length; i++)
        line->text[i] = line->text[i + count];
    line->length = (u16)(line->length - count);
    line->text[line->length] = 0;
    axide_recount_indent(line);
    return true;
}

static bool_t axide_insert_char(axide_line_t *line, u16 at, char c)
{
    u16 i;
    if (at > line->length)
        at = line->length;
    if (line->length + 1 >= AXIDE_MAX_COLS)
        return false;
    for (i = line->length; i > at; i--)
        line->text[i] = line->text[i - 1];
    line->text[at] = c;
    line->length = (u16)(line->length + 1);
    line->text[line->length] = 0;
    return true;
}

static bool_t axide_delete_char(axide_line_t *line, u16 at)
{
    u16 i;
    if (at >= line->length)
        return false;
    for (i = at; i + 1 < line->length; i++)
        line->text[i] = line->text[i + 1];
    line->length = (u16)(line->length - 1);
    line->text[line->length] = 0;
    axide_recount_indent(line);
    return true;
}

static bool_t axide_split_line(axide_editor_t *ed, u16 at)
{
    u16 i;
    u16 tail;
    if (ed->line_count >= AXIDE_MAX_LINES)
        return false;
    for (i = ed->line_count; i > at + 1; i--)
        ed->lines[i] = ed->lines[i - 1];
    axide_zero_line(&ed->lines[at + 1]);
    tail = ed->lines[at].length;
    if (tail > AXIDE_MAX_COLS - 2)
        tail = AXIDE_MAX_COLS - 2;
    for (i = 0; i < tail; i++)
        ed->lines[at + 1].text[i] = ed->lines[at].text[i];
    ed->lines[at + 1].length = tail;
    ed->lines[at + 1].text[tail] = 0;
    ed->lines[at].length = 0;
    ed->lines[at].text[0] = 0;
    ed->line_count = (u16)(ed->line_count + 1);
    axide_recount_indent(&ed->lines[at]);
    axide_recount_indent(&ed->lines[at + 1]);
    return true;
}

static bool_t axide_join_line(axide_editor_t *ed, u16 at)
{
    u16 i;
    if (at + 1 >= ed->line_count)
        return false;
    if (ed->lines[at].length + ed->lines[at + 1].length >= AXIDE_MAX_COLS)
        return false;
    for (i = 0; i < ed->lines[at + 1].length; i++)
        ed->lines[at].text[ed->lines[at].length + i] = ed->lines[at + 1].text[i];
    ed->lines[at].length = (u16)(ed->lines[at].length + ed->lines[at + 1].length);
    ed->lines[at].text[ed->lines[at].length] = 0;
    for (i = at + 1; i + 1 < ed->line_count; i++)
        ed->lines[i] = ed->lines[i + 1];
    ed->line_count = (u16)(ed->line_count - 1);
    axide_recount_indent(&ed->lines[at]);
    return true;
}

/* --------------------------------------------------------------- undo ring */

static void axide_push_undo(axide_editor_t *ed, u16 line, u16 column, char removed)
{
    axide_undo_t *slot = &ed->undo[ed->undo_top];
    slot->line = line;
    slot->column = column;
    slot->removed = (u8)removed;
    slot->previous[0] = 0;
    ed->undo_top = (u16)((ed->undo_top + 1) % AXIDE_MAX_UNDO);
    if (ed->undo_count < AXIDE_MAX_UNDO)
        ed->undo_count++;
}

static bool_t axide_pop_undo(axide_editor_t *ed, axide_undo_t *out)
{
    if (ed->undo_count == 0)
        return false;
    ed->undo_top = (u16)((ed->undo_top + AXIDE_MAX_UNDO - 1) % AXIDE_MAX_UNDO);
    *out = ed->undo[ed->undo_top];
    ed->undo_count--;
    return true;
}

/* --------------------------------------------------------------- selection */

static bool_t axide_selection_empty(const axide_editor_t *ed)
{
    if (!ed->has_selection)
        return true;
    if (ed->anchor_line == ed->caret_line && ed->anchor_column == ed->caret_column)
        return true;
    return false;
}

static void axide_selection_bounds(const axide_editor_t *ed, u16 *l0, u16 *c0,
                                   u16 *l1, u16 *c1)
{
    if (ed->anchor_line < ed->caret_line)
    {
        *l0 = ed->anchor_line;
        *c0 = ed->anchor_column;
        *l1 = ed->caret_line;
        *c1 = ed->caret_column;
        return;
    }
    if (ed->anchor_line > ed->caret_line)
    {
        *l0 = ed->caret_line;
        *c0 = ed->caret_column;
        *l1 = ed->anchor_line;
        *c1 = ed->anchor_column;
        return;
    }
    if (ed->anchor_column <= ed->caret_column)
    {
        *l0 = ed->caret_line;
        *c0 = ed->anchor_column;
        *l1 = ed->caret_line;
        *c1 = ed->caret_column;
        return;
    }
    *l0 = ed->caret_line;
    *c0 = ed->caret_column;
    *l1 = ed->caret_line;
    *c1 = ed->anchor_column;
}

static void axide_clamp_caret(axide_editor_t *ed)
{
    if (ed->line_count == 0)
    {
        ed->caret_line = 0;
        ed->caret_column = 0;
        return;
    }
    if (ed->caret_line >= ed->line_count)
        ed->caret_line = (u16)(ed->line_count - 1);
    if (ed->caret_column > ed->lines[ed->caret_line].length)
        ed->caret_column = ed->lines[ed->caret_line].length;
    if (ed->anchor_line >= ed->line_count)
        ed->anchor_line = (u16)(ed->line_count - 1);
    if (ed->anchor_column > ed->lines[ed->anchor_line].length)
        ed->anchor_column = ed->lines[ed->anchor_line].length;
}

/* ------------------------------------------------------------------ scroll */

static i32 axide_visible_lines(const axide_editor_t *ed)
{
    i32 available = ed->view_height;
    if (available <= 0)
        return 1;
    return available / AXIDE_LINE_HEIGHT;
}

static void axide_scroll_into_view(axide_editor_t *ed)
{
    i32 max_scroll;
    if (ed->scroll_y < 0)
        ed->scroll_y = 0;
    max_scroll = ed->line_count * AXIDE_LINE_HEIGHT - ed->view_height;
    if (max_scroll < 0)
        max_scroll = 0;
    if (ed->scroll_y > max_scroll)
        ed->scroll_y = max_scroll;
    if (ed->scroll_x < 0)
        ed->scroll_x = 0;
}

static void axide_ensure_visible(axide_editor_t *ed)
{
    i32 top = ed->caret_line * AXIDE_LINE_HEIGHT;
    i32 bottom = top + AXIDE_LINE_HEIGHT;
    if (top < ed->scroll_y)
        ed->scroll_y = top;
    if (bottom > ed->scroll_y + ed->view_height)
        ed->scroll_y = bottom - ed->view_height;
    axide_scroll_into_view(ed);
}

static i32 axide_line_pixel_y(const axide_editor_t *ed, u16 line)
{
    return (i32)line * AXIDE_LINE_HEIGHT - ed->scroll_y;
}

static u16 axide_line_at_pixel(const axide_editor_t *ed, i32 y)
{
    i32 index = (y + ed->scroll_y) / AXIDE_LINE_HEIGHT;
    if (index < 0)
        return 0;
    if (index >= (i32)ed->line_count)
        return (u16)(ed->line_count == 0 ? 0 : ed->line_count - 1);
    return (u16)index;
}

static u16 axide_column_at_pixel(const axide_editor_t *ed, i32 x)
{
    char buffer[2];
    u16 column = 0;
    u16 limit = ed->lines[ed->caret_line].length;
    i32 advance = 0;
    buffer[1] = 0;
    while (column < limit)
    {
        buffer[0] = ed->lines[ed->caret_line].text[column];
        advance += gui_text_width(buffer, 1);
        if (advance > x)
            break;
        column++;
    }
    return column;
}

/* -------------------------------------------------------------- caret moves */

static void axide_move_home(axide_editor_t *ed)
{
    ed->caret_column = 0;
    ed->desired_column = 0;
}

static void axide_move_end(axide_editor_t *ed)
{
    ed->caret_column = ed->lines[ed->caret_line].length;
    ed->desired_column = ed->caret_column;
}

static void axide_move_word(axide_editor_t *ed, i32 direction)
{
    if (direction < 0)
    {
        while (ed->caret_column > 0 && axide_is_space(ed->lines[ed->caret_line].text[ed->caret_column - 1]))
            ed->caret_column--;
        while (ed->caret_column > 0 && axide_is_ident_char(ed->lines[ed->caret_line].text[ed->caret_column - 1]))
            ed->caret_column--;
    }
    else
    {
        while (ed->caret_column < ed->lines[ed->caret_line].length &&
               axide_is_ident_char(ed->lines[ed->caret_line].text[ed->caret_column]))
            ed->caret_column++;
        while (ed->caret_column < ed->lines[ed->caret_line].length &&
               axide_is_space(ed->lines[ed->caret_line].text[ed->caret_column]))
            ed->caret_column++;
    }
    ed->desired_column = ed->caret_column;
}

static void axide_move_vertical(axide_editor_t *ed, i32 delta)
{
    i32 target = (i32)ed->caret_line + delta;
    if (target < 0)
        target = 0;
    if (target >= (i32)ed->line_count)
        target = (i32)(ed->line_count == 0 ? 0 : ed->line_count - 1);
    ed->caret_line = (u16)target;
    if (ed->desired_column > ed->lines[ed->caret_line].length)
        ed->caret_column = ed->lines[ed->caret_line].length;
    else
        ed->caret_column = ed->desired_column;
}

static void axide_select_all(axide_editor_t *ed)
{
    ed->anchor_line = 0;
    ed->anchor_column = 0;
    ed->caret_line = (u16)(ed->line_count == 0 ? 0 : ed->line_count - 1);
    ed->caret_column = ed->lines[ed->caret_line].length;
    ed->has_selection = true;
}

/* ------------------------------------------------------------- edit actions */

static void axide_delete_selection(axide_editor_t *ed)
{
    u16 l0, c0, l1, c1;
    u16 i;
    if (axide_selection_empty(ed))
        return;
    axide_selection_bounds(ed, &l0, &c0, &l1, &c1);
    if (l0 == l1)
    {
        axide_shift_left(&ed->lines[l0], c0, (u16)(c1 - c0));
    }
    else
    {
        ed->lines[l0].length = c0;
        ed->lines[l0].text[c0] = 0;
        for (i = 0; i < ed->lines[l1].length - c1; i++)
            ed->lines[l0].text[c0 + i] = ed->lines[l1].text[c1 + i];
        ed->lines[l0].length = (u16)(c0 + ed->lines[l1].length - c1);
        ed->lines[l0].text[ed->lines[l0].length] = 0;
        for (i = l0 + 1; i + (l1 - l0) < ed->line_count; i++)
            ed->lines[i] = ed->lines[i + (u16)(l1 - l0)];
        ed->line_count = (u16)(ed->line_count - (l1 - l0));
        axide_recount_indent(&ed->lines[l0]);
    }
    ed->caret_line = l0;
    ed->caret_column = c0;
    ed->anchor_line = l0;
    ed->anchor_column = c0;
    ed->has_selection = false;
    ed->dirty = true;
    ed->edits++;
}

static void axide_insert_text(axide_editor_t *ed, const char *text)
{
    u16 i = 0;
    axide_delete_selection(ed);
    while (text[i] != 0)
    {
        if (text[i] == '\n')
        {
            axide_split_line(ed, ed->caret_line);
            ed->caret_line = (u16)(ed->caret_line + 1);
            ed->caret_column = 0;
        }
        else
        {
            if (!axide_insert_char(&ed->lines[ed->caret_line], ed->caret_column, text[i]))
                break;
            ed->caret_column++;
        }
        i++;
    }
    axide_recount_indent(&ed->lines[ed->caret_line]);
    ed->desired_column = ed->caret_column;
    ed->dirty = true;
    ed->edits++;
    axide_ensure_visible(ed);
}

static void axide_newline(axide_editor_t *ed)
{
    u16 indent = 0;
    axide_delete_selection(ed);
    axide_push_undo(ed, ed->caret_line, ed->caret_column, 0);
    indent = ed->lines[ed->caret_line].indent;
    axide_split_line(ed, ed->caret_line);
    ed->caret_line = (u16)(ed->caret_line + 1);
    ed->caret_column = 0;
    if (ed->auto_indent)
    {
        while (indent >= AXIDE_TAB_WIDTH && ed->caret_column + AXIDE_TAB_WIDTH < AXIDE_MAX_COLS - 2)
        {
            axide_insert_char(&ed->lines[ed->caret_line], ed->caret_column, ' ');
            ed->caret_column++;
            indent = (u16)(indent - AXIDE_TAB_WIDTH);
        }
        while (indent > 0)
        {
            axide_insert_char(&ed->lines[ed->caret_line], ed->caret_column, ' ');
            ed->caret_column++;
            indent--;
        }
    }
    ed->dirty = true;
    ed->edits++;
    axide_ensure_visible(ed);
}

static void axide_backspace(axide_editor_t *ed)
{
    if (!axide_selection_empty(ed))
    {
        axide_delete_selection(ed);
        return;
    }
    if (ed->caret_column > 0)
    {
        char removed = ed->lines[ed->caret_line].text[ed->caret_column - 1];
        axide_push_undo(ed, ed->caret_line, (u16)(ed->caret_column - 1), removed);
        axide_delete_char(&ed->lines[ed->caret_line], (u16)(ed->caret_column - 1));
        ed->caret_column--;
    }
    else if (ed->caret_line > 0)
    {
        u16 previous_length = ed->lines[ed->caret_line - 1].length;
        axide_join_line(ed, (u16)(ed->caret_line - 1));
        ed->caret_line--;
        ed->caret_column = previous_length;
    }
    ed->dirty = true;
    ed->edits++;
    axide_ensure_visible(ed);
}

static void axide_delete_forward(axide_editor_t *ed)
{
    if (!axide_selection_empty(ed))
    {
        axide_delete_selection(ed);
        return;
    }
    if (ed->caret_column < ed->lines[ed->caret_line].length)
        axide_delete_char(&ed->lines[ed->caret_line], ed->caret_column);
    else if (ed->caret_line + 1 < ed->line_count)
        axide_join_line(ed, ed->caret_line);
    ed->dirty = true;
    ed->edits++;
}

static void axide_undo(axide_editor_t *ed)
{
    axide_undo_t entry;
    if (!axide_pop_undo(ed, &entry))
        return;
    if (entry.removed != 0)
    {
        if (entry.line < ed->line_count)
        {
            axide_insert_char(&ed->lines[entry.line], entry.column, (char)entry.removed);
            ed->caret_line = entry.line;
            ed->caret_column = (u16)(entry.column + 1);
        }
    }
    else if (entry.line < ed->line_count)
    {
        axide_delete_char(&ed->lines[entry.line], entry.column);
        ed->caret_line = entry.line;
        ed->caret_column = entry.column;
    }
    axide_clamp_caret(ed);
    ed->dirty = true;
    axide_ensure_visible(ed);
}

static void axide_toggle_comment(axide_editor_t *ed)
{
    u16 l0, c0, l1, c1;
    u16 line;
    if (ed->line_count == 0)
        return;
    if (axide_selection_empty(ed))
    {
        l0 = ed->caret_line;
        l1 = ed->caret_line;
    }
    else
    {
        axide_selection_bounds(ed, &l0, &c0, &l1, &c1);
    }
    for (line = l0; line <= l1; line++)
    {
        axide_line_t *ptr = &ed->lines[line];
        if (ptr->length >= 2 && ptr->text[0] == '/' && ptr->text[1] == '/')
            axide_shift_left(ptr, 0, 2);
        else if (ptr->length < AXIDE_MAX_COLS - 2)
        {
            axide_shift_right(ptr, 0, 2);
            ptr->text[0] = '/';
            ptr->text[1] = '/';
            axide_recount_indent(ptr);
        }
    }
    ed->dirty = true;
    ed->edits++;
}

static void axide_indent_selection(axide_editor_t *ed, bool_t outward)
{
    u16 l0, c0, l1, c1;
    u16 line;
    if (axide_selection_empty(ed))
    {
        if (outward)
            axide_insert_text(ed, "    ");
        return;
    }
    axide_selection_bounds(ed, &l0, &c0, &l1, &c1);
    for (line = l0; line <= l1; line++)
    {
        if (outward)
        {
            u16 k;
            for (k = 0; k < AXIDE_TAB_WIDTH; k++)
                axide_insert_char(&ed->lines[line], 0, ' ');
            axide_recount_indent(&ed->lines[line]);
        }
        else
        {
            u16 k = 0;
            while (k < AXIDE_TAB_WIDTH && ed->lines[line].length > 0 &&
                   ed->lines[line].text[0] == ' ')
            {
                axide_delete_char(&ed->lines[line], 0);
                k++;
            }
            axide_recount_indent(&ed->lines[line]);
        }
    }
    ed->dirty = true;
    ed->edits++;
}

/* ----------------------------------------------------------------- search */

static bool_t axide_match_at(const axide_line_t *line, u16 at, const char *needle)
{
    u16 i = 0;
    if (needle[0] == 0)
        return false;
    if (at >= line->length)
        return false;
    while (needle[i] != 0)
    {
        if (at + i >= line->length)
            return false;
        if (line->text[at + i] != needle[i])
            return false;
        i++;
    }
    return true;
}

static u16 axide_find(axide_editor_t *ed, u16 from_line, const char *needle)
{
    u16 line;
    u16 column;
    if (needle[0] == 0)
        return 0;
    for (line = from_line; line < ed->line_count; line++)
    {
        for (column = 0; column < ed->lines[line].length; column++)
        {
            if (axide_match_at(&ed->lines[line], column, needle))
            {
                ed->search_hit_line = line;
                ed->search_hit_column = column;
                ed->caret_line = line;
                ed->caret_column = (u16)(column + axide_word_length(needle, AXIDE_MAX_SEARCH));
                axide_ensure_visible(ed);
                return (u16)(line + 1);
            }
        }
    }
    return 0;
}

/* --------------------------------------------------------------- rendering */

static u32 axide_token_colour(u8 token)
{
    switch (token)
    {
    case AXIDE_TOK_KEYWORD: return AXIDE_C_KEYWORD;
    case AXIDE_TOK_TYPE:    return AXIDE_C_TYPE;
    case AXIDE_TOK_STRING:  return AXIDE_C_STRING;
    case AXIDE_TOK_NUMBER:  return AXIDE_C_NUMBER;
    case AXIDE_TOK_COMMENT: return AXIDE_C_COMMENT;
    case AXIDE_TOK_PREPROC: return AXIDE_C_PREPROC;
    case AXIDE_TOK_PUNCT:   return AXIDE_C_HINT;
    case AXIDE_TOK_IDENT:   return AXIDE_C_TEXT;
    default:                return AXIDE_C_TEXT;
    }
}

static void axide_paint_token_run(gui_surface_t *surface, i32 x, i32 y,
                                  const char *text, u16 length, u32 colour, u8 scale)
{
    char buffer[AXIDE_MAX_COLS];
    u16 i;
    if (length == 0)
        return;
    if (length >= AXIDE_MAX_COLS)
        length = AXIDE_MAX_COLS - 1;
    for (i = 0; i < length; i++)
        buffer[i] = text[i];
    buffer[length] = 0;
    gui_text(surface, x, y, buffer, colour, scale);
}

static void axide_paint_indent_guides(gui_surface_t *surface,
                                      const axide_editor_t *ed,
                                      u16 line_index, i32 y, i32 origin_x)
{
    u16 indent = ed->lines[line_index].indent;
    i32 step = gui_text_width(" ", 1) * AXIDE_TAB_WIDTH;
    i32 x = origin_x + 4;
    u16 level = 0;
    if (step <= 0)
        return;
    while (level < indent / AXIDE_TAB_WIDTH + 1)
    {
        gui_vline(surface, x, y, y + AXIDE_LINE_HEIGHT - 1, GUI_RGB(0x2C, 0x2E, 0x38));
        x += step;
        level++;
    }
}

static void axide_paint_whitespace_marks(gui_surface_t *surface,
                                         const axide_editor_t *ed,
                                         u16 line_index, i32 y, i32 origin_x)
{
    const char *text = ed->lines[line_index].text;
    u16 length = ed->lines[line_index].length;
    u16 i;
    i32 x = origin_x;
    char mark[2];
    mark[1] = 0;
    for (i = 0; i < length; i++)
    {
        char glyph[2];
        glyph[0] = text[i];
        glyph[1] = 0;
        if (text[i] == ' ')
        {
            mark[0] = '.';
            gui_text(surface, x + 2, y + 4, mark, GUI_RGB(0x3A, 0x3D, 0x48), 1);
        }
        else if (text[i] == '\t')
        {
            mark[0] = '>';
            gui_text(surface, x + 2, y + 4, mark, GUI_RGB(0x4A, 0x40, 0x30), 1);
        }
        x += gui_text_width(glyph, 1);
    }
}

static void axide_paint_gutter(gui_surface_t *surface, const axide_editor_t *ed,
                               gui_rect_t bounds)
{
    u16 first = axide_line_at_pixel(ed, 0);
    u16 last = axide_line_at_pixel(ed, bounds.h);
    u16 line;
    gui_fill_rect(surface, gui_rect(bounds.x, bounds.y, AXIDE_GUTTER_WIDTH, bounds.h),
                  AXIDE_C_GUTTER);
    gui_vline(surface, bounds.x + AXIDE_GUTTER_WIDTH - 1, bounds.y,
              bounds.y + bounds.h, GUI_RGB(0x2B, 0x2D, 0x36));
    for (line = first; line <= last && line < ed->line_count; line++)
    {
        char number[16];
        i32 y = bounds.y + axide_line_pixel_y(ed, line);
        u32 colour = (line == ed->caret_line) ? AXIDE_C_LINENO_CUR : AXIDE_C_LINENO;
        u16 i;
        u16 digits = 1;
        u32 value = line + 1;
        while (value >= 10)
        {
            value /= 10;
            digits++;
        }
        for (i = 0; i < digits; i++)
            number[i] = '0';
        number[digits] = 0;
        value = line + 1;
        for (i = digits; i > 0; i--)
        {
            number[i - 1] = (char)('0' + (value % 10));
            value /= 10;
        }
        gui_text(surface, bounds.x + AXIDE_GUTTER_WIDTH - 12 - digits * 8, y + 2,
                 number, colour, 1);
        if (line == ed->caret_line)
            gui_fill_rect(surface, gui_rect(bounds.x, y, AXIDE_GUTTER_WIDTH - 1,
                                            AXIDE_LINE_HEIGHT),
                          GUI_RGB(0x22, 0x24, 0x2C));
    }
}

static void axide_paint_minimap_viewport(gui_surface_t *surface,
                                         const axide_editor_t *ed,
                                         gui_rect_t bounds)
{
    i32 total = ed->line_count * 2;
    i32 start;
    i32 height;
    i32 top;
    if (total <= 0)
        return;
    if (total > bounds.h)
        total = bounds.h;
    start = ed->scroll_y / AXIDE_LINE_HEIGHT * 2;
    height = ed->view_height / AXIDE_LINE_HEIGHT * 2;
    if (height < 4)
        height = 4;
    top = bounds.y + start * bounds.h / (ed->line_count * 2 > 0 ? ed->line_count * 2 : 1);
    gui_rect_outline(surface, gui_rect(bounds.x + 2, top, bounds.w - 4, height),
                     GUI_RGB(0x44, 0x48, 0x58));
}

static void axide_paint_minimap(gui_surface_t *surface, const axide_editor_t *ed,
                                gui_rect_t bounds)
{
    u16 line;
    gui_fill_rect(surface, bounds, AXIDE_C_BG_SOFT);
    gui_vline(surface, bounds.x, bounds.y, bounds.y + bounds.h,
              GUI_RGB(0x2B, 0x2D, 0x36));
    for (line = 0; line < ed->line_count; line++)
    {
        i32 y = bounds.y + (i32)line * bounds.h / (ed->line_count > 0 ? ed->line_count : 1);
        i32 width = ed->lines[line].length * 2;
        i32 i;
        if (y >= bounds.y + bounds.h)
            break;
        for (i = 0; i < width; i += 6)
        {
            u16 at = (u16)i;
            if (at >= ed->lines[line].length)
                break;
            gui_fill_rect(surface, gui_rect(bounds.x + 4 + i,
                                            y,
                                            4,
                                            1),
                          (ed->lines[line].text[at] == ' ' || ed->lines[line].text[at] == '\t')
                              ? GUI_RGB(0x33, 0x36, 0x41)
                              : GUI_RGB(0x6A, 0x70, 0x84));
        }
    }
    axide_paint_minimap_viewport(surface, ed, bounds);
}

static void axide_paint_tabstrip(gui_surface_t *surface, const axide_editor_t *ed,
                                 gui_rect_t bounds)
{
    i32 x = bounds.x;
    u16 i;
    gui_fill_rect(surface, bounds, AXIDE_C_BG_SOFT);
    for (i = 0; i < ed->tab_count; i++)
    {
        i32 width = gui_text_width(ed->tabs[i].title, 1) + 44;
        gui_rect_t tab = gui_rect(x, bounds.y, width, bounds.h);
        u32 colour = (i == ed->tab_active) ? AXIDE_C_BG : AXIDE_C_BG_SOFT;
        gui_fill_rect(surface, tab, colour);
        gui_fill_rect(surface, gui_rect(x, bounds.y, 1, bounds.h),
                      GUI_RGB(0x2B, 0x2D, 0x36));
        gui_text(surface, x + 14, bounds.y + 10, ed->tabs[i].title,
                 (i == ed->tab_active) ? AXIDE_C_TEXT : AXIDE_C_HINT, 1);
        if (ed->tabs[i].dirty)
            gui_fill_rect(surface, gui_rect(x + width - 30, bounds.y + 13, 6, 6),
                          AXIDE_C_ACCENT);
        gui_text(surface, x + width - 18, bounds.y + 9, "x", AXIDE_C_HINT, 1);
        x += width;
    }
    gui_text(surface, x + 16, bounds.y + 10, "+", AXIDE_C_HINT, 1);
    gui_fill_rect(surface, gui_rect(bounds.x, bounds.y + bounds.h - 1, bounds.w, 1),
                  GUI_RGB(0x2B, 0x2D, 0x36));
}

static void axide_paint_status(gui_surface_t *surface, const axide_editor_t *ed,
                               gui_rect_t bounds)
{
    char caret[32];
    u32 i = 0;
    u32 value = ed->caret_line + 1;
    u16 digits = 0;
    u16 tmp;
    gui_fill_rect(surface, bounds, GUI_RGB(0x59, 0xA8, 0xFF));
    gui_text(surface, bounds.x + 12, bounds.y + 7, ed->status, GUI_RGB(0x0F, 0x17, 0x24), 1);
    caret[0] = 'L';
    caret[1] = 'n';
    caret[2] = ' ';
    caret[3] = ' ';
    i = 4;
    tmp = 0;
    while (value >= 10)
    {
        value /= 10;
        tmp++;
    }
    digits = (u16)(tmp + 1);
    value = ed->caret_line + 1;
    for (tmp = digits; tmp > 0; tmp--)
    {
        caret[i + tmp - 1] = (char)('0' + (value % 10));
        value /= 10;
    }
    i = (u32)(i + digits);
    caret[i] = ' ';
    caret[i + 1] = 'C';
    caret[i + 2] = 'o';
    caret[i + 3] = 'l';
    caret[i + 4] = ' ';
    caret[i + 5] = 0;
    gui_text(surface, bounds.x + bounds.w - 220, bounds.y + 7, caret,
             GUI_RGB(0x0F, 0x17, 0x24), 1);
    gui_text(surface, bounds.x + bounds.w - 120, bounds.y + 7,
             ed->read_only ? "read only" : "editing", GUI_RGB(0x0F, 0x17, 0x24), 1);
}

static void axide_paint_caret(gui_surface_t *surface, const axide_editor_t *ed,
                              gui_rect_t bounds)
{
    i32 x = bounds.x + AXIDE_GUTTER_WIDTH + 8;
    i32 y = bounds.y + axide_line_pixel_y(ed, ed->caret_line);
    char prefix[AXIDE_MAX_COLS];
    u16 i;
    u16 limit = ed->caret_column;
    if (limit >= AXIDE_MAX_COLS)
        limit = AXIDE_MAX_COLS - 1;
    for (i = 0; i < limit; i++)
        prefix[i] = ed->lines[ed->caret_line].text[i];
    prefix[limit] = 0;
    x += gui_text_width(prefix, 1);
    if (y < bounds.y || y + AXIDE_LINE_HEIGHT > bounds.y + bounds.h)
        return;
    if ((ed->caret_blink % 60) < 30)
        gui_fill_rect(surface, gui_rect(x, y + 1, 2, AXIDE_LINE_HEIGHT - 3), AXIDE_C_CARET);
}

static void axide_paint_code(gui_surface_t *surface, const axide_editor_t *ed,
                             gui_rect_t bounds)
{
    u16 first = axide_line_at_pixel(ed, 0);
    u16 last = axide_line_at_pixel(ed, bounds.h);
    u16 line;
    u16 sel_l0, sel_c0, sel_l1, sel_c1;
    bool_t selection = !axide_selection_empty(ed);
    i32 origin_x = bounds.x + AXIDE_GUTTER_WIDTH + 8 - ed->scroll_x;
    if (selection)
        axide_selection_bounds(ed, &sel_l0, &sel_c0, &sel_l1, &sel_c1);
    else
    {
        sel_l0 = 0;
        sel_c0 = 0;
        sel_l1 = 0;
        sel_c1 = 0;
    }
    for (line = first; line <= last && line < ed->line_count; line++)
    {
        const char *text = ed->lines[line].text;
        u16 length = ed->lines[line].length;
        i32 y = bounds.y + axide_line_pixel_y(ed, line);
        i32 x = origin_x;
        u16 i = 0;
        bool_t in_comment = false;
        bool_t in_string = false;
        if (y + AXIDE_LINE_HEIGHT < bounds.y || y > bounds.y + bounds.h)
            continue;
        if (line == ed->caret_line)
            gui_fill_rect(surface, gui_rect(bounds.x + AXIDE_GUTTER_WIDTH, y,
                                            bounds.w - AXIDE_GUTTER_WIDTH, AXIDE_LINE_HEIGHT),
                          AXIDE_C_CURRENT);
        if (ed->show_whitespace)
            axide_paint_whitespace_marks(surface, ed, line, y, origin_x);
        axide_paint_indent_guides(surface, ed, line, y, origin_x);
        while (i < length)
        {
            u16 start = i;
            u8 token = AXIDE_TOK_PLAIN;
            u32 colour;
            if (i + 1 < length && text[i] == '/' && text[i + 1] == '/')
            {
                in_comment = true;
                i = length;
                token = AXIDE_TOK_COMMENT;
            }
            else if (text[i] == '"')
            {
                in_string = !in_string;
                i = (u16)(i + 1);
                while (i < length && text[i] != '"')
                    i++;
                if (i < length)
                    i = (u16)(i + 1);
                token = AXIDE_TOK_STRING;
            }
            else if (text[i] == '#' && start == 0)
            {
                i = length;
                token = AXIDE_TOK_PREPROC;
            }
            else if (axide_is_digit(text[i]))
            {
                while (i < length && (axide_is_ident_char(text[i]) || text[i] == '.'))
                    i++;
                token = AXIDE_TOK_NUMBER;
            }
            else if (axide_is_ident_start(text[i]))
            {
                while (i < length && axide_is_ident_char(text[i]))
                    i++;
                token = (in_comment || in_string) ? AXIDE_TOK_PLAIN
                                                  : axide_classify_word(&text[start], (u16)(i - start));
            }
            else
            {
                token = AXIDE_TOK_PUNCT;
                i++;
            }
            colour = (in_comment && token != AXIDE_TOK_COMMENT) ? AXIDE_C_COMMENT
                                                                : axide_token_colour(token);
            if (token == AXIDE_TOK_STRING && !in_comment)
                colour = AXIDE_C_STRING;
            if (in_comment)
                colour = AXIDE_C_COMMENT;
            {
                u16 run = (u16)(i - start);
                i32 width = 0;
                u16 k;
                char glyph[2];
                glyph[1] = 0;
                for (k = 0; k < run; k++)
                {
                    glyph[0] = text[start + k];
                    width += gui_text_width(glyph, 1);
                }
                if (selection && line >= sel_l0 && line <= sel_l1)
                {
                    u16 from = (line == sel_l0) ? sel_c0 : 0;
                    u16 to = (line == sel_l1) ? sel_c1 : length;
                    if (to > from)
                    {
                        i32 sel_x = origin_x;
                        u16 k2;
                        char measure[2];
                        measure[1] = 0;
                        for (k2 = 0; k2 < from; k2++)
                        {
                            measure[0] = text[k2];
                            sel_x += gui_text_width(measure, 1);
                        }
                        gui_fill_rect(surface, gui_rect(sel_x, y, 0, 0), 0);
                        gui_blend_rect(surface,
                                       gui_rect(sel_x, y, gui_text_width(text, 1), AXIDE_LINE_HEIGHT),
                                       GUI_ARGB(0x66, 0x33, 0x3F, 0x5C));
                    }
                }
                axide_paint_token_run(surface, x, y + 2, &text[start], run, colour, 1);
                x += width;
            }
        }
        if (length == 0 && line == ed->caret_line)
            gui_fill_rect(surface, gui_rect(x, y + 2, 2, AXIDE_LINE_HEIGHT - 4), AXIDE_C_CARET);
    }
}

/* ------------------------------------------------------------- public API */

void axide_init(axide_editor_t *ed)
{
    u16 i;
    u32 j;
    for (i = 0; i < AXIDE_MAX_LINES; i++)
        axide_zero_line(&ed->lines[i]);
    ed->line_count = 0;
    ed->caret_line = 0;
    ed->caret_column = 0;
    ed->anchor_line = 0;
    ed->anchor_column = 0;
    ed->has_selection = false;
    ed->scroll_y = 0;
    ed->scroll_x = 0;
    ed->view_height = 0;
    ed->view_width = 0;
    ed->read_only = false;
    ed->wrap = false;
    ed->show_minimap = true;
    ed->show_whitespace = false;
    ed->auto_indent = true;
    ed->desired_column = 0;
    ed->search_length = 0;
    ed->search_hit_line = 0;
    ed->search_hit_column = 0;
    ed->search_active = false;
    ed->caret_blink = 0;
    ed->tab_count = 0;
    ed->tab_active = 0;
    ed->undo_count = 0;
    ed->undo_top = 0;
    ed->dirty = false;
    ed->edits = 0;
    ed->keystrokes = 0;
    for (i = 0; i < AXIDE_MAX_SEARCH; i++)
        ed->search[i] = 0;
    for (j = 0; j < 20; j++)
        ed->status[j] = "Untitled - Aurora Code"[j];
    for (i = 20; i < 64; i++)
        ed->status[i] = 0;
    axide_add_tab(ed, "kernel.c", 0);
}

void axide_add_tab(axide_editor_t *ed, const char *title, u8 language)
{
    axide_tab_t *tab;
    u16 i;
    if (ed->tab_count >= AXIDE_MAX_TABS)
        return;
    tab = &ed->tabs[ed->tab_count];
    for (i = 0; i < 31 && title[i] != 0; i++)
        tab->title[i] = title[i];
    tab->title[i] = 0;
    tab->dirty = false;
    tab->language = language;
    tab->checksum = 0;
    ed->tab_count++;
    ed->tab_active = (u16)(ed->tab_count - 1);
}

void axide_load_text(axide_editor_t *ed, const char *text)
{
    u16 line = 0;
    u16 column = 0;
    u16 i = 0;
    axide_zero_line(&ed->lines[0]);
    ed->line_count = 1;
    while (text[i] != 0)
    {
        char c = text[i];
        if (c == '\n')
        {
            if (line + 1 >= AXIDE_MAX_LINES)
                break;
            line = (u16)(line + 1);
            axide_zero_line(&ed->lines[line]);
            ed->line_count = (u16)(line + 1);
            column = 0;
        }
        else if (c == '\t')
        {
            u16 k;
            for (k = 0; k < AXIDE_TAB_WIDTH && column < AXIDE_MAX_COLS - 1; k++)
            {
                ed->lines[line].text[column] = ' ';
                column++;
            }
        }
        else if (column < AXIDE_MAX_COLS - 1)
        {
            ed->lines[line].text[column] = c;
            column++;
        }
        i++;
    }
    for (line = 0; line < ed->line_count; line++)
    {
        ed->lines[line].length = 0;
        while (ed->lines[line].text[ed->lines[line].length] != 0 &&
               ed->lines[line].length < AXIDE_MAX_COLS - 1)
            ed->lines[line].length++;
        axide_recount_indent(&ed->lines[line]);
    }
    ed->caret_line = 0;
    ed->caret_column = 0;
    ed->scroll_y = 0;
    ed->scroll_x = 0;
    ed->dirty = false;
}

u16 axide_line_count(const axide_editor_t *ed)
{
    return ed->line_count;
}

u32 axide_total_chars(const axide_editor_t *ed)
{
    u32 total = 0;
    u16 i;
    for (i = 0; i < ed->line_count; i++)
        total += ed->lines[i].length;
    return total;
}

i32 axide_document_height(const axide_editor_t *ed)
{
    return ed->line_count * AXIDE_LINE_HEIGHT;
}

bool_t axide_key(axide_editor_t *ed, u16 key, bool_t shift, bool_t control)
{
    ed->keystrokes++;
    ed->caret_blink = 0;
    if (control)
    {
        switch (key)
        {
        case 'a':
        case 'A':
            axide_select_all(ed);
            return true;
        case 'z':
        case 'Z':
            axide_undo(ed);
            return true;
        case 's':
        case 'S':
            ed->dirty = false;
            gui_text(0, 0, 0, "", 0, 0);
            return true;
        default:
            break;
        }
    }
    switch (key)
    {
    case 0x08:
        axide_backspace(ed);
        return true;
    case 0x09:
        axide_insert_text(ed, "    ");
        return true;
    case 0x0A:
    case 0x0D:
        axide_newline(ed);
        return true;
    case 0x7F:
        axide_delete_forward(ed);
        return true;
    default:
        break;
    }
    if (key >= 0x20 && key < 0x7F)
    {
        char text[2];
        if (ed->read_only)
            return false;
        text[0] = (char)key;
        text[1] = 0;
        axide_insert_text(ed, text);
        return true;
    }
    return false;
}

bool_t axide_nav_key(axide_editor_t *ed, u16 key, bool_t shift)
{
    u16 old_line = ed->caret_line;
    u16 old_column = ed->caret_column;
    if (shift && !ed->has_selection)
    {
        ed->anchor_line = ed->caret_line;
        ed->anchor_column = ed->caret_column;
        ed->has_selection = true;
    }
    else if (!shift)
    {
        ed->has_selection = false;
    }
    switch (key)
    {
    case 0x01:
        if (ed->caret_line > 0)
            ed->caret_line--;
        break;
    case 0x02:
        if (ed->caret_line + 1 < ed->line_count)
            ed->caret_line++;
        break;
    case 0x03:
        if (ed->caret_column > 0)
            ed->caret_column--;
        break;
    case 0x04:
        if (ed->caret_column < ed->lines[ed->caret_line].length)
            ed->caret_column++;
        break;
    case 0x05:
        axide_move_home(ed);
        break;
    case 0x06:
        axide_move_end(ed);
        break;
    case 0x07:
        axide_move_word(ed, -1);
        break;
    case 0x08:
        axide_move_word(ed, 1);
        break;
    default:
        return false;
    }
    axide_clamp_caret(ed);
    if (!shift && ed->caret_line == old_line && ed->caret_column == old_column)
        ed->has_selection = false;
    ed->caret_blink = 0;
    axide_ensure_visible(ed);
    return true;
}

void axide_pointer_down(axide_editor_t *ed, i32 x, i32 y, bool_t shift)
{
    u16 line;
    if (y < 0)
        y = 0;
    line = axide_line_at_pixel(ed, y);
    ed->caret_line = line;
    ed->caret_column = axide_column_at_pixel(ed, x);
    if (shift)
        ed->has_selection = true;
    else
    {
        ed->anchor_line = ed->caret_line;
        ed->anchor_column = ed->caret_column;
        ed->has_selection = false;
    }
    ed->desired_column = ed->caret_column;
    ed->caret_blink = 0;
}

void axide_pointer_drag(axide_editor_t *ed, i32 x, i32 y)
{
    u16 line = axide_line_at_pixel(ed, y < 0 ? 0 : y);
    ed->caret_line = line;
    ed->caret_column = axide_column_at_pixel(ed, x);
    ed->has_selection = true;
    axide_ensure_visible(ed);
}

void axide_scroll(axide_editor_t *ed, i32 delta)
{
    ed->scroll_y += delta;
    axide_scroll_into_view(ed);
}

void axide_set_viewport(axide_editor_t *ed, i32 width, i32 height)
{
    ed->view_width = width;
    ed->view_height = height;
    axide_scroll_into_view(ed);
}

void axide_tick(axide_editor_t *ed)
{
    ed->caret_blink++;
}

u16 axide_goto_line(axide_editor_t *ed, u16 line)
{
    if (line >= ed->line_count)
        return 0;
    ed->caret_line = line;
    ed->caret_column = 0;
    ed->desired_column = 0;
    axide_ensure_visible(ed);
    return (u16)(line + 1);
}

void axide_set_search(axide_editor_t *ed, const char *needle)
{
    u16 i = 0;
    while (needle[i] != 0 && i < AXIDE_MAX_SEARCH - 1)
    {
        ed->search[i] = needle[i];
        i++;
    }
    ed->search[i] = 0;
    ed->search_length = i;
    ed->search_active = (bool_t)(i > 0);
}

u16 axide_search_next(axide_editor_t *ed)
{
    u16 hit;
    if (ed->search_length == 0)
        return 0;
    hit = axide_find(ed, (u16)(ed->caret_line + 1), ed->search);
    if (hit == 0)
        hit = axide_find(ed, 0, ed->search);
    return hit;
}

void axide_paint(axide_editor_t *ed, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t tabstrip;
    gui_rect_t code;
    gui_rect_t status;
    i32 gutter = AXIDE_GUTTER_WIDTH;
    i32 minimap = ed->show_minimap ? AXIDE_MINIMAP_WIDTH : 0;
    if (bounds.w <= 0 || bounds.h <= 0)
        return;
    gui_fill_rect(surface, bounds, AXIDE_C_BG);
    tabstrip = gui_rect(bounds.x, bounds.y, bounds.w, AXIDE_TABSTRIP_HEIGHT);
    axide_paint_tabstrip(surface, ed, tabstrip);
    status = gui_rect(bounds.x, bounds.y + bounds.h - AXIDE_STATUS_HEIGHT,
                      bounds.w, AXIDE_STATUS_HEIGHT);
    code = gui_rect(bounds.x + gutter, bounds.y + AXIDE_TABSTRIP_HEIGHT,
                    bounds.w - gutter - minimap,
                    bounds.h - AXIDE_TABSTRIP_HEIGHT - AXIDE_STATUS_HEIGHT);
    ed->view_height = code.h;
    ed->view_width = code.w;
    axide_paint_gutter(surface, ed, gui_rect(bounds.x, code.y, gutter, code.h));
    axide_paint_code(surface, ed, gui_rect(code.x, code.y, code.w, code.h));
    axide_paint_caret(surface, ed, gui_rect(code.x, code.y, code.w, code.h));
    if (ed->show_minimap)
        axide_paint_minimap(surface, ed,
                            gui_rect(bounds.x + bounds.w - minimap, code.y,
                                     minimap, code.h));
    axide_paint_status(surface, ed, status);
    if (ed->search_active)
    {
        char hint[64];
        u16 i;
        for (i = 0; i < 64; i++)
            hint[i] = 0;
        hint[0] = '/';
        for (i = 0; i < ed->search_length && i < 40; i++)
            hint[i + 1] = ed->search[i];
        gui_fill_rect(surface, gui_rect(code.x + 12, code.y + 8, 260, 26),
                      GUI_RGB(0x25, 0x28, 0x33));
        gui_rect_outline(surface, gui_rect(code.x + 12, code.y + 8, 260, 26),
                         AXIDE_C_ACCENT);
        gui_text(surface, code.x + 20, code.y + 14, hint, AXIDE_C_TEXT, 1);
    }
    axide_scroll_into_view(ed);
}

void axide_dump(const axide_editor_t *ed)
{
    u16 i;
    printk("axide: %u lines, caret %u:%u, %u edits, scroll %d\n",
           (unsigned)ed->line_count, (unsigned)ed->caret_line,
           (unsigned)ed->caret_column, (unsigned)ed->edits, (int)ed->scroll_y);
    for (i = 0; i < ed->tab_count; i++)
        printk("  tab %u: %s%s\n", (unsigned)i, ed->tabs[i].title,
               ed->tabs[i].dirty ? " *" : "");
}

bool_t axide_selftest(void)
{
    axide_editor_t ed;
    axide_init(&ed);
    axide_load_text(&ed, "int main(void)\n{\n    return 0;\n}\n");
    if (ed.line_count != 5)
        return false;
    axide_goto_line(&ed, 1);
    axide_key(&ed, 'x', false, false);
    if (ed.lines[1].text[0] != 'x')
        return false;
    axide_backspace(&ed);
    if (ed.lines[1].text[0] != '{')
        return false;
    axide_select_all(&ed);
    axide_delete_selection(&ed);
    if (ed.line_count != 1 || ed.lines[0].length != 0)
        return false;
    axide_load_text(&ed, "// comment\nint value = 42;\n");
    axide_set_search(&ed, "42");
    if (axide_search_next(&ed) != 2)
        return false;
    axide_toggle_comment(&ed);
    if (ed.lines[1].text[0] != '/')
        return false;
    return true;
}

