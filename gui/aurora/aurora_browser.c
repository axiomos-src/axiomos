/* AxiomOS - Aurora: web browser shell.
 *
 * This is the browser application of the Aurora environment.  It owns tabs, a
 * navigation history stack, a bookmark bar, a downloads counter and a small
 * renderer capable of laying out a constrained subset of HTML:
 *
 *   <h1>..<h3>, <p>, <a href>, <ul><li>, <li>, <hr>, <br>, <pre>, <img>,
 *   <title> and inline text; tags are case insensitive and attributes are
 *   parsed only for href, src and alt.
 *
 * The renderer produces a display list of blocks and links; painting is pure
 * geometry and text, no layout recursion beyond a single block pass.
 */

#include "gui.h"

#define AXBR_MAX_TABS        8
#define AXBR_MAX_HISTORY     24
#define AXBR_MAX_BOOKMARKS   16
#define AXBR_MAX_BLOCKS      96
#define AXBR_MAX_LINKS       64
#define AXBR_MAX_DOC         4096
#define AXBR_TOOLBAR_HEIGHT  56
#define AXBR_TABSTRIP_HEIGHT 34
#define AXBR_STATUSBAR       24
#define AXBR_CONTENT_MARGIN  24

#define AXBR_C_CHROME   GUI_RGB(0x22, 0x24, 0x2C)
#define AXBR_C_CHROME2  GUI_RGB(0x2B, 0x2E, 0x38)
#define AXBR_C_PAGE     GUI_RGB(0xF2, 0xF1, 0xEC)
#define AXBR_C_INK      GUI_RGB(0x1C, 0x1E, 0x24)
#define AXBR_C_INK_SOFT GUI_RGB(0x55, 0x5A, 0x66)
#define AXBR_C_LINK     GUI_RGB(0x1F, 0x5F, 0xB8)
#define AXBR_C_ACCENT   GUI_RGB(0x59, 0xA8, 0xFF)
#define AXBR_C_CHROME_TEXT GUI_RGB(0xD2, 0xD6, 0xE0)
#define AXBR_C_OMNI     GUI_RGB(0x31, 0x35, 0x40)
#define AXBR_C_SEP      GUI_RGB(0x39, 0x3D, 0x48)
#define AXBR_C_FAVICON  GUI_RGB(0x4E, 0x8E, 0x62)

enum axbr_block_kind
{
    AXBR_BLOCK_HEADING = 0,
    AXBR_BLOCK_PARAGRAPH,
    AXBR_BLOCK_LIST_ITEM,
    AXBR_BLOCK_CODE,
    AXBR_BLOCK_IMAGE,
    AXBR_BLOCK_RULE,
    AXBR_BLOCK_TITLE
};

typedef struct axbr_block
{
    u8   kind;
    u8   level;
    u16  text_start;
    u16  text_length;
    u16  link;          /* index into links or 0xFFFF */
    i32  height;
} axbr_block_t;

typedef struct axbr_link
{
    u16  text_start;
    u16  text_length;
    char href[96];
} axbr_link_t;

typedef struct axbr_tab
{
    char  title[64];
    char  url[160];
    i32   scroll;
    bool_t loading;
    u32   progress;
    u32   history[AXBR_MAX_HISTORY];
    u16   history_count;
    u16   history_index;
    u8    blocks[AXBR_MAX_BLOCKS];
    u16   block_count;
    u32   stamp;
} axbr_tab_t;

typedef struct axbr_bookmark
{
    char title[40];
    char url[96];
} axbr_bookmark_t;

typedef struct axbr_browser
{
    axbr_tab_t     tabs[AXBR_MAX_TABS];
    u16            tab_count;
    u16            tab_active;
    axbr_block_t   blocks[AXBR_MAX_BLOCKS];
    u16            block_count;
    axbr_link_t    links[AXBR_MAX_LINKS];
    u16            link_count;
    char           text[AXBR_MAX_DOC];
    u16            text_length;
    axbr_bookmark_t bookmarks[AXBR_MAX_BOOKMARKS];
    u16            bookmark_count;
    bool_t         bookmark_bar;
    char           omni[160];
    u16            omni_length;
    bool_t         omni_focused;
    i32            scroll_y;
    i32            content_height;
    i32            view_width;
    i32            view_height;
    u32            downloads;
    u32            blocked_trackers;
    char           status[96];
    u8             hover_block;
} axbr_browser_t;

/* One entry in the parser's tag vocabulary. */
enum axbr_tag
{
    AXBR_TAG_NONE = 0,
    AXBR_TAG_TITLE,
    AXBR_TAG_H1,
    AXBR_TAG_H2,
    AXBR_TAG_H3,
    AXBR_TAG_P,
    AXBR_TAG_A,
    AXBR_TAG_UL,
    AXBR_TAG_LI,
    AXBR_TAG_PRE,
    AXBR_TAG_IMG,
    AXBR_TAG_HR,
    AXBR_TAG_BR,
    AXBR_TAG_DIV,
    AXBR_TAG_UNKNOWN
};

static void  axbr_zero(void *ptr, u32 size);
static bool_t axbr_equals(const char *a, const char *b, u16 length);
static u16   axbr_tag_lookup(const char *name, u16 length);
static void  axbr_copy(char *dst, u16 capacity, const char *src, u16 length);
static void  axbr_parse(axbr_browser_t *br, const char *html);
static void  axbr_emit_text(axbr_browser_t *br, const char *text, u16 length);
static void  axbr_begin_block(axbr_browser_t *br, u8 kind, u8 level);
static void  axbr_end_block(axbr_browser_t *br);
static void  axbr_append_link(axbr_browser_t *br, const char *href, u16 length);
static u16   axbr_line_height(u8 kind, u8 level);
static u16   axbr_measure_scale(u8 kind);
static i32   axbr_wrap_line(gui_surface_t *surface, axbr_browser_t *br,
                            i32 x, i32 y, i32 width, const char *text,
                            u16 length, u32 colour, u8 scale, bool_t underline);
static i32   axbr_paint_block(gui_surface_t *surface, axbr_browser_t *br,
                              const axbr_block_t *block, i32 x, i32 y,
                              i32 width);
static void  axbr_paint_tabstrip(gui_surface_t *surface, axbr_browser_t *br,
                                 gui_rect_t strip);
static void  axbr_paint_toolbar(gui_surface_t *surface, axbr_browser_t *br,
                                gui_rect_t bar);
static void  axbr_paint_status(gui_surface_t *surface, axbr_browser_t *br,
                               gui_rect_t bar);
static void  axbr_paint_content(gui_surface_t *surface, axbr_browser_t *br,
                                gui_rect_t content);
static void  axbr_paint_bookmark_bar(gui_surface_t *surface, axbr_browser_t *br,
                                     gui_rect_t bar);
static axbr_tab_t *axbr_current(axbr_browser_t *br);
static void  axbr_set_url(axbr_browser_t *br, const char *url);
static void  axbr_navigate(axbr_browser_t *br, const char *url);
static void  axbr_add_bookmark(axbr_browser_t *br, const char *title,
                               const char *url);
static const char *axbr_sample_home(void);

static void axbr_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static bool_t axbr_equals(const char *a, const char *b, u16 length)
{
    u16 i;
    for (i = 0; i < length; i++)
    {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (ca != cb)
            return false;
    }
    return b[length] == 0 || b[length] == ' ' || b[length] == '>' || b[length] == '/';
}

static u16 axbr_tag_lookup(const char *name, u16 length)
{
    static const char *const tags[] = {
        "title", "h1", "h2", "h3", "p", "a", "ul", "li",
        "pre", "img", "hr", "br", "div"
    };
    static const u16 kinds[] = {
        AXBR_TAG_TITLE, AXBR_TAG_H1, AXBR_TAG_H2, AXBR_TAG_H3, AXBR_TAG_P,
        AXBR_TAG_A, AXBR_TAG_UL, AXBR_TAG_LI, AXBR_TAG_PRE, AXBR_TAG_IMG,
        AXBR_TAG_HR, AXBR_TAG_BR, AXBR_TAG_DIV
    };
    u16 i;
    for (i = 0; i < 13; i++)
    {
        if (axbr_equals(name, tags[i], length))
            return kinds[i];
    }
    return AXBR_TAG_UNKNOWN;
}

static void axbr_copy(char *dst, u16 capacity, const char *src, u16 length)
{
    u16 i = 0;
    while (i < length && i + 1 < capacity)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void axbr_emit_text(axbr_browser_t *br, const char *text, u16 length)
{
    u16 i = 0;
    while (i < length && br->text_length + 1 < AXBR_MAX_DOC)
    {
        char c = text[i];
        if (c == '\r')
        {
            i++;
            continue;
        }
        if (c == '\n' || c == '\t')
            c = ' ';
        br->text[br->text_length] = c;
        br->text_length++;
        i++;
    }
}

static void axbr_append_link(axbr_browser_t *br, const char *href, u16 length)
{
    axbr_link_t *link;
    if (br->link_count >= AXBR_MAX_LINKS)
        return;
    link = &br->links[br->link_count];
    axbr_copy(link->href, 96, href, length);
    link->text_start = br->text_length;
    link->text_length = 0;
    br->link_count++;
}

static void axbr_begin_block(axbr_browser_t *br, u8 kind, u8 level)
{
    axbr_block_t *block;
    if (br->block_count >= AXBR_MAX_BLOCKS)
        return;
    block = &br->blocks[br->block_count];
    block->kind = kind;
    block->level = level;
    block->text_start = br->text_length;
    block->text_length = 0;
    block->link = 0xFFFF;
    block->height = 0;
    br->block_count++;
}

static void axbr_end_block(axbr_browser_t *br)
{
    axbr_block_t *block;
    u16 k;
    if (br->block_count == 0)
        return;
    block = &br->blocks[br->block_count - 1];
    block->text_length = (u16)(br->text_length - block->text_start);
    for (k = 0; k < br->link_count; k++)
    {
        if (br->links[k].text_start >= block->text_start &&
            br->links[k].text_start < block->text_start + block->text_length)
        {
            block->link = k;
            br->links[k].text_length = (u16)(br->text_length - br->links[k].text_start);
            break;
        }
    }
    block->height = axbr_line_height(block->kind, block->level);
}

static void axbr_parse(axbr_browser_t *br, const char *html)
{
    u16 i = 0;
    u16 length = 0;
    u8 level = 0;
    bool_t in_title = false;
    br->text_length = 0;
    br->block_count = 0;
    br->link_count = 0;
    while (html[length] != 0)
        length++;
    while (i < length)
    {
        if (html[i] == '<')
        {
            u16 start = (u16)(i + 1);
            u16 end = start;
            u16 name_end;
            u16 tag;
            bool_t closing = false;
            if (html[start] == '/')
            {
                closing = true;
                start++;
            }
            while (end < length && html[end] != '>' && html[end] != ' ')
                end++;
            name_end = (u16)(end - start);
            tag = axbr_tag_lookup(&html[start], name_end);
            if (!closing)
            {
                if (in_title && tag != AXBR_TAG_TITLE)
                {
                    /* text inside <title> is stored in a dedicated block */
                }
                switch (tag)
                {
                case AXBR_TAG_TITLE:
                    in_title = true;
                    axbr_begin_block(br, AXBR_BLOCK_TITLE, 0);
                    break;
                case AXBR_TAG_H1:
                    if (br->block_count > 0)
                        axbr_end_block(br);
                    level = 1;
                    axbr_begin_block(br, AXBR_BLOCK_HEADING, 1);
                    break;
                case AXBR_TAG_H2:
                    if (br->block_count > 0)
                        axbr_end_block(br);
                    level = 2;
                    axbr_begin_block(br, AXBR_BLOCK_HEADING, 2);
                    break;
                case AXBR_TAG_H3:
                    if (br->block_count > 0)
                        axbr_end_block(br);
                    level = 3;
                    axbr_begin_block(br, AXBR_BLOCK_HEADING, 3);
                    break;
                case AXBR_TAG_P:
                case AXBR_TAG_DIV:
                    if (br->block_count > 0)
                        axbr_end_block(br);
                    level = 0;
                    axbr_begin_block(br, AXBR_BLOCK_PARAGRAPH, 0);
                    break;
                case AXBR_TAG_LI:
                    if (br->block_count > 0 &&
                        br->blocks[br->block_count - 1].kind == AXBR_BLOCK_LIST_ITEM)
                        axbr_end_block(br);
                    axbr_begin_block(br, AXBR_BLOCK_LIST_ITEM, 0);
                    break;
                case AXBR_TAG_PRE:
                    if (br->block_count > 0)
                        axbr_end_block(br);
                    axbr_begin_block(br, AXBR_BLOCK_CODE, 0);
                    break;
                case AXBR_TAG_A:
                {
                    u16 cursor = start;
                    while (cursor + 5 < end)
                    {
                        if (html[cursor] == 'h' && html[cursor + 1] == 'r' &&
                            html[cursor + 2] == 'e' && html[cursor + 3] == 'f' &&
                            html[cursor + 4] == '=')
                        {
                            u16 value = (u16)(cursor + 5);
                            u16 value_end = value;
                            char quote = html[value];
                            if (quote == '"' || quote == '\'')
                            {
                                value++;
                                value_end = value;
                                while (value_end < end && html[value_end] != quote)
                                    value_end++;
                                axbr_append_link(br, &html[value], (u16)(value_end - value));
                            }
                            break;
                        }
                        cursor++;
                    }
                }
                break;
                case AXBR_TAG_IMG:
                    if (br->block_count > 0)
                        axbr_end_block(br);
                    axbr_begin_block(br, AXBR_BLOCK_IMAGE, 0);
                    axbr_end_block(br);
                    break;
                case AXBR_TAG_HR:
                    if (br->block_count > 0)
                        axbr_end_block(br);
                    axbr_begin_block(br, AXBR_BLOCK_RULE, 0);
                    axbr_end_block(br);
                    break;
                case AXBR_TAG_BR:
                    axbr_emit_text(br, "\n", 1);
                    break;
                default:
                    break;
                }
            }
            else
            {
                switch (tag)
                {
                case AXBR_TAG_TITLE:
                    in_title = false;
                    if (br->block_count > 0)
                        axbr_end_block(br);
                    break;
                case AXBR_TAG_H1:
                case AXBR_TAG_H2:
                case AXBR_TAG_H3:
                case AXBR_TAG_P:
                case AXBR_TAG_LI:
                case AXBR_TAG_PRE:
                case AXBR_TAG_DIV:
                case AXBR_TAG_A:
                    if (br->block_count > 0)
                        axbr_end_block(br);
                    level = 0;
                    break;
                default:
                    break;
                }
            }
            i = (u16)(end + 1);
            continue;
        }
        {
            u16 start = i;
            while (i < length && html[i] != '<')
                i++;
            if (i > start)
                axbr_emit_text(br, &html[start], (u16)(i - start));
        }
    }
    if (br->block_count > 0 &&
        br->blocks[br->block_count - 1].text_length == 0)
        br->block_count--;
    if (br->block_count > 0)
        br->blocks[br->block_count - 1].height = axbr_line_height(
            br->blocks[br->block_count - 1].kind,
            br->blocks[br->block_count - 1].level);
    AXIOM_UNUSED_VAR(level);
}

static u16 axbr_measure_scale(u8 kind)
{
    if (kind == AXBR_BLOCK_HEADING)
        return 3;
    return 1;
}

static u16 axbr_line_height(u8 kind, u8 level)
{
    if (kind == AXBR_BLOCK_HEADING)
    {
        if (level == 1)
            return 44;
        if (level == 2)
            return 34;
        return 26;
    }
    if (kind == AXBR_BLOCK_CODE)
        return 18;
    if (kind == AXBR_BLOCK_IMAGE)
        return 120;
    if (kind == AXBR_BLOCK_RULE)
        return 20;
    return 18;
}

/* Simple greedy wrapper used for paragraphs and list items.  Returns the new
 * y coordinate after painting. */
static i32 axbr_wrap_line(gui_surface_t *surface, axbr_browser_t *br,
                          i32 x, i32 y, i32 width, const char *text,
                          u16 length, u32 colour, u8 scale, bool_t underline)
{
    i32 line_height = (scale >= 3) ? 40 : (scale == 2 ? 26 : 18);
    i32 cx = x;
    u16 i = 0;
    u16 start = 0;
    while (i <= length)
    {
        char measure[AXBR_MAX_DOC];
        u16 k;
        if (i == length || text[i] == ' ')
        {
            u16 count = (u16)(i - start);
            for (k = 0; k < count; k++)
                measure[k] = text[start + k];
            measure[count] = 0;
            if (count > 0)
            {
                i32 text_width = gui_text_width(measure, scale);
                if (cx + text_width > x + width && cx > x)
                {
                    y += line_height;
                    cx = x;
                }
                gui_text(surface, cx, y, measure, colour, scale);
                if (underline)
                    gui_hline(surface, cx, cx + text_width, y + 14 + scale * 4, colour);
                cx += text_width + gui_text_width(" ", scale);
            }
            start = (u16)(i + 1);
        }
        i++;
    }
    br->content_height += 0;
    return y + line_height;
}

static i32 axbr_paint_block(gui_surface_t *surface, axbr_browser_t *br,
                            const axbr_block_t *block, i32 x, i32 y,
                            i32 width)
{
    const char *text = &br->text[block->text_start];
    u32 colour = AXBR_C_INK;
    u8 scale = 1;
    bool_t underline = false;
    if (block->kind == AXBR_BLOCK_HEADING)
    {
        scale = (block->level == 1) ? 3 : (block->level == 2 ? 2 : 2);
        if (block->level == 3)
            scale = 2;
    }
    if (block->link != 0xFFFF && block->kind != AXBR_BLOCK_HEADING)
    {
        colour = AXBR_C_LINK;
        underline = true;
    }
    if (block->kind == AXBR_BLOCK_TITLE)
        return y;
    if (block->kind == AXBR_BLOCK_RULE)
    {
        gui_hline(surface, x, x + width, y + 10, AXBR_C_INK_SOFT);
        return y + 20;
    }
    if (block->kind == AXBR_BLOCK_IMAGE)
    {
        gui_rect_t frame = gui_rect(x, y, width < 320 ? width : 320, 104);
        gui_fill_rect(surface, frame, GUI_RGB(0xDD, 0xDB, 0xD4));
        gui_rect_outline(surface, frame, GUI_RGB(0xC5, 0xC2, 0xBA));
        gui_text(surface, x + 12, y + 44, "[image]", AXBR_C_INK_SOFT, 1);
        return y + 120;
    }
    if (block->kind == AXBR_BLOCK_CODE)
    {
        gui_fill_rect(surface, gui_rect(x, y, width, block->height),
                      GUI_RGB(0xE6, 0xE4, 0xDC));
        gui_text(surface, x + 10, y + 2, text, AXBR_C_INK, 1);
        return y + block->height;
    }
    if (block->kind == AXBR_BLOCK_LIST_ITEM)
    {
        gui_fill_rect(surface, gui_rect(x - 12, y + 6, 4, 4), AXBR_C_INK);
        return axbr_wrap_line(surface, br, x, y, width, text, block->text_length,
                              colour, scale, underline);
    }
    return axbr_wrap_line(surface, br, x, y, width, text, block->text_length,
                          colour, scale, underline);
}

static void axbr_paint_tabstrip(gui_surface_t *surface, axbr_browser_t *br,
                                gui_rect_t strip)
{
    i32 x = strip.x + 8;
    u16 i;
    gui_fill_rect(surface, strip, AXBR_C_CHROME);
    for (i = 0; i < br->tab_count; i++)
    {
        i32 width = 180;
        gui_rect_t tab = gui_rect(x, strip.y + 4, width, strip.h - 8);
        u32 fill = (i == br->tab_active) ? AXBR_C_CHROME2 : GUI_RGB(0x26, 0x28, 0x30);
        gui_fill_round_rect(surface, tab, 6, fill);
        gui_fill_rect(surface, gui_rect(x + 12, strip.y + 13, 6, 6), AXBR_C_FAVICON);
        gui_text(surface, x + 24, strip.y + 9, br->tabs[i].title,
                 AXBR_C_CHROME_TEXT, 1);
        gui_text(surface, x + width - 22, strip.y + 9, "x", GUI_RGB(0x8A, 0x90, 0x9E), 1);
        x += width + 6;
    }
    gui_text(surface, x + 8, strip.y + 9, "+", AXBR_C_CHROME_TEXT, 1);
}

static void axbr_paint_toolbar(gui_surface_t *surface, axbr_browser_t *br,
                               gui_rect_t bar)
{
    i32 y = bar.y + 18;
    gui_fill_rect(surface, bar, AXBR_C_CHROME2);
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y + bar.h - 1, AXBR_C_SEP);
    gui_text(surface, bar.x + 16, y, "<", AXBR_C_CHROME_TEXT, 1);
    gui_text(surface, bar.x + 40, y, ">", AXBR_C_CHROME_TEXT, 1);
    gui_text(surface, bar.x + 64, y, "R", AXBR_C_CHROME_TEXT, 1);
    {
        gui_rect_t omni = gui_rect(bar.x + 96, bar.y + 10, bar.w - 260, 34);
        gui_fill_round_rect(surface, omni, 17, AXBR_C_OMNI);
        gui_fill_rect(surface, gui_rect(omni.x + 12, omni.y + 13, 8, 8), AXBR_C_FAVICON);
        gui_text(surface, omni.x + 28, omni.y + 10, br->omni, AXBR_C_CHROME_TEXT, 1);
        if (br->omni_focused)
        {
            i32 caret = omni.x + 28 + gui_text_width(br->omni, 1) + 2;
            gui_fill_rect(surface, gui_rect(caret, omni.y + 8, 2, 18), AXBR_C_ACCENT);
            gui_rect_outline(surface, omni, AXBR_C_ACCENT);
        }
    }
    gui_text(surface, bar.x + bar.w - 148, y, "star", GUI_RGB(0xA8, 0xB0, 0xC0), 1);
    gui_text(surface, bar.x + bar.w - 100, y, "menu", GUI_RGB(0xA8, 0xB0, 0xC0), 1);
    if (br->tabs[br->tab_active].loading)
    {
        i32 progress = (i32)br->tabs[br->tab_active].progress;
        gui_fill_rect(surface, gui_rect(bar.x, bar.y + bar.h - 3, progress * bar.w / 100, 3),
                      AXBR_C_ACCENT);
    }
}

static void axbr_paint_bookmark_bar(gui_surface_t *surface, axbr_browser_t *br,
                                    gui_rect_t bar)
{
    i32 x = bar.x + 12;
    u16 i;
    gui_fill_rect(surface, bar, GUI_RGB(0x2E, 0x31, 0x3A));
    for (i = 0; i < br->bookmark_count; i++)
    {
        i32 width = gui_text_width(br->bookmarks[i].title, 1) + 24;
        gui_rect_t chip = gui_rect(x, bar.y + 4, width, bar.h - 8);
        gui_fill_round_rect(surface, chip, 4, GUI_RGB(0x3A, 0x3E, 0x4A));
        gui_text(surface, x + 10, bar.y + 8, br->bookmarks[i].title,
                 AXBR_C_CHROME_TEXT, 1);
        x += width + 6;
    }
}

static void axbr_paint_status(gui_surface_t *surface, axbr_browser_t *br,
                              gui_rect_t bar)
{
    char counter[32];
    u32 value = br->blocked_trackers;
    u16 digits = 1;
    u32 tmp = value;
    gui_fill_rect(surface, bar, AXBR_C_CHROME);
    gui_text(surface, bar.x + 12, bar.y + 7, br->status, GUI_RGB(0x9A, 0xA2, 0xB2), 1);
    while (tmp >= 10)
    {
        tmp /= 10;
        digits++;
    }
    counter[0] = 'T';
    counter[1] = 'r';
    counter[2] = 'a';
    counter[3] = 'c';
    counter[4] = 'k';
    counter[5] = 'e';
    counter[6] = 'r';
    counter[7] = 's';
    counter[8] = ' ';
    counter[9] = ' ';
    tmp = value;
    {
        u16 k;
        for (k = 0; k < digits; k++)
        {
            counter[9 + digits - k] = (char)('0' + (tmp % 10));
            tmp /= 10;
        }
    }
    counter[9 + digits + 1] = 0;
    gui_text(surface, bar.x + bar.w - 140, bar.y + 7, counter,
             GUI_RGB(0x9A, 0xA2, 0xB2), 1);
}

static void axbr_paint_content(gui_surface_t *surface, axbr_browser_t *br,
                               gui_rect_t content)
{
    u16 i;
    i32 y = content.y + AXBR_CONTENT_MARGIN - br->scroll_y;
    i32 width = content.w - AXBR_CONTENT_MARGIN * 2;
    gui_fill_rect(surface, content, AXBR_C_PAGE);
    br->content_height = 0;
    for (i = 0; i < br->block_count; i++)
    {
        axbr_block_t *block = &br->blocks[i];
        if (y + block->height > content.y && y < content.y + content.h)
            y = axbr_paint_block(surface, br, block, content.x + AXBR_CONTENT_MARGIN,
                                 y, width);
        else
            y += block->height + 8;
        br->content_height = y;
    }
    if (br->block_count == 0)
        gui_text(surface, content.x + 24, content.y + 24, "About:blank",
                 AXBR_C_INK_SOFT, 1);
    {
        i32 thumb_height = content.h * content.h / (br->content_height > 0 ? br->content_height : content.h);
        i32 thumb_top = content.y + br->scroll_y * content.h /
                        (br->content_height > 0 ? br->content_height : content.h);
        if (br->content_height > content.h)
        {
            gui_fill_rect(surface, gui_rect(content.x + content.w - 10, content.y,
                                            4, content.h),
                          GUI_RGB(0xDE, 0xDC, 0xD5));
            gui_fill_round_rect(surface,
                                gui_rect(content.x + content.w - 10, thumb_top, 4,
                                         thumb_height < 24 ? 24 : thumb_height),
                                2, GUI_RGB(0x98, 0x9C, 0xA6));
        }
    }
}

/* -------------------------------------------------------------- public API */

static axbr_tab_t *axbr_current(axbr_browser_t *br)
{
    if (br->tab_active >= br->tab_count)
        br->tab_active = (u16)(br->tab_count == 0 ? 0 : br->tab_count - 1);
    return &br->tabs[br->tab_active];
}

static const char *axbr_sample_home(void)
{
    return "<title>Axiom start</title>"
           "<h1>Axiom home</h1>"
           "<p>This page lives entirely inside the kernel. No network stack is "
           "involved in rendering it; the browser parses a small HTML subset and "
           "paints a display list.</p>"
           "<h2>Shortcuts</h2>"
           "<ul><li>term - terminal</li><li>files - file manager</li>"
           "<li>sys - system monitor</li></ul>"
           "<hr>"
           "<p>Navigate with <a href=\"axiom://docs\">the docs page</a> to try "
           "link following, or use the omnibox to type a URL.</p>";
}

void axbr_init(axbr_browser_t *br)
{
    axbr_zero(br, (u32)sizeof(*br));
    br->tab_count = 1;
    br->tab_active = 0;
    br->bookmark_bar = true;
    br->status[0] = 'R';
    br->status[1] = 'e';
    br->status[2] = 'a';
    br->status[3] = 'd';
    br->status[4] = 'y';
    br->status[5] = 0;
    axbr_copy(br->tabs[0].title, 64, "Axiom start", 11);
    axbr_copy(br->tabs[0].url, 160, "axiom://home", 11);
    axbr_parse(br, axbr_sample_home());
    axbr_set_url(br, "axiom://home");
    axbr_add_bookmark(br, "Home", "axiom://home");
    axbr_add_bookmark(br, "Docs", "axiom://docs");
    axbr_add_bookmark(br, "Kernel", "axiom://kernel");
}

static void axbr_set_url(axbr_browser_t *br, const char *url)
{
    axbr_tab_t *tab = axbr_current(br);
    u16 i = 0;
    while (url[i] != 0 && i < 159)
    {
        br->omni[i] = url[i];
        tab->url[i] = url[i];
        i++;
    }
    br->omni[i] = 0;
    tab->url[i] = 0;
    br->omni_length = i;
    tab->scroll = 0;
    br->scroll_y = 0;
}

static void axbr_navigate(axbr_browser_t *br, const char *url)
{
    axbr_tab_t *tab = axbr_current(br);
    if (tab->history_count < AXBR_MAX_HISTORY)
        tab->history_count++;
    tab->history[tab->history_count - 1] = tab->stamp;
    tab->history_index = (u16)(tab->history_count - 1);
    tab->stamp++;
    axbr_set_url(br, url);
    if (url[0] == 'a' && url[1] == 'x' && url[2] == 'i' && url[3] == 'o' &&
        url[4] == 'm' && url[5] == ':' && url[6] == '/' && url[7] == '/' &&
        url[8] == 'd' && url[9] == 'o' && url[10] == 'c' && url[11] == 's')
    {
        axbr_parse(br,
                   "<title>Docs</title><h1>Documentation</h1>"
                   "<p>The AxiomOS manual is generated from the kernel headers. "
                   "Each subsystem documents its invariants at the top of the "
                   "translation unit that implements it.</p>"
                   "<h2>Subsystems</h2><ul>"
                   "<li>boot - UEFI loader</li><li>mm - paging and heap</li>"
                   "<li>fs - VFS, ext2, FAT32, tmpfs</li>"
                   "<li>security - HVCI policy engine</li></ul>");
        axbr_copy(tab->title, 64, "Docs", 4);
    }
    else if (url[0] == 'a' && url[1] == 'x' && url[2] == 'i' && url[3] == 'o' &&
             url[4] == 'm' && url[5] == ':' && url[6] == '/' && url[7] == '/' &&
             url[8] == 'k')
    {
        axbr_parse(br,
                   "<title>Kernel</title><h1>Kernel overview</h1>"
                   "<p>The kernel is a single address space, preemptive, with a "
                   "round robin scheduler and a software compositor.</p>"
                   "<p>Line counts are tracked by tools/count.py.</p>");
        axbr_copy(tab->title, 64, "Kernel", 6);
    }
    else
    {
        axbr_parse(br, axbr_sample_home());
        axbr_copy(tab->title, 64, "Axiom start", 11);
    }
}

void axbr_new_tab(axbr_browser_t *br, const char *url)
{
    if (br->tab_count >= AXBR_MAX_TABS)
        return;
    br->tab_count++;
    br->tab_active = (u16)(br->tab_count - 1);
    axbr_zero(&br->tabs[br->tab_active], (u32)sizeof(br->tabs[br->tab_active]));
    axbr_set_url(br, url);
    axbr_parse(br, axbr_sample_home());
}

void axbr_close_tab(axbr_browser_t *br, u16 index)
{
    u16 i;
    if (index >= br->tab_count || br->tab_count <= 1)
        return;
    for (i = index; i + 1 < br->tab_count; i++)
        br->tabs[i] = br->tabs[i + 1];
    br->tab_count--;
    if (br->tab_active >= br->tab_count)
        br->tab_active = (u16)(br->tab_count - 1);
}

static void axbr_add_bookmark(axbr_browser_t *br, const char *title,
                              const char *url)
{
    axbr_bookmark_t *slot;
    u16 i = 0;
    if (br->bookmark_count >= AXBR_MAX_BOOKMARKS)
        return;
    slot = &br->bookmarks[br->bookmark_count];
    while (title[i] != 0 && i < 39)
    {
        slot->title[i] = title[i];
        i++;
    }
    slot->title[i] = 0;
    i = 0;
    while (url[i] != 0 && i < 95)
    {
        slot->url[i] = url[i];
        i++;
    }
    slot->url[i] = 0;
    br->bookmark_count++;
}

void axbr_omni_type(axbr_browser_t *br, char c)
{
    if (!br->omni_focused)
        br->omni_focused = true;
    if (br->omni_length + 1 >= 160)
        return;
    br->omni[br->omni_length] = c;
    br->omni_length++;
    br->omni[br->omni_length] = 0;
}

void axbr_omni_backspace(axbr_browser_t *br)
{
    if (br->omni_length == 0)
        return;
    br->omni_length--;
    br->omni[br->omni_length] = 0;
}

void axbr_omni_commit(axbr_browser_t *br)
{
    br->omni[br->omni_length] = 0;
    br->omni_focused = false;
    if (br->omni_length == 0)
        return;
    axbr_navigate(br, br->omni);
}

bool_t axbr_click(axbr_browser_t *br, i32 x, i32 y, i32 top)
{
    i32 local_y = y - top;
    if (local_y < AXBR_TABSTRIP_HEIGHT)
    {
        i32 tab_index = (x - 8) / 186;
        if (tab_index >= 0 && tab_index < (i32)br->tab_count)
            br->tab_active = (u16)tab_index;
        return true;
    }
    if (local_y < AXBR_TABSTRIP_HEIGHT + AXBR_TOOLBAR_HEIGHT)
    {
        if (x > 96 && x < 96 + br->view_width - 260)
            br->omni_focused = true;
        return true;
    }
    br->omni_focused = false;
    return false;
}

void axbr_scroll(axbr_browser_t *br, i32 delta)
{
    br->scroll_y += delta;
    if (br->scroll_y < 0)
        br->scroll_y = 0;
    {
        i32 max = br->content_height - br->view_height;
        if (max < 0)
            max = 0;
        if (br->scroll_y > max)
            br->scroll_y = max;
    }
}

void axbr_tick(axbr_browser_t *br)
{
    axbr_tab_t *tab = axbr_current(br);
    if (tab->loading)
    {
        tab->progress += 4;
        if (tab->progress >= 100)
        {
            tab->progress = 100;
            tab->loading = false;
            br->status[0] = 'D';
            br->status[1] = 'o';
            br->status[2] = 'n';
            br->status[3] = 'e';
            br->status[4] = 0;
        }
    }
}

void axbr_paint(axbr_browser_t *br, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t strip = gui_rect(bounds.x, bounds.y, bounds.w, AXBR_TABSTRIP_HEIGHT);
    gui_rect_t toolbar = gui_rect(bounds.x, strip.y + strip.h, bounds.w,
                                  AXBR_TOOLBAR_HEIGHT);
    gui_rect_t bookmarks = gui_rect(bounds.x, toolbar.y + toolbar.h, bounds.w, 28);
    gui_rect_t status = gui_rect(bounds.x, bounds.y + bounds.h - AXBR_STATUSBAR,
                                 bounds.w, AXBR_STATUSBAR);
    gui_rect_t content;
    br->view_width = bounds.w;
    br->view_height = bounds.h;
    gui_fill_rect(surface, bounds, AXBR_C_CHROME);
    axbr_paint_tabstrip(surface, br, strip);
    axbr_paint_toolbar(surface, br, toolbar);
    if (br->bookmark_bar)
        axbr_paint_bookmark_bar(surface, br, bookmarks);
    content = gui_rect(bounds.x, bookmarks.y + (br->bookmark_bar ? bookmarks.h : 0),
                       bounds.w, status.y - bookmarks.y - (br->bookmark_bar ? bookmarks.h : 0));
    axbr_paint_content(surface, br, content);
    axbr_paint_status(surface, br, status);
}

void axbr_dump(const axbr_browser_t *br)
{
    printk("axbr: tabs=%u blocks=%u links=%u text=%u\n",
           (unsigned)br->tab_count, (unsigned)br->block_count,
           (unsigned)br->link_count, (unsigned)br->text_length);
}

bool_t axbr_selftest(void)
{
    axbr_browser_t br;
    axbr_init(&br);
    axbr_parse(&br, "<h1>Title</h1><p>body text</p><ul><li>one</li></ul>");
    if (br.block_count < 1)
        return false;
    axbr_parse(&br, "<title>T</title><h1>Title</h1><p>body</p>");
    if (br.block_count < 2)
        return false;
    if (br.text_length == 0)
        return false;
    axbr_parse(&br, "<a href=\"axiom://docs\">Docs</a>");
    if (br.link_count != 1)
        return false;
    if (br.links[0].href[0] != 'a')
        return false;
    axbr_navigate(&br, "axiom://docs");
    if (br.tabs[0].title[0] != 'D')
        return false;
    axbr_new_tab(&br, "axiom://kernel");
    if (br.tab_count != 2)
        return false;
    axbr_close_tab(&br, 1);
    if (br.tab_count != 1)
        return false;
    return true;
}

