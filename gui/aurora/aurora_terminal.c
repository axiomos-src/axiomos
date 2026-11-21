/*
 * AxiomOS - Terminal chrome.
 *
 * This file owns the presentation of terminal content, not the shell itself.  It
 * keeps a character grid, a scrollback ring, a cursor, and a selection anchored
 * by cell coordinates.  A tab bar shows the shells, a status line shows the
 * cursor position and encoding, and the grid is painted with the 16 ANSI colours
 * mapped through the active theme so dark and light both stay legible.
 *
 * The grid stores characters plus an attribute byte, and nothing else.  A
 * cell's colour is resolved at paint time from the attribute, which means a
 * theme switch recolours existing output without rewriting the buffer.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define TERM_MAX_TABS      6
#define TERM_COLS         120
#define TERM_ROWS         40
#define TERM_SCROLLBACK   512
#define TERM_TAB_HEIGHT   36
#define TERM_STATUS_HEIGHT 24
#define TERM_CELL_W       8
#define TERM_CELL_H       16

/* Attribute bits: low nibble is the foreground, next nibble the background,
 * then bold, underline, inverse, wide. */
#define TERM_ATTR_FG(attr)  ((attr) & 0x0F)
#define TERM_ATTR_BG(attr)  (((attr) >> 4) & 0x0F)
#define TERM_ATTR_BOLD      (1u << 8)
#define TERM_ATTR_UNDERLINE (1u << 9)
#define TERM_ATTR_INVERSE   (1u << 10)
#define TERM_ATTR_DIRTY     (1u << 11)

/* One grid cell: a code point and a packed attribute byte.  Colour resolution
 * is deferred to paint time so a theme switch recolours the whole buffer. */
typedef struct axterm_cell {
    u16 ch;
    u16 attr;
} axterm_cell_t;


typedef struct term_tab {
    char        title[32];
    axterm_cell_t grid[TERM_ROWS][TERM_COLS];
    axterm_cell_t scrollback[TERM_SCROLLBACK][TERM_COLS];
    i32         scrollback_count;
    i32         scrollback_head;
    i32         cursor_x;
    i32         cursor_y;
    i32         cols;
    i32         rows;
    u16         attr;
    bool_t      cursor_visible;
    bool_t      blink_on;
    bool_t      busy;
} term_tab_t;

typedef struct term_app {
    gui_rect_t  frame;
    term_tab_t  tabs[TERM_MAX_TABS];
    i32         tab_count;
    i32         active_tab;
    i32         scroll_offset;
    bool_t      selecting;
    i32         select_start_x;
    i32         select_start_y;
    i32         select_end_x;
    i32         select_end_y;
    bool_t      has_selection;
    char        title[48];
    i32         hover_tab;
    i32         hover_close;
    axau_tabbar_t tabbar;
} term_app_t;

/* ------------------------------------------------------------------ */
/* Colour mapping                                                      */
/* ------------------------------------------------------------------ */

/* The 16 ANSI slots mapped onto the Aurora palette.  The first eight are the
 * conventional colours; the bright eight are lifted from the accent and status
 * colours so a themed shell does not look like a transplanted terminal. */
static u32 term_colour(const axau_palette_t *palette, u8 index)
{
    switch (index & 0x0F) {
    case 0:  return axau_colour(palette, AXAU_COLOUR_TEXT);
    case 1:  return 0x00C04030;
    case 2:  return axau_colour(palette, AXAU_COLOUR_SUCCESS);
    case 3:  return axau_colour(palette, AXAU_COLOUR_WARNING);
    case 4:  return axau_colour(palette, AXAU_COLOUR_ACCENT);
    case 5:  return 0x00A040A0;
    case 6:  return 0x0040A0B0;
    case 7:  return axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY);
    case 8:  return axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY);
    case 9:  return 0x00E05050;
    case 10: return axui_lighten(axau_colour(palette, AXAU_COLOUR_SUCCESS), 20);
    case 11: return axui_lighten(axau_colour(palette, AXAU_COLOUR_WARNING), 20);
    case 12: return axui_lighten(axau_colour(palette, AXAU_COLOUR_ACCENT), 20);
    case 13: return 0x00D060D0;
    case 14: return 0x0060C0D0;
    case 15: return axau_colour(palette, AXAU_COLOUR_TEXT);
    default: return axau_colour(palette, AXAU_COLOUR_TEXT);
    }
}

/* ------------------------------------------------------------------ */
/* Grid operations                                                     */
/* ------------------------------------------------------------------ */

void term_app_init(term_app_t *app, gui_rect_t frame)
{
    if (!app) return;
    ax_memset(app, 0, sizeof(*app));
    app->frame = frame;
    app->hover_tab = -1;
    app->hover_close = -1;
    ax_strlcpy(app->title, "Axiom Terminal", sizeof(app->title));
    for (i32 i = 0; i < TERM_MAX_TABS; i++) {
        app->tabs[i].cols = TERM_COLS;
        app->tabs[i].rows = TERM_ROWS;
        app->tabs[i].cursor_visible = (bool_t)1;
        app->tabs[i].attr = (u16)7;
    }
    app->tab_count = 1;
    ax_strlcpy(app->tabs[0].title, "shell", sizeof(app->tabs[0].title));
    axau_tabbar_init(&app->tabbar,
                     gui_rect(frame.x, frame.y, frame.w, TERM_TAB_HEIGHT));
    axau_tabbar_add(&app->tabbar, "shell", (bool_t)1);
    axau_tabbar_add(&app->tabbar, "+", (bool_t)0);
    app->tabbar.selected = 0;
}

static term_tab_t *term_active(term_app_t *app)
{
    if (!app || app->active_tab < 0 || app->active_tab >= app->tab_count) {
        return app ? &app->tabs[0] : 0;
    }
    return &app->tabs[app->active_tab];
}

i32 term_app_new_tab(term_app_t *app, const char *title)
{
    if (!app || app->tab_count >= TERM_MAX_TABS) return -1;
    i32 index = app->tab_count++;
    term_tab_t *tab = &app->tabs[index];
    ax_memset(tab, 0, sizeof(*tab));
    tab->cols = TERM_COLS;
    tab->rows = TERM_ROWS;
    tab->cursor_visible = (bool_t)1;
    tab->attr = (u16)7;
    ax_strlcpy(tab->title, title ? title : "shell", sizeof(tab->title));
    axau_tabbar_add(&app->tabbar, tab->title, (bool_t)1);
    app->active_tab = index;
    app->tabbar.selected = index;
    return index;
}

void term_app_close_tab(term_app_t *app, i32 index)
{
    if (!app || index < 0 || index >= app->tab_count || app->tab_count <= 1) {
        return;
    }
    for (i32 i = index; i + 1 < app->tab_count; i++) {
        app->tabs[i] = app->tabs[i + 1];
    }
    app->tab_count--;
    if (app->active_tab >= app->tab_count) {
        app->active_tab = app->tab_count - 1;
    }
    app->tabbar.count--;
    app->tabbar.selected = app->active_tab;
}

static void term_scroll_up(term_tab_t *tab)
{
    /* The top line leaves the grid and enters scrollback, then the grid shifts
     * one row and the last row is cleared. */
    i32 slot = (tab->scrollback_head + tab->scrollback_count) % TERM_SCROLLBACK;
    for (i32 x = 0; x < tab->cols; x++) {
        tab->scrollback[slot][x] = tab->grid[0][x];
    }
    if (tab->scrollback_count < TERM_SCROLLBACK) {
        tab->scrollback_count++;
    } else {
        tab->scrollback_head = (tab->scrollback_head + 1) % TERM_SCROLLBACK;
    }
    for (i32 y = 0; y + 1 < tab->rows; y++) {
        for (i32 x = 0; x < tab->cols; x++) {
            tab->grid[y][x] = tab->grid[y + 1][x];
        }
    }
    for (i32 x = 0; x < tab->cols; x++) {
        tab->grid[tab->rows - 1][x].ch = ' ';
        tab->grid[tab->rows - 1][x].attr = tab->attr;
    }
    tab->cursor_y = tab->rows - 1;
}

static void term_advance(term_tab_t *tab)
{
    tab->cursor_x++;
    if (tab->cursor_x >= tab->cols) {
        tab->cursor_x = 0;
        tab->cursor_y++;
        if (tab->cursor_y >= tab->rows) {
            term_scroll_up(tab);
            tab->cursor_y = tab->rows - 1;
        }
    }
}

void term_app_putc(term_app_t *app, char ch)
{
    term_tab_t *tab = term_active(app);
    if (!tab) return;
    switch (ch) {
    case '\r':
        tab->cursor_x = 0;
        return;
    case '\n':
        tab->cursor_y++;
        if (tab->cursor_y >= tab->rows) {
            term_scroll_up(tab);
            tab->cursor_y = tab->rows - 1;
        }
        return;
    case '\b':
        if (tab->cursor_x > 0) tab->cursor_x--;
        tab->grid[tab->cursor_y][tab->cursor_x].ch = ' ';
        return;
    case '\t': {
        i32 target = (tab->cursor_x / 8 + 1) * 8;
        while (tab->cursor_x < target && tab->cursor_x < tab->cols) {
            tab->grid[tab->cursor_y][tab->cursor_x].ch = ' ';
            tab->grid[tab->cursor_y][tab->cursor_x].attr = tab->attr;
            tab->cursor_x++;
        }
        return;
    }
    default:
        break;
    }
    tab->grid[tab->cursor_y][tab->cursor_x].ch = ch;
    tab->grid[tab->cursor_y][tab->cursor_x].attr = tab->attr;
    term_advance(tab);
}

void term_app_write(term_app_t *app, const char *text)
{
    if (!app || !text) return;
    while (*text) {
        term_app_putc(app, *text++);
    }
}

void term_app_set_attr(term_app_t *app, u8 foreground, u8 background,
                       bool_t bold)
{
    term_tab_t *tab = term_active(app);
    if (!tab) return;
    tab->attr = (u16)((foreground & 0x0F) | ((background & 0x0F) << 4) |
                      (bold ? TERM_ATTR_BOLD : 0));
}

void term_app_clear(term_app_t *app)
{
    term_tab_t *tab = term_active(app);
    if (!tab) return;
    for (i32 y = 0; y < tab->rows; y++) {
        for (i32 x = 0; x < tab->cols; x++) {
            tab->grid[y][x].ch = ' ';
            tab->grid[y][x].attr = tab->attr;
        }
    }
    tab->cursor_x = 0;
    tab->cursor_y = 0;
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

static gui_rect_t term_grid_rect(const term_app_t *app)
{
    return gui_rect(app->frame.x, app->frame.y + TERM_TAB_HEIGHT,
                    app->frame.w, app->frame.h - TERM_TAB_HEIGHT -
                                    TERM_STATUS_HEIGHT);
}

void term_app_paint(gui_surface_t *surface, term_app_t *app)
{
    if (!surface || !app) return;
    const axau_context_t *context = axau_context();
    const axau_palette_t *palette = &context->palette;
    term_tab_t *tab = term_active(app);
    if (!tab) return;

    gui_fill_rect(surface, app->frame,
                  axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    axau_tabbar_paint(surface, &app->tabbar, palette, &context->metrics);

    gui_rect_t grid = term_grid_rect(app);
    gui_fill_rect(surface, grid, axau_colour(palette, AXAU_COLOUR_SURFACE));

    i32 columns_fit = grid.w / TERM_CELL_W;
    i32 rows_fit = grid.h / TERM_CELL_H;
    if (columns_fit > tab->cols) columns_fit = tab->cols;
    if (rows_fit > tab->rows) rows_fit = tab->rows;

    i32 top = grid.y + 8;
    for (i32 y = 0; y < rows_fit; y++) {
        i32 line_y = top + y * TERM_CELL_H;
        for (i32 x = 0; x < columns_fit; x++) {
            axterm_cell_t *cell = &tab->grid[y][x];
            if (cell->ch == ' ') {
                continue;
            }
            u32 fg = term_colour(palette, (u8)TERM_ATTR_FG(cell->attr));
            u8 bg_index = (u8)TERM_ATTR_BG(cell->attr);
            if (cell->attr & TERM_ATTR_INVERSE) {
                u32 swapped = fg;
                fg = term_colour(palette, bg_index);
                (void)swapped;
            }
            i32 cell_x = grid.x + 8 + x * TERM_CELL_W;
            { char glyph[2]; glyph[0] = cell->ch; glyph[1] = 0; gui_text(surface, cell_x, line_y, glyph, fg, 1); }
            if (cell->attr & TERM_ATTR_UNDERLINE) {
                gui_hline(surface, cell_x, line_y + TERM_CELL_H - 2, TERM_CELL_W,
                          fg);
            }
            { char glyph[2]; glyph[0] = cell->ch; glyph[1] = 0; gui_text(surface, cell_x, line_y, glyph, fg, 1); }
        }
    }

    /* Selection is painted as a translucent band over the cells; text is
     * re-emitted afterwards so the highlight never obscures the glyphs. */
    if (app->has_selection) {
        i32 y0 = app->select_start_y;
        i32 y1 = app->select_end_y;
        if (y0 > y1) { i32 swap = y0; y0 = y1; y1 = swap; }
        for (i32 y = y0; y <= y1 && y < rows_fit; y++) {
            i32 x0 = y == y0 ? app->select_start_x : 0;
            i32 x1 = y == y1 ? app->select_end_x : columns_fit - 1;
            if (x0 > x1) { i32 swap = x0; x0 = x1; x1 = swap; }
            if (x0 < 0) x0 = 0;
            if (x1 >= columns_fit) x1 = columns_fit - 1;
            gui_blend_rect(surface,
                           gui_rect(grid.x + 8 + x0 * TERM_CELL_W,
                                    top + y * TERM_CELL_H,
                                    (x1 - x0 + 1) * TERM_CELL_W, TERM_CELL_H),
                           axui_colour_opacity(axau_colour(palette, AXAU_COLOUR_ACCENT), 46));
            for (i32 x = x0; x <= x1; x++) {
                axterm_cell_t *cell = &tab->grid[y][x];
                if (cell->ch == ' ') continue;
                char glyph[2];
                glyph[0] = cell->ch;
                glyph[1] = 0;
                gui_text(surface, grid.x + 8 + x * TERM_CELL_W,
                         top + y * TERM_CELL_H, glyph,
                         axau_colour(palette, AXAU_COLOUR_TEXT), 1);
            }
        }
    }

    if (tab->cursor_visible && tab->blink_on && !app->has_selection) {
        gui_rect_t caret = gui_rect(grid.x + 8 + tab->cursor_x * TERM_CELL_W,
                                    top + tab->cursor_y * TERM_CELL_H,
                                    TERM_CELL_W / 2 + 1, TERM_CELL_H / 2);
        gui_fill_rect(surface, caret, axau_colour(palette, AXAU_COLOUR_ACCENT));
    }

    gui_rect_t status = gui_rect(app->frame.x, app->frame.y + app->frame.h -
                                 TERM_STATUS_HEIGHT, app->frame.w,
                                 TERM_STATUS_HEIGHT);
    gui_fill_rect(surface, status,
                  axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    axau_paint_divider(surface, gui_rect(status.x, status.y, status.w, 1),
                       palette);
    char left[48];
    ax_snprintf(left, sizeof(left), "%s  line %d, col %d",
                tab->busy ? "busy" : "ready", tab->cursor_y + 1,
                tab->cursor_x + 1);
    axui_draw_text(surface, status.x + 12, status.y + 4, left,
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 1));
    char right[48];
    ax_snprintf(right, sizeof(right), "UTF-8  %d x %d  %d tabs", tab->cols,
                tab->rows, app->tab_count);
    axui_draw_text_right(surface,
                         gui_rect(status.x, status.y, status.w - 12,
                                  TERM_STATUS_HEIGHT),
                         right,
                         axui_style(axau_colour(palette,
                                                AXAU_COLOUR_TEXT_TERTIARY), 1));
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

i32 term_app_click(term_app_t *app, i32 x, i32 y, u32 button)
{
    if (!app) return 0;
    bool_t close_hit = (bool_t)0;
    i32 tab_index = axau_tabbar_hit(&app->tabbar, x, y, &close_hit);
    if (tab_index >= 0) {
        if (tab_index == app->tabbar.count - 1) {
            term_app_new_tab(app, "shell");
            return 1;
        }
        app->active_tab = tab_index;
        app->tabbar.selected = tab_index;
        app->has_selection = (bool_t)0;
        return 1;
    }
    gui_rect_t grid = term_grid_rect(app);
    if (!gui_rect_contains(grid, x, y)) return 0;
    i32 cell_x = (x - grid.x - 8) / TERM_CELL_W;
    i32 cell_y = (y - grid.y - 8) / TERM_CELL_H;
    if (button == 0x01) {
        app->selecting = (bool_t)1;
        app->select_start_x = cell_x;
        app->select_start_y = cell_y;
        app->select_end_x = cell_x;
        app->select_end_y = cell_y;
        app->has_selection = (bool_t)0;
        return 1;
    }
    return 0;
}

i32 term_app_drag(term_app_t *app, i32 x, i32 y)
{
    if (!app || !app->selecting) return 0;
    gui_rect_t grid = term_grid_rect(app);
    i32 cell_x = (x - grid.x - 8) / TERM_CELL_W;
    i32 cell_y = (y - grid.y - 8) / TERM_CELL_H;
    if (cell_x < 0) cell_x = 0;
    if (cell_y < 0) cell_y = 0;
    app->select_end_x = cell_x;
    app->select_end_y = cell_y;
    if (cell_x != app->select_start_x || cell_y != app->select_start_y) {
        app->has_selection = (bool_t)1;
    }
    return 1;
}

i32 term_app_release(term_app_t *app)
{
    if (!app) return 0;
    bool_t was = app->selecting;
    app->selecting = (bool_t)0;
    return was;
}

/* Copy the selected cells into a caller-supplied buffer, newline-terminated per
 * row.  Returns the byte count written. */
i32 term_app_copy_selection(term_app_t *app, char *out, i32 capacity)
{
    term_tab_t *tab = term_active(app);
    if (!tab || !out || capacity <= 0 || !app->has_selection) return 0;
    i32 y0 = app->select_start_y;
    i32 y1 = app->select_end_y;
    if (y0 > y1) { i32 swap = y0; y0 = y1; y1 = swap; }
    i32 written = 0;
    for (i32 y = y0; y <= y1 && y < tab->rows; y++) {
        i32 x0 = y == y0 ? app->select_start_x : 0;
        i32 x1 = y == y1 ? app->select_end_x : tab->cols - 1;
        if (x0 > x1) { i32 swap = x0; x0 = x1; x1 = swap; }
        for (i32 x = x0; x <= x1 && x < tab->cols; x++) {
            if (written + 1 >= capacity) break;
            out[written++] = tab->grid[y][x].ch;
        }
        if (written + 1 < capacity) {
            out[written++] = '\n';
        }
    }
    out[written] = 0;
    return written;
}

void term_app_cycle_tab(term_app_t *app, i32 delta)
{
    if (!app || app->tab_count <= 0) return;
    app->active_tab = (app->active_tab + delta + app->tab_count) % app->tab_count;
    app->tabbar.selected = app->active_tab;
    app->has_selection = (bool_t)0;
}

void term_app_resize(term_app_t *app, gui_rect_t frame)
{
    if (!app) return;
    app->frame = frame;
    app->tabbar.rect = gui_rect(frame.x, frame.y, frame.w, TERM_TAB_HEIGHT);
    term_tab_t *tab = term_active(app);
    if (tab) {
        i32 columns = (frame.w - 16) / TERM_CELL_W;
        i32 rows = (frame.h - TERM_TAB_HEIGHT - TERM_STATUS_HEIGHT - 16) /
                   TERM_CELL_H;
        if (columns > 0 && columns < TERM_COLS) tab->cols = columns;
        if (rows > 0 && rows < TERM_ROWS) tab->rows = rows;
    }
}

void term_app_dump(const term_app_t *app)
{
    if (!app) return;
    const term_tab_t *tab = &app->tabs[app->active_tab];
    printk("terminal: tab %d/%d cursor %d,%d cols=%d rows=%d scrollback=%d\n",
           app->active_tab + 1, app->tab_count, tab->cursor_x, tab->cursor_y,
           tab->cols, tab->rows, tab->scrollback_count);
}



