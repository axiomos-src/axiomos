/* AxiomOS - Aurora: document editor (rich text).
 *
 * A paragraph based word processor.  Text is stored as a flat character array
 * with a parallel style array, plus paragraph metadata (alignment, indent,
 * spacing, list level).  Formatting is applied to selections, which keeps the
 * model simple and the renderer cheap: every paragraph is laid out once per
 * frame into visual line records and then painted.
 */

#include "gui.h"

#define AXDOC_MAX_CHARS     8192
#define AXDOC_MAX_PARAS     512
#define AXDOC_MAX_LINES     2048
#define AXDOC_PAGE_WIDTH    720
#define AXDOC_PAGE_MARGIN   64
#define AXDOC_RULER_HEIGHT  28
#define AXDOC_TOOLBAR_HEIGHT 40
#define AXDOC_STATUS_HEIGHT 24
#define AXDOC_MAX_UNDO      32
#define AXDOC_MAX_FIND      48

#define AXDOC_C_DESK     GUI_RGB(0x20, 0x22, 0x28)
#define AXDOC_C_PAGE     GUI_RGB(0xF6, 0xF5, 0xF2)
#define AXDOC_C_INK      GUI_RGB(0x1E, 0x20, 0x26)
#define AXDOC_C_INK_SOFT GUI_RGB(0x5A, 0x5E, 0x68)
#define AXDOC_C_RULE     GUI_RGB(0xD8, 0xD6, 0xD0)
#define AXDOC_C_SEL      GUI_RGB(0x9F, 0xC4, 0xF2)
#define AXDOC_C_ACCENT   GUI_RGB(0x2E, 0x6C, 0xC0)
#define AXDOC_C_MARGIN   GUI_RGB(0x2A, 0x2C, 0x34)
#define AXDOC_C_HILITE   GUI_RGB(0xFF, 0xE7, 0x8A)

typedef struct axdoc_char
{
    char ch;
    u8   bold;
    u8   italic;
    u8   underline;
    u8   size;          /* 1 = small, 2 = body, 3 = large */
} axdoc_char_t;

typedef struct axdoc_para
{
    u32  start;         /* index into the character array */
    u32  length;
    u8   align;         /* 0 left, 1 centre, 2 right, 3 justify */
    u8   indent;        /* in units of 16 px */
    u8   list;          /* 0 none, 1 bullet, 2 numbered */
    u8   spacing;       /* extra pixels before the paragraph */
    u8   heading;       /* 0 body, 1 h1, 2 h2, 3 h3 */
} axdoc_para_t;

typedef struct axdoc_line
{
    u32  para;
    u32  first;         /* first character index inside the paragraph */
    u32  count;
    i32  y;
    i32  height;
} axdoc_line_t;

typedef struct axdoc_undo_entry
{
    u32  position;
    u32  length;
    u8   kind;          /* 0 insert, 1 delete, 2 format */
} axdoc_undo_entry_t;

typedef struct axdoc_doc
{
    axdoc_char_t chars[AXDOC_MAX_CHARS];
    u32          char_count;
    axdoc_para_t paras[AXDOC_MAX_PARAS];
    u32          para_count;
    axdoc_line_t lines[AXDOC_MAX_LINES];
    u32          line_count;
    u32          caret;
    u32          anchor;
    bool_t       selecting;
    i32          scroll_y;
    i32          zoom;       /* percent, 50..200 */
    u16          bold;
    u16          italic;
    u16          underline;
    u16          current_size;
    u16          alignment;
    bool_t       show_ruler;
    bool_t       show_margins;
    char         title[48];
    char         find[AXDOC_MAX_FIND];
    u16          find_length;
    u32          find_hit;
    void        *undo;
    axdoc_undo_entry_t undo_entries[AXDOC_MAX_UNDO];
    u16          undo_count;
    u16          undo_top;
    u32          edits;
    u32          words;
    u32          paragraphs_dirty;
} axdoc_doc_t;

static void axdoc_zero(void *ptr, u32 size);
static i32  axdoc_measure_scale(u16 size);
static i32  axdoc_measure(const char *text, u16 size);
static i32  axdoc_line_height(u16 size);
static u32  axdoc_para_at(const axdoc_doc_t *doc, u32 position);
static u32  axdoc_para_start(const axdoc_doc_t *doc, u16 para);
static u32  axdoc_para_end(const axdoc_doc_t *doc, u16 para);
static void axdoc_reindex(axdoc_doc_t *doc);
static void axdoc_count_words(axdoc_doc_t *doc);
static void axdoc_reflow(axdoc_doc_t *doc);
static void axdoc_push_undo(axdoc_doc_t *doc, u32 position, u32 length, u8 kind);
static void axdoc_undo(axdoc_doc_t *doc);
static void axdoc_insert(axdoc_doc_t *doc, const char *text);
static void axdoc_split_paragraph(axdoc_doc_t *doc);
static void axdoc_delete_selection(axdoc_doc_t *doc);
static void axdoc_backspace(axdoc_doc_t *doc);
static void axdoc_delete_forward(axdoc_doc_t *doc);
static void axdoc_apply_style(axdoc_doc_t *doc, u8 bold, u8 italic, u8 underline);
static void axdoc_set_alignment(axdoc_doc_t *doc, u8 alignment);
static void axdoc_toggle_list(axdoc_doc_t *doc);
static void axdoc_set_heading(axdoc_doc_t *doc, u8 heading);
static void axdoc_goto(axdoc_doc_t *doc, u32 position);
static void axdoc_move(axdoc_doc_t *doc, i32 delta, bool_t extend);
static u32  axdoc_position_from_point(const axdoc_doc_t *doc, i32 x, i32 y);
static void axdoc_ensure_visible(axdoc_doc_t *doc, i32 view_height);
static void axdoc_scroll_into_view(axdoc_doc_t *doc, i32 view_height);
static void axdoc_paint_page(gui_surface_t *surface, axdoc_doc_t *doc,
                             gui_rect_t page, i32 origin_y);
static void axdoc_paint_ruler(gui_surface_t *surface, const axdoc_doc_t *doc,
                              gui_rect_t ruler, i32 page_x);
static void axdoc_paint_toolbar(gui_surface_t *surface, const axdoc_doc_t *doc,
                                gui_rect_t bar);
static void axdoc_paint_status(gui_surface_t *surface, const axdoc_doc_t *doc,
                               gui_rect_t bar);
static void axdoc_paint_paragraph(gui_surface_t *surface, const axdoc_doc_t *doc,
                                  const axdoc_line_t *line, i32 x, i32 y,
                                  i32 width);
static void axdoc_paint_list_marker(gui_surface_t *surface,
                                    const axdoc_doc_t *doc, u16 para,
                                    i32 x, i32 y, i32 number);
static i32  axdoc_line_width(const axdoc_doc_t *doc, const axdoc_line_t *line);
static i32  axdoc_align_offset(const axdoc_doc_t *doc, const axdoc_line_t *line,
                               i32 width);
static void axdoc_paint_caret(gui_surface_t *surface, const axdoc_doc_t *doc,
                              i32 page_x, i32 origin_y);
static u32  axdoc_word_at(const axdoc_doc_t *doc, u32 position);

static void axdoc_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static i32 axdoc_measure_scale(u16 size)
{
    if (size >= 4)
        return 3;
    if (size == 3)
        return 2;
    if (size == 0)
        return 1;
    return 1;
}

static i32 axdoc_measure(const char *text, u16 size)
{
    return gui_text_width(text, (u8)axdoc_measure_scale(size));
}

static i32 axdoc_line_height(u16 size)
{
    i32 scale = axdoc_measure_scale(size);
    i32 base = gui_text_width("X", 1);
    AXIOM_UNUSED_VAR(base);
    if (scale >= 3)
        return 46;
    if (scale == 2)
        return 30;
    return 18;
}

static u32 axdoc_para_start(const axdoc_doc_t *doc, u16 para)
{
    if (para >= doc->para_count)
        return doc->char_count;
    return doc->paras[para].start;
}

static u32 axdoc_para_end(const axdoc_doc_t *doc, u16 para)
{
    if (para >= doc->para_count)
        return doc->char_count;
    return doc->paras[para].start + doc->paras[para].length;
}

static u32 axdoc_para_at(const axdoc_doc_t *doc, u32 position)
{
    u16 i;
    for (i = 0; i < doc->para_count; i++)
    {
        if (position >= doc->paras[i].start &&
            position <= doc->paras[i].start + doc->paras[i].length)
            return i;
    }
    return doc->para_count == 0 ? 0 : (u32)(doc->para_count - 1);
}

static void axdoc_reindex(axdoc_doc_t *doc)
{
    u32 i = 0;
    u16 para = 0;
    doc->para_count = 0;
    if (doc->char_count == 0)
    {
        doc->para_count = 1;
        doc->paras[0].start = 0;
        doc->paras[0].length = 0;
        doc->paras[0].align = 0;
        doc->paras[0].heading = 0;
        doc->paras[0].list = 0;
        doc->paras[0].indent = 0;
        doc->paras[0].spacing = 0;
        return;
    }
    while (para < AXDOC_MAX_PARAS)
    {
        doc->paras[para].start = i;
        while (i < doc->char_count && doc->chars[i].ch != '\n')
            i++;
        doc->paras[para].length = i - doc->paras[para].start;
        if (doc->paras[para].align > 3)
            doc->paras[para].align = 0;
        para++;
        if (i >= doc->char_count)
            break;
        i++;
    }
    doc->para_count = para;
}

static void axdoc_count_words(axdoc_doc_t *doc)
{
    u32 i;
    u32 words = 0;
    bool_t in_word = false;
    for (i = 0; i < doc->char_count; i++)
    {
        char c = doc->chars[i].ch;
        bool_t separator = (bool_t)(c == ' ' || c == '\n' || c == '\t' ||
                                    c == '.' || c == ',' || c == ';');
        if (!separator && !in_word)
        {
            words++;
            in_word = true;
        }
        else if (separator)
        {
            in_word = false;
        }
    }
    doc->words = words;
}

static void axdoc_reflow(axdoc_doc_t *doc)
{
    u16 para;
    u32 line = 0;
    u32 i;
    i32 usable = AXDOC_PAGE_WIDTH - AXDOC_PAGE_MARGIN * 2;
    doc->line_count = 0;
    for (para = 0; para < doc->para_count; para++)
    {
        axdoc_para_t *p = &doc->paras[para];
        u32 start = p->start;
        u32 remaining = p->length;
        u16 size = p->heading >= 1 ? 4 : doc->current_size;
        if (p->heading == 2)
            size = 3;
        else if (p->heading == 3)
            size = 2;
        usable = AXDOC_PAGE_WIDTH - AXDOC_PAGE_MARGIN * 2 - p->indent * 16;
        if (usable < 160)
            usable = 160;
        while (remaining > 0 || (remaining == 0 && p->length == 0 && line < AXDOC_MAX_LINES))
        {
            u32 fit = 0;
            i32 width = 0;
            while (fit < remaining)
            {
                char glyph[2];
                i32 advance;
                if (doc->chars[start + fit].ch == '\t')
                    advance = 24;
                else
                {
                    glyph[0] = doc->chars[start + fit].ch;
                    glyph[1] = 0;
                    advance = axdoc_measure(glyph, doc->chars[start + fit].size) + 1;
                }
                if (width + advance > usable && fit > 0)
                    break;
                width += advance;
                fit++;
            }
            if (line < AXDOC_MAX_LINES)
            {
                doc->lines[line].para = para;
                doc->lines[line].first = (u32)(start - p->start + (p->length - remaining));
                doc->lines[line].count = fit;
                doc->lines[line].height = axdoc_line_height(size);
                line++;
            }
            start += fit;
            remaining -= fit;
            if (fit == 0)
                break;
        }
    }
    doc->line_count = line;
    i = 0;
    AXIOM_UNUSED_VAR(i);
}

/* ------------------------------------------------------------------ undo */

static void axdoc_push_undo(axdoc_doc_t *doc, u32 position, u32 length, u8 kind)
{
    axdoc_undo_entry_t *entry = &doc->undo_entries[doc->undo_top];
    entry->position = position;
    entry->length = length;
    entry->kind = kind;
    doc->undo_top = (u16)((doc->undo_top + 1) % AXDOC_MAX_UNDO);
    if (doc->undo_count < AXDOC_MAX_UNDO)
        doc->undo_count++;
}

static void axdoc_undo(axdoc_doc_t *doc)
{
    axdoc_undo_entry_t entry;
    u32 i;
    if (doc->undo_count == 0)
        return;
    doc->undo_top = (u16)((doc->undo_top + AXDOC_MAX_UNDO - 1) % AXDOC_MAX_UNDO);
    entry = doc->undo_entries[doc->undo_top];
    doc->undo_count--;
    if (entry.kind == 0 && entry.position < doc->char_count)
    {
        doc->char_count = (u32)(doc->char_count - entry.length);
    }
    else if (entry.kind == 1 && doc->char_count < AXDOC_MAX_CHARS)
    {
        doc->char_count = (u32)(doc->char_count + entry.length);
    }
    if (entry.position <= doc->char_count)
        doc->caret = entry.position;
    else
        doc->caret = doc->char_count;
    doc->anchor = doc->caret;
    doc->selecting = false;
    for (i = 0; i < doc->char_count; i++)
        doc->chars[i].ch = doc->chars[i].ch;
    axdoc_reindex(doc);
    axdoc_reflow(doc);
    axdoc_count_words(doc);
    doc->edits++;
}

/* -------------------------------------------------------------- mutation */

static void axdoc_insert(axdoc_doc_t *doc, const char *text)
{
    u32 i = 0;
    u32 count = 0;
    u32 tail;
    if (doc->selecting && doc->anchor != doc->caret)
        axdoc_delete_selection(doc);
    while (text[count] != 0)
        count++;
    if (doc->char_count + count >= AXDOC_MAX_CHARS)
        return;
    tail = (u32)(doc->char_count - doc->caret);
    for (i = tail; i > 0; i--)
        doc->chars[doc->caret + count + i - 1] = doc->chars[doc->caret + i - 1];
    for (i = 0; i < count; i++)
    {
        axdoc_char_t c;
        c.ch = text[i];
        c.bold = (u8)doc->bold;
        c.italic = (u8)doc->italic;
        c.underline = (u8)doc->underline;
        c.size = (u8)doc->current_size;
        doc->chars[doc->caret + i] = c;
    }
    doc->char_count += count;
    doc->caret += count;
    doc->anchor = doc->caret;
    axdoc_push_undo(doc, doc->caret, count, 0);
    axdoc_reindex(doc);
    axdoc_reflow(doc);
    axdoc_count_words(doc);
    doc->edits++;
}

static void axdoc_split_paragraph(axdoc_doc_t *doc)
{
    axdoc_insert(doc, "\n");
}

static void axdoc_delete_selection(axdoc_doc_t *doc)
{
    u32 from;
    u32 to;
    u32 i;
    if (!doc->selecting || doc->anchor == doc->caret)
    {
        doc->selecting = false;
        return;
    }
    from = doc->anchor < doc->caret ? doc->anchor : doc->caret;
    to = doc->anchor < doc->caret ? doc->caret : doc->anchor;
    axdoc_push_undo(doc, from, to - from, 1);
    for (i = from; i + (to - from) < doc->char_count; i++)
        doc->chars[i] = doc->chars[i + (to - from)];
    doc->char_count = (u32)(doc->char_count - (to - from));
    doc->caret = from;
    doc->anchor = from;
    doc->selecting = false;
    axdoc_reindex(doc);
    axdoc_reflow(doc);
    axdoc_count_words(doc);
    doc->edits++;
}

static void axdoc_backspace(axdoc_doc_t *doc)
{
    u32 i;
    if (doc->selecting && doc->anchor != doc->caret)
    {
        axdoc_delete_selection(doc);
        return;
    }
    if (doc->caret == 0)
        return;
    axdoc_push_undo(doc, (u32)(doc->caret - 1), 1, 1);
    for (i = (u32)(doc->caret - 1); i + 1 < doc->char_count; i++)
        doc->chars[i] = doc->chars[i + 1];
    doc->char_count--;
    doc->caret--;
    axdoc_reindex(doc);
    axdoc_reflow(doc);
    axdoc_count_words(doc);
    doc->edits++;
}

static void axdoc_delete_forward(axdoc_doc_t *doc)
{
    u32 i;
    if (doc->selecting && doc->anchor != doc->caret)
    {
        axdoc_delete_selection(doc);
        return;
    }
    if (doc->caret >= doc->char_count)
        return;
    axdoc_push_undo(doc, doc->caret, 1, 1);
    for (i = doc->caret; i + 1 < doc->char_count; i++)
        doc->chars[i] = doc->chars[i + 1];
    doc->char_count--;
    axdoc_reindex(doc);
    axdoc_reflow(doc);
    axdoc_count_words(doc);
    doc->edits++;
}

static void axdoc_apply_style(axdoc_doc_t *doc, u8 bold, u8 italic, u8 underline)
{
    u32 from;
    u32 to;
    u32 i;
    if (doc->selecting && doc->anchor != doc->caret)
    {
        from = doc->anchor < doc->caret ? doc->anchor : doc->caret;
        to = doc->anchor < doc->caret ? doc->caret : doc->anchor;
    }
    else
    {
        from = doc->caret;
        to = doc->caret + 1;
    }
    if (to > doc->char_count)
        to = doc->char_count;
    for (i = from; i < to; i++)
    {
        doc->chars[i].bold = bold;
        doc->chars[i].italic = italic;
        doc->chars[i].underline = underline;
    }
    axdoc_push_undo(doc, from, to - from, 2);
    axdoc_reflow(doc);
    doc->edits++;
}

static void axdoc_set_alignment(axdoc_doc_t *doc, u8 alignment)
{
    u16 para = (u16)axdoc_para_at(doc, doc->caret);
    if (para >= doc->para_count)
        return;
    doc->paras[para].align = alignment;
    doc->alignment = alignment;
    axdoc_reflow(doc);
    doc->edits++;
}

static void axdoc_toggle_list(axdoc_doc_t *doc)
{
    u16 para = (u16)axdoc_para_at(doc, doc->caret);
    u16 i;
    u16 from = para;
    u16 to = para;
    if (doc->selecting && doc->anchor != doc->caret)
    {
        u16 anchor_para = (u16)axdoc_para_at(doc, doc->anchor);
        from = anchor_para < para ? anchor_para : para;
        to = anchor_para < para ? para : anchor_para;
    }
    for (i = from; i <= to && i < doc->para_count; i++)
    {
        if (doc->paras[i].list == 0)
            doc->paras[i].list = 1;
        else if (doc->paras[i].list == 1)
            doc->paras[i].list = 2;
        else
            doc->paras[i].list = 0;
    }
    axdoc_reflow(doc);
    doc->edits++;
}

static void axdoc_set_heading(axdoc_doc_t *doc, u8 heading)
{
    u16 para = (u16)axdoc_para_at(doc, doc->caret);
    if (para >= doc->para_count)
        return;
    doc->paras[para].heading = heading;
    doc->paras[para].spacing = heading ? 12 : 4;
    axdoc_reflow(doc);
    doc->edits++;
}

/* ------------------------------------------------------------------ caret */

static void axdoc_goto(axdoc_doc_t *doc, u32 position)
{
    if (position > doc->char_count)
        position = doc->char_count;
    doc->caret = position;
    doc->anchor = position;
    doc->selecting = false;
}

static void axdoc_move(axdoc_doc_t *doc, i32 delta, bool_t extend)
{
    u32 target;
    if (!extend)
    {
        doc->anchor = doc->caret;
        doc->selecting = false;
    }
    else if (!doc->selecting)
    {
        doc->anchor = doc->caret;
        doc->selecting = true;
    }
    if (delta < 0 && doc->caret >= (u32)(-delta))
        target = doc->caret + (u32)delta;
    else if (delta > 0 && doc->caret + (u32)delta <= doc->char_count)
        target = doc->caret + (u32)delta;
    else
        target = doc->caret;
    doc->caret = target;
}

static u32 axdoc_position_from_point(const axdoc_doc_t *doc, i32 x, i32 y)
{
    u32 line;
    u32 i;
    i32 cursor_x;
    i32 cursor_y = 0;
    for (line = 0; line < doc->line_count; line++)
    {
        i32 height = doc->lines[line].height;
        if (y >= cursor_y && y < cursor_y + height)
        {
            const axdoc_para_t *p = &doc->paras[doc->lines[line].para];
            u32 first = doc->lines[line].first;
            u32 count = doc->lines[line].count;
            i32 origin = AXDOC_PAGE_MARGIN + p->indent * 16;
            cursor_x = origin;
            for (i = 0; i < count; i++)
            {
                char glyph[2];
                i32 advance;
                u32 index = p->start + first + i;
                if (index >= doc->char_count)
                    return doc->char_count;
                glyph[0] = doc->chars[index].ch;
                glyph[1] = 0;
                advance = axdoc_measure(glyph, doc->chars[index].size) + 1;
                if (x < cursor_x + advance / 2)
                    return index;
                cursor_x += advance;
            }
            return p->start + first + count;
        }
        cursor_y += height;
    }
    return doc->char_count;
}

static void axdoc_scroll_into_view(axdoc_doc_t *doc, i32 view_height)
{
    if (doc->scroll_y < 0)
        doc->scroll_y = 0;
    {
        i32 total = 0;
        u32 i;
        for (i = 0; i < doc->line_count; i++)
            total += doc->lines[i].height;
        total += 120;
        if (doc->scroll_y + view_height > total)
            doc->scroll_y = total - view_height;
        if (doc->scroll_y < 0)
            doc->scroll_y = 0;
    }
}

static void axdoc_ensure_visible(axdoc_doc_t *doc, i32 view_height)
{
    u32 line;
    u32 i;
    i32 top = 0;
    if (doc->line_count == 0)
        return;
    line = doc->line_count - 1;
    for (i = 0; i < doc->line_count; i++)
    {
        if (doc->caret >= doc->paras[doc->lines[i].para].start &&
            doc->caret <= doc->paras[doc->lines[i].para].start +
                          doc->paras[doc->lines[i].para].length)
            line = i;
    }
    for (i = 0; i < line; i++)
        top += doc->lines[i].height;
    if (top < doc->scroll_y)
        doc->scroll_y = top;
    if (top + doc->lines[line].height > doc->scroll_y + view_height)
        doc->scroll_y = top + doc->lines[line].height - view_height;
    axdoc_scroll_into_view(doc, view_height);
}

/* --------------------------------------------------------------- rendering */

static i32 axdoc_line_width(const axdoc_doc_t *doc, const axdoc_line_t *line)
{
    const axdoc_para_t *p = &doc->paras[line->para];
    u32 i;
    i32 width = 0;
    for (i = 0; i < line->count; i++)
    {
        u32 index = p->start + line->first + i;
        char glyph[2];
        if (index >= doc->char_count)
            break;
        glyph[0] = doc->chars[index].ch;
        glyph[1] = 0;
        width += axdoc_measure(glyph, doc->chars[index].size) + 1;
    }
    return width;
}

static i32 axdoc_align_offset(const axdoc_doc_t *doc, const axdoc_line_t *line,
                              i32 width)
{
    const axdoc_para_t *p = &doc->paras[line->para];
    i32 line_width = axdoc_line_width(doc, line);
    if (p->align == 1)
        return (width - line_width) / 2;
    if (p->align == 2)
        return width - line_width;
    return 0;
}

static void axdoc_paint_list_marker(gui_surface_t *surface,
                                    const axdoc_doc_t *doc, u16 para,
                                    i32 x, i32 y, i32 number)
{
    const axdoc_para_t *p = &doc->paras[para];
    if (p->list == 1)
    {
        gui_fill_rect(surface, gui_rect(x - 14, y + 6, 5, 5), AXDOC_C_INK);
        return;
    }
    if (p->list == 2)
    {
        char label[8];
        i32 digits = number >= 10 ? 2 : 1;
        label[0] = (char)('0' + (number / 10) % 10);
        label[1] = (char)('0' + number % 10);
        label[2] = '.';
        label[3] = 0;
        if (digits == 1)
        {
            label[0] = (char)('0' + number % 10);
            label[1] = '.';
            label[2] = 0;
        }
        gui_text(surface, x - 22, y, label, AXDOC_C_INK, 1);
    }
}

static void axdoc_paint_paragraph_preview(void)
{
    /* nothing yet: kept for the print preview pass */
}

static void axdoc_paint_paragraph(gui_surface_t *surface, const axdoc_doc_t *doc,
                                  const axdoc_line_t *line, i32 x, i32 y,
                                  i32 width)
{
    const axdoc_para_t *p = &doc->paras[line->para];
    u32 i;
    i32 cursor = x + axdoc_align_offset(doc, line, width);
    for (i = 0; i < line->count; i++)
    {
        u32 index = p->start + line->first + i;
        char glyph[2];
        axdoc_char_t c;
        u32 colour;
        i32 advance;
        if (index >= doc->char_count)
            break;
        c = doc->chars[index];
        glyph[0] = c.ch;
        glyph[1] = 0;
        colour = AXDOC_C_INK;
        if (doc->selecting && doc->anchor != doc->caret)
        {
            u32 from = doc->anchor < doc->caret ? doc->anchor : doc->caret;
            u32 to = doc->anchor < doc->caret ? doc->caret : doc->anchor;
            if (index >= from && index < to)
            {
                advance = axdoc_measure(glyph, c.size) + 1;
                gui_fill_rect(surface, gui_rect(cursor, y, advance,
                                                axdoc_line_height(c.size)),
                              AXDOC_C_SEL);
            }
        }
        gui_text(surface, cursor, y, glyph, colour, (u8)axdoc_measure_scale(c.size));
        if (c.bold)
            gui_text(surface, cursor + 1, y, glyph, colour,
                     (u8)axdoc_measure_scale(c.size));
        if (c.underline)
            gui_hline(surface, cursor, cursor + axdoc_measure(glyph, c.size),
                      y + axdoc_line_height(c.size) - 2, AXDOC_C_INK);
        if (c.italic)
            gui_vline(surface, cursor + 1, y, y + axdoc_line_height(c.size) - 3,
                      GUI_ARGB(0x30, 0x1E, 0x20, 0x26));
        cursor += axdoc_measure(glyph, c.size) + 1;
    }
    AXIOM_UNUSED_VAR(axdoc_paint_paragraph_preview);
}

static void axdoc_paint_caret(gui_surface_t *surface, const axdoc_doc_t *doc,
                              i32 page_x, i32 origin_y)
{
    u32 line;
    u32 i;
    i32 top = 0;
    for (line = 0; line < doc->line_count; line++)
    {
        const axdoc_para_t *p = &doc->paras[doc->lines[line].para];
        if (doc->caret >= p->start && doc->caret <= p->start + p->length)
        {
            i32 cursor = page_x + AXDOC_PAGE_MARGIN + p->indent * 16;
            cursor += axdoc_align_offset(doc, &doc->lines[line],
                                         AXDOC_PAGE_WIDTH - AXDOC_PAGE_MARGIN * 2);
            for (i = 0; i < doc->lines[line].first; i++)
            {
                u32 index = p->start + i;
                char glyph[2];
                if (index >= doc->char_count)
                    break;
                glyph[0] = doc->chars[index].ch;
                glyph[1] = 0;
                cursor += axdoc_measure(glyph, doc->chars[index].size) + 1;
            }
            for (i = doc->lines[line].first; i < doc->caret - p->start; i++)
            {
                u32 index = p->start + i;
                char glyph[2];
                if (index >= doc->char_count)
                    break;
                glyph[0] = doc->chars[index].ch;
                glyph[1] = 0;
                cursor += axdoc_measure(glyph, doc->chars[index].size) + 1;
            }
            gui_fill_rect(surface,
                          gui_rect(cursor, origin_y + top + 2, 2,
                                   doc->lines[line].height - 4),
                          AXDOC_C_ACCENT);
            return;
        }
        top += doc->lines[line].height;
    }
}

static void axdoc_paint_page(gui_surface_t *surface, axdoc_doc_t *doc,
                             gui_rect_t page, i32 origin_y)
{
    u32 line;
    i32 y = page.y + AXDOC_PAGE_MARGIN - doc->scroll_y;
    i32 width = AXDOC_PAGE_WIDTH - AXDOC_PAGE_MARGIN * 2;
    u16 para_number = 0;
    gui_shadow(surface, page, 18, 0x55);
    gui_fill_rect(surface, page, AXDOC_C_PAGE);
    for (line = 0; line < doc->line_count; line++)
    {
        const axdoc_para_t *p = &doc->paras[doc->lines[line].para];
        if (doc->lines[line].first == 0)
        {
            y += p->spacing;
            if (p->list == 2)
                para_number++;
            else
                para_number = 0;
            if (doc->lines[line].first == 0)
            {
                i32 marker_x = page.x + AXDOC_PAGE_MARGIN + p->indent * 16 - 24;
                if (p->list != 0)
                    axdoc_paint_list_marker(surface, doc, doc->lines[line].para,
                                            marker_x, y, para_number);
            }
        }
        if (y + doc->lines[line].height > page.y && y < page.y + page.h)
        {
            axdoc_paint_paragraph(surface, doc, &doc->lines[line],
                                  page.x + AXDOC_PAGE_MARGIN + p->indent * 16,
                                  y, width - p->indent * 16);
        }
        y += doc->lines[line].height;
    }
    if (doc->show_margins)
    {
        gui_vline(surface, page.x + AXDOC_PAGE_MARGIN - 8, page.y,
                  page.y + page.h, AXDOC_C_RULE);
        gui_vline(surface, page.x + AXDOC_PAGE_WIDTH - AXDOC_PAGE_MARGIN + 8,
                  page.y, page.y + page.h, AXDOC_C_RULE);
    }
    gui_text(surface, page.x + AXDOC_PAGE_WIDTH / 2 - 12,
             page.y + page.h - 32, "- 1 -", AXDOC_C_INK_SOFT, 1);
    axdoc_paint_caret(surface, doc, page.x, page.y + AXDOC_PAGE_MARGIN - doc->scroll_y);
    AXIOM_UNUSED_VAR(origin_y);
}

static void axdoc_paint_ruler(gui_surface_t *surface, const axdoc_doc_t *doc,
                              gui_rect_t ruler, i32 page_x)
{
    i32 i;
    gui_fill_rect(surface, ruler, AXDOC_C_DESK);
    gui_hline(surface, ruler.x, ruler.x + ruler.w, ruler.y + ruler.h - 1,
              GUI_RGB(0x33, 0x36, 0x3E));
    for (i = 0; i <= AXDOC_PAGE_WIDTH; i += 40)
    {
        i32 x = page_x + i;
        u32 colour = (i % 80 == 0) ? AXDOC_C_INK_SOFT : GUI_RGB(0x4A, 0x4E, 0x58);
        gui_vline(surface, x, ruler.y + ruler.h - 8, ruler.y + ruler.h - 1, colour);
    }
    gui_fill_rect(surface, gui_rect(page_x + AXDOC_PAGE_MARGIN, ruler.y + 6,
                                    3, ruler.h - 8),
                  AXDOC_C_ACCENT);
    gui_fill_rect(surface, gui_rect(page_x + AXDOC_PAGE_WIDTH - AXDOC_PAGE_MARGIN,
                                    ruler.y + 6, 3, ruler.h - 8),
                  AXDOC_C_ACCENT);
    AXIOM_UNUSED_VAR(doc);
}

static void axdoc_paint_toolbar(gui_surface_t *surface, const axdoc_doc_t *doc,
                                gui_rect_t bar)
{
    i32 x = bar.x + 12;
    gui_fill_rect(surface, bar, GUI_RGB(0x2A, 0x2C, 0x34));
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y + bar.h - 1,
              GUI_RGB(0x14, 0x16, 0x1C));
    {
        const char *labels[8] = { "B", "I", "U", "L", "<", ">", "H1", "H2" };
        u16 i;
        for (i = 0; i < 8; i++)
        {
            i32 width = 30;
            gui_rect_t button = gui_rect(x, bar.y + 6, width, bar.h - 12);
            u32 colour = GUI_RGB(0x35, 0x38, 0x42);
            if (i == 0 && doc->bold)
                colour = AXDOC_C_ACCENT;
            if (i == 1 && doc->italic)
                colour = AXDOC_C_ACCENT;
            if (i == 2 && doc->underline)
                colour = AXDOC_C_ACCENT;
            if (i == 3 && doc->paras[axdoc_para_at(doc, doc->caret)].list != 0)
                colour = AXDOC_C_ACCENT;
            gui_fill_round_rect(surface, button, 4, colour);
            gui_text(surface, x + 11, bar.y + 12, labels[i], GUI_RGB(0xE6, 0xE8, 0xEE), 1);
            x += width + 4;
        }
    }
    gui_vline(surface, x + 4, bar.y + 8, bar.y + bar.h - 8, GUI_RGB(0x3C, 0x40, 0x4A));
    x += 12;
    {
        char zoom[16];
        i32 value = doc->zoom;
        zoom[0] = (char)('0' + (value / 100) % 10);
        zoom[1] = '0';
        zoom[2] = (char)('0' + (value / 10) % 10);
        zoom[3] = (char)('0' + value % 10);
        zoom[4] = '%';
        zoom[5] = 0;
        if (value < 100)
        {
            zoom[0] = '0';
            zoom[1] = (char)('0' + (value / 10) % 10);
            zoom[2] = (char)('0' + value % 10);
            zoom[3] = '%';
            zoom[4] = 0;
        }
        gui_text(surface, x, bar.y + 12, zoom, GUI_RGB(0xC8, 0xCC, 0xD8), 1);
    }
}

static void axdoc_paint_status(gui_surface_t *surface, const axdoc_doc_t *doc,
                               gui_rect_t bar)
{
    char buffer[64];
    u32 i = 0;
    u32 value;
    u16 digits;
    u32 words = doc->words;
    gui_fill_rect(surface, bar, GUI_RGB(0x26, 0x28, 0x30));
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y, GUI_RGB(0x3C, 0x40, 0x4A));
    for (i = 0; i < 18; i++)
        buffer[i] = doc->title[i];
    buffer[18] = 0;
    gui_text(surface, bar.x + 12, bar.y + 7, buffer, GUI_RGB(0xA8, 0xAE, 0xBC), 1);
    buffer[0] = 'W';
    buffer[1] = 'o';
    buffer[2] = 'r';
    buffer[3] = 'd';
    buffer[4] = 's';
    buffer[5] = ' ';
    buffer[6] = ' ';
    i = 7;
    digits = 1;
    value = words;
    while (value >= 10)
    {
        value /= 10;
        digits++;
    }
    value = words;
    {
        u16 k;
        for (k = 0; k < digits; k++)
        {
            buffer[i + digits - k - 1] = (char)('0' + (value % 10));
            value /= 10;
        }
        i += digits;
    }
    buffer[i] = 0;
    gui_text(surface, bar.x + bar.w - 120, bar.y + 7, buffer,
             GUI_RGB(0xA8, 0xAE, 0xBC), 1);
}

/* -------------------------------------------------------------- public API */

void axdoc_init(axdoc_doc_t *doc, const char *title)
{
    u32 i = 0;
    axdoc_zero(doc, (u32)sizeof(*doc));
    doc->char_count = 0;
    doc->para_count = 1;
    doc->paras[0].start = 0;
    doc->paras[0].length = 0;
    doc->paras[0].align = 0;
    doc->paras[0].indent = 0;
    doc->paras[0].list = 0;
    doc->paras[0].spacing = 4;
    doc->paras[0].heading = 0;
    doc->zoom = 100;
    doc->current_size = 2;
    doc->bold = 0;
    doc->italic = 0;
    doc->underline = 0;
    doc->show_ruler = true;
    doc->show_margins = true;
    doc->undo = NULL_PTR;
    while (title[i] != 0 && i < 47)
    {
        doc->title[i] = title[i];
        i++;
    }
    doc->title[i] = 0;
    axdoc_reflow(doc);
}

void axdoc_load(axdoc_doc_t *doc, const char *text)
{
    u32 i = 0;
    doc->char_count = 0;
    doc->caret = 0;
    doc->anchor = 0;
    doc->selecting = false;
    while (text[i] != 0 && doc->char_count < AXDOC_MAX_CHARS - 1)
    {
        axdoc_char_t c;
        c.ch = text[i];
        c.bold = 0;
        c.italic = 0;
        c.underline = 0;
        c.size = 2;
        doc->chars[doc->char_count] = c;
        doc->char_count++;
        i++;
    }
    axdoc_reindex(doc);
    axdoc_reflow(doc);
    axdoc_count_words(doc);
}

void axdoc_set_zoom(axdoc_doc_t *doc, i32 zoom)
{
    if (zoom < 50)
        zoom = 50;
    if (zoom > 200)
        zoom = 200;
    doc->zoom = zoom;
}

void axdoc_set_find(axdoc_doc_t *doc, const char *needle)
{
    u16 i = 0;
    while (needle[i] != 0 && i < AXDOC_MAX_FIND - 1)
    {
        doc->find[i] = needle[i];
        i++;
    }
    doc->find[i] = 0;
    doc->find_length = i;
}

u32 axdoc_find_next(axdoc_doc_t *doc)
{
    u32 i;
    u32 j;
    if (doc->find_length == 0)
        return 0;
    for (i = doc->caret + 1; i + doc->find_length <= doc->char_count; i++)
    {
        bool_t match = true;
        for (j = 0; j < doc->find_length; j++)
        {
            if (doc->chars[i + j].ch != doc->find[j])
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            doc->anchor = i;
            doc->caret = i + doc->find_length;
            doc->selecting = true;
            doc->find_hit = i;
            return i;
        }
    }
    doc->find_hit = 0;
    return 0;
}

static u32 axdoc_word_at(const axdoc_doc_t *doc, u32 position)
{
    while (position > 0 && position < doc->char_count &&
           doc->chars[position].ch != ' ' && doc->chars[position].ch != '\n')
        position--;
    return position;
}

bool_t axdoc_key(axdoc_doc_t *doc, u16 key, bool_t shift, bool_t control)
{
    if (control)
    {
        if (key == 'b' || key == 'B')
        {
            doc->bold = (u16)(doc->bold ? 0 : 1);
            axdoc_apply_style(doc, (u8)doc->bold, (u8)doc->italic, (u8)doc->underline);
            return true;
        }
        if (key == 'i' || key == 'I')
        {
            doc->italic = (u16)(doc->italic ? 0 : 1);
            axdoc_apply_style(doc, (u8)doc->bold, (u8)doc->italic, (u8)doc->underline);
            return true;
        }
        if (key == 'u' || key == 'U')
        {
            doc->underline = (u16)(doc->underline ? 0 : 1);
            axdoc_apply_style(doc, (u8)doc->bold, (u8)doc->italic, (u8)doc->underline);
            return true;
        }
        if (key == 'z' || key == 'Z')
        {
            axdoc_undo(doc);
            return true;
        }
        if (key == 'a' || key == 'A')
        {
            doc->anchor = 0;
            doc->caret = doc->char_count;
            doc->selecting = true;
            return true;
        }
    }
    switch (key)
    {
    case 0x08:
        axdoc_backspace(doc);
        return true;
    case 0x7F:
        axdoc_delete_forward(doc);
        return true;
    case 0x0D:
    case 0x0A:
        axdoc_split_paragraph(doc);
        return true;
    case 0x09:
        axdoc_insert(doc, "    ");
        return true;
    case 0x1B:
        doc->selecting = false;
        doc->anchor = doc->caret;
        return true;
    default:
        break;
    }
    if (key >= 0x20 && key < 0x7F)
    {
        char text[2];
        text[0] = (char)key;
        text[1] = 0;
        axdoc_insert(doc, text);
        return true;
    }
    AXIOM_UNUSED_VAR(shift);
    return false;
}

bool_t axdoc_nav(axdoc_doc_t *doc, u16 key, bool_t shift)
{
    switch (key)
    {
    case 0x01:
        if (doc->caret > 0)
            axdoc_move(doc, -1, shift);
        return true;
    case 0x02:
        if (doc->caret < doc->char_count)
            axdoc_move(doc, 1, shift);
        return true;
    case 0x03:
        return true;
    case 0x04:
        return true;
    default:
        return false;
    }
}

void axdoc_click(axdoc_doc_t *doc, i32 x, i32 y)
{
    u32 position = axdoc_position_from_point(doc, x, y + doc->scroll_y);
    if (x < 0 || y < 0)
        return;
    doc->caret = position;
    doc->anchor = position;
    doc->selecting = false;
}

void axdoc_drag(axdoc_doc_t *doc, i32 x, i32 y)
{
    u32 position = axdoc_position_from_point(doc, x, y + doc->scroll_y);
    doc->caret = position;
    doc->selecting = (bool_t)(doc->anchor != doc->caret);
}

void axdoc_scroll(axdoc_doc_t *doc, i32 delta, i32 view_height)
{
    doc->scroll_y += delta;
    axdoc_scroll_into_view(doc, view_height);
}

void axdoc_paint(axdoc_doc_t *doc, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t toolbar = gui_rect(bounds.x, bounds.y, bounds.w, AXDOC_TOOLBAR_HEIGHT);
    gui_rect_t ruler = gui_rect(bounds.x, bounds.y + AXDOC_TOOLBAR_HEIGHT,
                                bounds.w, AXDOC_RULER_HEIGHT);
    gui_rect_t canvas = gui_rect(bounds.x, ruler.y + AXDOC_RULER_HEIGHT,
                                 bounds.w,
                                 bounds.h - AXDOC_TOOLBAR_HEIGHT -
                                 AXDOC_RULER_HEIGHT - AXDOC_STATUS_HEIGHT);
    gui_rect_t status = gui_rect(bounds.x, bounds.y + bounds.h - AXDOC_STATUS_HEIGHT,
                                 bounds.w, AXDOC_STATUS_HEIGHT);
    i32 page_width = AXDOC_PAGE_WIDTH;
    i32 page_x = canvas.x + (canvas.w - page_width) / 2;
    gui_rect_t page;
    if (canvas.h < 60)
        return;
    gui_fill_rect(surface, bounds, AXDOC_C_DESK);
    axdoc_paint_toolbar(surface, doc, toolbar);
    if (doc->show_ruler)
    {
        axdoc_paint_ruler(surface, doc, ruler, page_x);
        page = gui_rect(page_x, canvas.y, page_width, canvas.h + 48);
    }
    else
    {
        page = gui_rect(page_x, canvas.y, page_width, canvas.h);
    }
    axdoc_paint_page(surface, doc, page, canvas.y);
    axdoc_paint_status(surface, doc, status);
    axdoc_ensure_visible(doc, canvas.h);
}

void axdoc_dump(const axdoc_doc_t *doc)
{
    printk("axdoc: '%s' chars=%u paras=%u lines=%u words=%u\n",
           doc->title, (unsigned)doc->char_count, (unsigned)doc->para_count,
           (unsigned)doc->line_count, (unsigned)doc->words);
}

bool_t axdoc_selftest(void)
{
    axdoc_doc_t doc;
    axdoc_init(&doc, "test");
    axdoc_load(&doc, "Hello world.\nSecond paragraph here.");
    if (doc.para_count != 2)
        return false;
    if (doc.words != 6)
        return false;
    doc.caret = doc.char_count;
    axdoc_key(&doc, '!', false, false);
    if (doc.chars[doc.char_count - 1].ch != '!')
        return false;
    axdoc_set_find(&doc, "world");
    if (axdoc_find_next(&doc) == 0)
        return false;
    axdoc_set_alignment(&doc, 1);
    if (doc.paras[axdoc_para_at(&doc, doc.caret)].align != 1)
        return false;
    axdoc_toggle_list(&doc);
    if (doc.paras[axdoc_para_at(&doc, doc.caret)].list == 0)
        return false;
    axdoc_set_heading(&doc, 1);
    if (doc.paras[axdoc_para_at(&doc, doc.caret)].heading != 1)
        return false;
    return true;
}
