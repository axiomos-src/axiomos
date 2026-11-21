/* AxiomOS terminal - screen buffer operations.
 *
 * All of the grid mutations live here: erasing, scrolling, inserting and
 * deleting lines or characters, cursor motion and the alternate screen.  The
 * parser calls into these functions and never touches a cell directly, which
 * keeps the invariants (cursor clamped to the scroll region, wrap pending
 * state cleared on every absolute move) in one place.
 *
 * The buffer is two fixed arrays plus a stride of TERM_MAX_COLS.  Rows are
 * always allocated at the maximum width so a resize is just a change of
 * `cols`/`rows` plus a clamp on the cursor; no cell moves, and the alternate
 * screen keeps its own copy.
 */

#include <axiom/lib.h>
#include "term.h"

/* ------------------------------------------------------------------ */
/* Cell helpers                                                        */
/* ------------------------------------------------------------------ */

term_cell_t *term_cell_at(term_t *term, i32 x, i32 y)
{
    if (!term || !term->cells) {
        return 0;
    }
    if (x < 0 || y < 0 || x >= term->cols || y >= term->rows) {
        return 0;
    }
    return &term->cells[(usize)y * TERM_MAX_COLS + (usize)x];
}

/* Write a run of blank cells in the current attribute.  Used by every erase
 * and scroll path; the background colour is what actually shows, so the cell
 * keeps the current background rather than the theme's. */
void term_blank_run(term_t *term, i32 x, i32 y, i32 count)
{
    if (!term || count <= 0) {
        return;
    }
    if (y < 0 || y >= term->rows) {
        return;
    }
    if (x < 0) {
        count += x;
        x = 0;
    }
    if (x + count > term->cols) {
        count = term->cols - x;
    }
    if (count <= 0) {
        return;
    }
    term_cell_t *row = &term->cells[(usize)y * TERM_MAX_COLS + (usize)x];
    for (i32 i = 0; i < count; i++) {
        row[i].ch = 0;
        row[i].fg = term->cursor.fg;
        row[i].bg = term->cursor.bg;
        row[i].flags = term->cursor.flags;
        row[i].wide = 0;
    }
}

/* Copy a cell run without changing attributes; used by insert/delete and by
 * the alternate-screen switch. */
static void term_copy_run(term_cell_t *dst, const term_cell_t *src, i32 count)
{
    if (count <= 0) {
        return;
    }
    ax_memmove(dst, src, (usize)count * sizeof(term_cell_t));
}

/* ------------------------------------------------------------------ */
/* Reset and resize                                                    */
/* ------------------------------------------------------------------ */

void axterm_reset(term_t *term)
{
    if (!term) {
        return;
    }

    term->cols = term->cols > 0 ? term->cols : TERM_DEFAULT_COLS;
    term->rows = term->rows > 0 ? term->rows : TERM_DEFAULT_ROWS;
    if (term->cols > TERM_MAX_COLS) {
        term->cols = TERM_MAX_COLS;
    }
    if (term->rows > TERM_MAX_ROWS) {
        term->rows = TERM_MAX_ROWS;
    }

    term->cells = &term->main_grid[0][0];
    term->modes = TERM_MODE_AUTOWRAP | TERM_MODE_CURSOR_BLINK;

    term->default_fg = axui_rgba(0xD8, 0xDE, 0xE9, 255);
    term->default_bg = axui_rgba(0x0A, 0x0D, 0x16, 255);
    term->cursor_colour = axui_rgba(0x9C, 0xD8, 0xFF, 255);
    term->default_flags = 0;

    axterm_reset_attributes(term);

    term->cursor.x = 0;
    term->cursor.y = 0;
    term->cursor.visible = (bool_t)1;
    term->cursor.pending_wrap = (bool_t)0;

    term->saved_cursor_main = term->cursor;
    term->saved_cursor_alt = term->cursor;

    term->scroll_top = 0;
    term->scroll_bottom = term->rows - 1;

    /* Reset tab stops to the classic every-eight-columns pattern. */
    term->tab_count = 0;
    for (i32 x = TERM_TAB_EVERY; x < term->cols; x += TERM_TAB_EVERY) {
        if (term->tab_count < TERM_MAX_TABS) {
            term->tab_stops[term->tab_count++] = x;
        }
    }

    term->state = TERM_STATE_GROUND;
    term->csi_param_count = 0;
    term->csi_private = (bool_t)0;
    term->csi_intermediate = 0;
    term->osc_length = 0;
    term->g0_charset = 0;
    term->g1_charset = 0;
    term->active_charset = 0;

    term->dirty = (bool_t)1;
    term->scrolled = (bool_t)0;
    term->title_changed = (bool_t)0;
    term->bytes_in = 0;
    term->lines_out = 0;
    term->escape_count = 0;
    term->bell_count = 0;

    term->selection.active = (bool_t)0;
    term->selection.dragging = (bool_t)0;

    axterm_clear_all(term);
}

void axterm_init(term_t *term)
{
    if (!term) {
        return;
    }
    ax_memset(term, 0, sizeof(*term));
    term->cols = TERM_DEFAULT_COLS;
    term->rows = TERM_DEFAULT_ROWS;
    axterm_history_init(term);
    axterm_reset(term);
}

void axterm_destroy(term_t *term)
{
    if (!term) {
        return;
    }
    term->cells = 0;
    ax_memset(term, 0, sizeof(*term));
}

/* Resizing keeps the top-left corner of the buffer and clamps the cursor.
 * Lines pushed out of the bottom go to the scrollback so a shrinking window
 * does not silently lose output. */
void axterm_resize(term_t *term, i32 cols, i32 rows)
{
    if (!term) {
        return;
    }
    if (cols < 8) {
        cols = 8;
    }
    if (rows < 3) {
        rows = 3;
    }
    if (cols > TERM_MAX_COLS) {
        cols = TERM_MAX_COLS;
    }
    if (rows > TERM_MAX_ROWS) {
        rows = TERM_MAX_ROWS;
    }
    if (cols == term->cols && rows == term->rows) {
        return;
    }

    /* Grow: blank the newly exposed columns and rows. */
    if (cols > term->cols) {
        for (i32 y = 0; y < term->rows; y++) {
            for (i32 x = term->cols; x < cols; x++) {
                term_cell_t *cell = &term->cells[(usize)y * TERM_MAX_COLS + (usize)x];
                cell->ch = 0;
                cell->fg = term->default_fg;
                cell->bg = term->default_bg;
                cell->flags = 0;
                cell->wide = 0;
            }
        }
    }
    if (rows > term->rows) {
        for (i32 y = term->rows; y < rows; y++) {
            for (i32 x = 0; x < cols; x++) {
                term_cell_t *cell = &term->cells[(usize)y * TERM_MAX_COLS + (usize)x];
                cell->ch = 0;
                cell->fg = term->default_fg;
                cell->bg = term->default_bg;
                cell->flags = 0;
                cell->wide = 0;
            }
        }
    }
    if (rows < term->rows) {
        /* Push the lines that will fall off the bottom into the history. */
        i32 dropped = term->rows - rows;
        for (i32 i = 0; i < dropped; i++) {
            i32 line = rows + i;
            axterm_history_push_line(term, &term->cells[(usize)line * TERM_MAX_COLS],
                                     term->cols);
        }
    }

    term->cols = cols;
    term->rows = rows;

    if (term->cursor.x >= cols) {
        term->cursor.x = cols - 1;
    }
    if (term->cursor.y >= rows) {
        term->cursor.y = rows - 1;
    }
    term->cursor.pending_wrap = (bool_t)0;

    term->scroll_top = 0;
    term->scroll_bottom = rows - 1;

    /* Rebuild tab stops for the new width. */
    term->tab_count = 0;
    for (i32 x = TERM_TAB_EVERY; x < cols; x += TERM_TAB_EVERY) {
        if (term->tab_count < TERM_MAX_TABS) {
            term->tab_stops[term->tab_count++] = x;
        }
    }

    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Cursor                                                              */
/* ------------------------------------------------------------------ */

void axterm_set_cursor(term_t *term, i32 x, i32 y)
{
    if (!term) {
        return;
    }
    if (term->modes & TERM_MODE_ORIGIN) {
        y += term->scroll_top;
    }
    term->cursor.x = axui_clamp_i32(x, 0, term->cols - 1);
    term->cursor.y = axui_clamp_i32(y, 0, term->rows - 1);
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

void axterm_move_relative(term_t *term, i32 dx, i32 dy)
{
    if (!term) {
        return;
    }
    axterm_set_cursor(term, term->cursor.x + dx, term->cursor.y + dy);
}

void axterm_save_cursor(term_t *term)
{
    if (!term) {
        return;
    }
    if (term->modes & TERM_MODE_ALT_SCREEN) {
        term->saved_cursor_alt = term->cursor;
    } else {
        term->saved_cursor_main = term->cursor;
    }
}

void axterm_restore_cursor(term_t *term)
{
    if (!term) {
        return;
    }
    if (term->modes & TERM_MODE_ALT_SCREEN) {
        term->cursor = term->saved_cursor_alt;
    } else {
        term->cursor = term->saved_cursor_main;
    }
    term->cursor.x = axui_clamp_i32(term->cursor.x, 0, term->cols - 1);
    term->cursor.y = axui_clamp_i32(term->cursor.y, 0, term->rows - 1);
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

/* Map the DEC special graphics set to the box drawing code points the painter
 * knows how to synthesise. */
static u16 dec_graphics(u16 ch)
{
    switch (ch) {
    case 'j': return 0xD9;   /* lower right corner */
    case 'k': return 0xBF;   /* upper right corner */
    case 'l': return 0xDA;   /* upper left corner */
    case 'm': return 0xC0;   /* lower left corner */
    case 'n': return 0xC5;   /* crossing */
    case 'q': return 0xC4;   /* horizontal */
    case 't': return 0xC3;   /* left tee */
    case 'u': return 0xB4;   /* right tee */
    case 'v': return 0xB3;   /* vertical */
    case 'w': return 0xB4;
    case 'x': return 0xB3;
    case 'a': return 0xB0;   /* checker board */
    case '~': return 0xB2;   /* bullet / shade */
    case '`': return 0xB1;
    case '0': return 0xDB;   /* full block */
    case 'f': return 0x27;   /* apostrophe */
    case 'g': return 0x2A;   /* degree-ish */
    case 'o': return 0x2D;
    case 's': return 0x5F;
    default: break;
    }
    return ch;
}

void axterm_put_cell(term_t *term, u32 ch, u8 flags)
{
    if (!term) {
        return;
    }

    if (ch < 0x20) {
        return;
    }

    /* Translate the active character set. */
    if (term->active_charset == 0 && term->g0_charset == 1) {
        if (ch < 0x80) {
            ch = dec_graphics((u16)ch);
        }
    } else if (term->active_charset == 1 && term->g1_charset == 1) {
        if (ch < 0x80) {
            ch = dec_graphics((u16)ch);
        }
    }

    /* Deferred wrap: a character arriving while the cursor sits on the last
     * column moves to the next line first. */
    if (term->cursor.pending_wrap) {
        term->cursor.pending_wrap = (bool_t)0;
        if (term->modes & TERM_MODE_AUTOWRAP) {
            axterm_newline(term);
            term->cursor.x = 0;
        }
    }

    if (term->cursor.x >= term->cols) {
        if (term->modes & TERM_MODE_AUTOWRAP) {
            axterm_newline(term);
            term->cursor.x = 0;
        } else {
            term->cursor.x = term->cols - 1;
        }
    }

    /* Insert mode shifts the rest of the line right by one. */
    if (term->modes & TERM_MODE_INSERT) {
        term_cell_t *row = &term->cells[(usize)term->cursor.y * TERM_MAX_COLS];
        for (i32 x = term->cols - 1; x > term->cursor.x; x--) {
            row[x] = row[x - 1];
        }
    }

    term_cell_t *cell = &term->cells[(usize)term->cursor.y * TERM_MAX_COLS +
                                     (usize)term->cursor.x];
    cell->ch = (u16)ch;
    cell->fg = term->cursor.fg;
    cell->bg = term->cursor.bg;
    cell->flags = (u8)(term->cursor.flags | flags);
    cell->wide = 0;

    term->cursor.x++;
    if (term->cursor.x >= term->cols) {
        term->cursor.x = term->cols - 1;
        term->cursor.pending_wrap = (bool_t)1;
    }
    term->dirty = (bool_t)1;
}

void axterm_newline(term_t *term)
{
    if (!term) {
        return;
    }
    if (term->cursor.y == term->scroll_bottom) {
        /* The region scrolls, and only a full-screen region feeds the
         * scrollback (an application's own scroll region would otherwise
         * pollute it with redrawn panels). */
        axterm_scroll_region_up(term, 1,
                                term->scroll_top == 0 &&
                                    term->scroll_bottom == term->rows - 1);
    } else if (term->cursor.y < term->rows - 1) {
        term->cursor.y++;
    }
    term->cursor.x = 0;
    term->cursor.pending_wrap = (bool_t)0;
    term->lines_out++;
    term->dirty = (bool_t)1;
}

void axterm_carriage_return(term_t *term)
{
    if (!term) {
        return;
    }
    term->cursor.x = 0;
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

void axterm_backspace(term_t *term)
{
    if (!term) {
        return;
    }
    if (term->cursor.x > 0) {
        term->cursor.x--;
    }
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

void axterm_reverse_index(term_t *term)
{
    if (!term) {
        return;
    }
    if (term->cursor.y == term->scroll_top) {
        axterm_scroll_region_down(term, 1);
    } else if (term->cursor.y > 0) {
        term->cursor.y--;
    }
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Tabs                                                                */
/* ------------------------------------------------------------------ */

static i32 next_tab_stop(const term_t *term, i32 from)
{
    for (i32 i = 0; i < term->tab_count; i++) {
        if (term->tab_stops[i] > from) {
            return term->tab_stops[i];
        }
    }
    return term->cols - 1;
}

static i32 prev_tab_stop(const term_t *term, i32 from)
{
    for (i32 i = term->tab_count - 1; i >= 0; i--) {
        if (term->tab_stops[i] < from) {
            return term->tab_stops[i];
        }
    }
    return 0;
}

void axterm_tab(term_t *term, i32 count)
{
    if (!term) {
        return;
    }
    if (count <= 0) {
        count = 1;
    }
    for (i32 i = 0; i < count; i++) {
        term->cursor.x = next_tab_stop(term, term->cursor.x);
        if (term->cursor.x >= term->cols) {
            term->cursor.x = term->cols - 1;
            break;
        }
    }
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

void axterm_back_tab(term_t *term, i32 count)
{
    if (!term) {
        return;
    }
    if (count <= 0) {
        count = 1;
    }
    for (i32 i = 0; i < count; i++) {
        term->cursor.x = prev_tab_stop(term, term->cursor.x);
    }
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

void axterm_clear_tabs(term_t *term, bool_t all)
{
    if (!term) {
        return;
    }
    if (all) {
        term->tab_count = 0;
    } else {
        /* Clear the stop at (or immediately before) the cursor. */
        for (i32 i = 0; i < term->tab_count; i++) {
            if (term->tab_stops[i] == term->cursor.x) {
                for (i32 j = i; j + 1 < term->tab_count; j++) {
                    term->tab_stops[j] = term->tab_stops[j + 1];
                }
                term->tab_count--;
                break;
            }
        }
    }
}

void axterm_set_tab_stop(term_t *term)
{
    if (!term || term->tab_count >= TERM_MAX_TABS) {
        return;
    }
    term->tab_stops[term->tab_count++] = term->cursor.x;
}

/* ------------------------------------------------------------------ */
/* Erasing                                                             */
/* ------------------------------------------------------------------ */

void axterm_clear_all(term_t *term)
{
    if (!term) {
        return;
    }
    for (i32 y = 0; y < TERM_MAX_ROWS; y++) {
        term_blank_run(term, 0, y, TERM_MAX_COLS);
    }
    term->dirty = (bool_t)1;
}

/* mode 0: cursor to end of line, 1: start of line to cursor, 2: whole line. */
void axterm_clear_line(term_t *term, i32 mode)
{
    if (!term) {
        return;
    }
    switch (mode) {
    case 0:
        term_blank_run(term, term->cursor.x, term->cursor.y,
                       term->cols - term->cursor.x);
        break;
    case 1:
        term_blank_run(term, 0, term->cursor.y, term->cursor.x + 1);
        break;
    case 2:
        term_blank_run(term, 0, term->cursor.y, term->cols);
        break;
    default:
        break;
    }
    term->dirty = (bool_t)1;
}

/* mode 0: cursor to end of screen, 1: start of screen to cursor, 2: all.
 * Mode 3 additionally clears the scrollback, which the shell's `clear`
 * command relies on. */
void axterm_clear_screen(term_t *term, i32 mode)
{
    if (!term) {
        return;
    }
    switch (mode) {
    case 0:
        term_blank_run(term, term->cursor.x, term->cursor.y,
                       term->cols - term->cursor.x);
        for (i32 y = term->cursor.y + 1; y < term->rows; y++) {
            term_blank_run(term, 0, y, term->cols);
        }
        break;
    case 1:
        for (i32 y = 0; y < term->cursor.y; y++) {
            term_blank_run(term, 0, y, term->cols);
        }
        term_blank_run(term, 0, term->cursor.y, term->cursor.x + 1);
        break;
    case 2:
        for (i32 y = 0; y < term->rows; y++) {
            term_blank_run(term, 0, y, term->cols);
        }
        break;
    default:
        break;
    }
    if (mode == 3) {
        axterm_clear_all(term);
        axterm_history_clear(term);
    }
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

void axterm_erase_chars(term_t *term, i32 count)
{
    if (!term || count <= 0) {
        return;
    }
    if (term->cursor.x + count > term->cols) {
        count = term->cols - term->cursor.x;
    }
    term_blank_run(term, term->cursor.x, term->cursor.y, count);
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

void axterm_scroll_region_up(term_t *term, i32 count, bool_t save_history)
{
    if (!term || count <= 0) {
        return;
    }
    i32 top = term->scroll_top;
    i32 bottom = term->scroll_bottom;
    i32 span = bottom - top + 1;
    if (count > span) {
        count = span;
    }

    if (save_history && top == 0 && bottom == term->rows - 1) {
        for (i32 i = 0; i < count; i++) {
            axterm_history_push_line(term, &term->cells[(usize)(top + i) * TERM_MAX_COLS],
                                     term->cols);
        }
        term->scrolled = (bool_t)1;
    }

    /* Shift the surviving rows up, then blank the tail of the region. */
    usize stride = TERM_MAX_COLS;
    if (count < span) {
        ax_memmove(&term->cells[(usize)top * stride],
                   &term->cells[(usize)(top + count) * stride],
                   (usize)(span - count) * stride * sizeof(term_cell_t));
    }
    for (i32 y = bottom - count + 1; y <= bottom; y++) {
        term_blank_run(term, 0, y, term->cols);
    }
    term->dirty = (bool_t)1;
}

void axterm_scroll_region_down(term_t *term, i32 count)
{
    if (!term || count <= 0) {
        return;
    }
    i32 top = term->scroll_top;
    i32 bottom = term->scroll_bottom;
    i32 span = bottom - top + 1;
    if (count > span) {
        count = span;
    }

    usize stride = TERM_MAX_COLS;
    if (count < span) {
        ax_memmove(&term->cells[(usize)(top + count) * stride],
                   &term->cells[(usize)top * stride],
                   (usize)(span - count) * stride * sizeof(term_cell_t));
    }
    for (i32 y = top; y < top + count; y++) {
        term_blank_run(term, 0, y, term->cols);
    }
    term->dirty = (bool_t)1;
}

/* Full-screen scroll used by the mouse wheel: positive `count` looks back in
 * the scrollback, so the view scrolls down. */
void axterm_scroll_view(term_t *term, i32 lines)
{
    if (!term || lines == 0) {
        return;
    }
    i32 available = axterm_history_count(term);
    i32 current = term->history.offset;

    current += lines;
    if (current < 0) {
        current = 0;
    }
    if (current > available) {
        current = available;
    }
    if (current == term->history.offset) {
        return;
    }
    term->history.offset = current;
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Line and character insertion                                        */
/* ------------------------------------------------------------------ */

void axterm_insert_lines(term_t *term, i32 count)
{
    if (!term || count <= 0) {
        return;
    }
    i32 top = term->cursor.y;
    if (top < term->scroll_top || top > term->scroll_bottom) {
        return;
    }
    i32 bottom = term->scroll_bottom;
    i32 span = bottom - top + 1;
    if (count > span) {
        count = span;
    }

    usize stride = TERM_MAX_COLS;
    if (count < span) {
        ax_memmove(&term->cells[(usize)(top + count) * stride],
                   &term->cells[(usize)top * stride],
                   (usize)(span - count) * stride * sizeof(term_cell_t));
    }
    for (i32 y = top; y < top + count; y++) {
        term_blank_run(term, 0, y, term->cols);
    }
    term->cursor.x = 0;
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

void axterm_delete_lines(term_t *term, i32 count)
{
    if (!term || count <= 0) {
        return;
    }
    i32 top = term->cursor.y;
    if (top < term->scroll_top || top > term->scroll_bottom) {
        return;
    }
    i32 bottom = term->scroll_bottom;
    i32 span = bottom - top + 1;
    if (count > span) {
        count = span;
    }

    usize stride = TERM_MAX_COLS;
    if (count < span) {
        ax_memmove(&term->cells[(usize)top * stride],
                   &term->cells[(usize)(top + count) * stride],
                   (usize)(span - count) * stride * sizeof(term_cell_t));
    }
    for (i32 y = bottom - count + 1; y <= bottom; y++) {
        term_blank_run(term, 0, y, term->cols);
    }
    term->cursor.x = 0;
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

void axterm_insert_chars(term_t *term, i32 count)
{
    if (!term || count <= 0) {
        return;
    }
    i32 x = term->cursor.x;
    if (x >= term->cols) {
        return;
    }
    if (count > term->cols - x) {
        count = term->cols - x;
    }
    term_cell_t *row = &term->cells[(usize)term->cursor.y * TERM_MAX_COLS];
    i32 tail = term->cols - x - count;
    if (tail > 0) {
        term_copy_run(&row[x + count], &row[x], tail);
    }
    term_blank_run(term, x, term->cursor.y, count);
    term->dirty = (bool_t)1;
}

void axterm_delete_chars(term_t *term, i32 count)
{
    if (!term || count <= 0) {
        return;
    }
    i32 x = term->cursor.x;
    if (x >= term->cols) {
        return;
    }
    if (count > term->cols - x) {
        count = term->cols - x;
    }
    term_cell_t *row = &term->cells[(usize)term->cursor.y * TERM_MAX_COLS];
    i32 tail = term->cols - x - count;
    if (tail > 0) {
        term_copy_run(&row[x], &row[x + count], tail);
    }
    term_blank_run(term, term->cols - count, term->cursor.y, count);
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Scroll regions and screens                                          */
/* ------------------------------------------------------------------ */

void axterm_set_scroll_region(term_t *term, i32 top, i32 bottom)
{
    if (!term) {
        return;
    }
    /* Both bounds are 1-based from the wire; -1 means "unchanged". */
    if (top < 1) {
        top = 1;
    }
    if (bottom < 1 || bottom > term->rows) {
        bottom = term->rows;
    }
    if (top >= bottom) {
        return;
    }
    term->scroll_top = top - 1;
    term->scroll_bottom = bottom - 1;

    if (term->modes & TERM_MODE_ORIGIN) {
        term->cursor.x = 0;
        term->cursor.y = term->scroll_top;
    }
    term->cursor.pending_wrap = (bool_t)0;
    term->dirty = (bool_t)1;
}

void axterm_reset_scroll_region(term_t *term)
{
    if (!term) {
        return;
    }
    term->scroll_top = 0;
    term->scroll_bottom = term->rows - 1;
    if (term->modes & TERM_MODE_ORIGIN) {
        term->cursor.x = 0;
        term->cursor.y = 0;
    }
    term->dirty = (bool_t)1;
}

/* The alternate screen keeps its own cursor and an independent grid, which
 * is what full-screen editors expect when they enter and leave. */
void axterm_switch_screen(term_t *term, bool_t alternate)
{
    if (!term) {
        return;
    }
    bool_t currently_alt = (bool_t)((term->modes & TERM_MODE_ALT_SCREEN) != 0);
    if (currently_alt == alternate) {
        return;
    }

    if (alternate) {
        term->saved_cursor_main = term->cursor;
        term->modes |= TERM_MODE_ALT_SCREEN;
        term->cells = &term->alt_grid[0][0];
        axterm_reset_attributes(term);
        term->cursor.x = 0;
        term->cursor.y = 0;
        term->cursor.pending_wrap = (bool_t)0;
        for (i32 y = 0; y < term->rows; y++) {
            term_blank_run(term, 0, y, term->cols);
        }
        /* The alternate screen has no scroll region of its own. */
        term->scroll_top = 0;
        term->scroll_bottom = term->rows - 1;
    } else {
        term->modes &= ~TERM_MODE_ALT_SCREEN;
        term->cells = &term->main_grid[0][0];
        term->cursor = term->saved_cursor_main;
        term->cursor.x = axui_clamp_i32(term->cursor.x, 0, term->cols - 1);
        term->cursor.y = axui_clamp_i32(term->cursor.y, 0, term->rows - 1);
        term->cursor.pending_wrap = (bool_t)0;
        term->scroll_top = 0;
        term->scroll_bottom = term->rows - 1;
    }
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Title and status                                                    */
/* ------------------------------------------------------------------ */

void axterm_set_title(term_t *term, const char *text)
{
    if (!term || !text) {
        return;
    }
    ax_strlcpy(term->title, text, sizeof(term->title));
    term->title_changed = (bool_t)1;
}

void axterm_set_status(term_t *term, const char *text)
{
    if (!term || !text) {
        return;
    }
    ax_strlcpy(term->status, text, sizeof(term->status));
}

/* ------------------------------------------------------------------ */
/* Whole-line text extraction                                          */
/* ------------------------------------------------------------------ */

/* Copy one logical row into a NUL-terminated string, trimming trailing
 * blanks.  Used by the selection clipper and by the shell's `dump` command. */
usize axterm_line_text(const term_t *term, i32 y, char *buffer, usize cap)
{
    if (!term || !buffer || cap == 0) {
        return 0;
    }
    if (y < 0 || y >= term->rows) {
        buffer[0] = 0;
        return 0;
    }
    i32 end = term->cols;
    const term_cell_t *row = &term->cells[(usize)y * TERM_MAX_COLS];
    while (end > 0 && (row[end - 1].ch == 0 || row[end - 1].ch == ' ')) {
        end--;
    }
    usize out = 0;
    for (i32 x = 0; x < end && out + 1 < cap; x++) {
        u32 ch = row[x].ch ? row[x].ch : ' ';
        if (ch < 0x80) {
            buffer[out++] = (char)ch;
        } else {
            /* The grid stores box-drawing and block characters; their ASCII
             * stand-ins keep the copied text readable in a plain text field. */
            switch (ch) {
            case 0xB3: buffer[out++] = '|'; break;
            case 0xC4: buffer[out++] = '-'; break;
            case 0xDA: buffer[out++] = '+'; break;
            case 0xBF: buffer[out++] = '+'; break;
            case 0xC0: buffer[out++] = '+'; break;
            case 0xD9: buffer[out++] = '+'; break;
            case 0xC5: buffer[out++] = '+'; break;
            case 0xDB: buffer[out++] = '#'; break;
            default:   buffer[out++] = '#'; break;
            }
        }
    }
    buffer[out] = 0;
    return out;
}
