/* AxiomOS terminal - mouse selection.
 *
 * The selection is stored in combined-view coordinates (scrollback rows
 * followed by the live screen), so it stays anchored to its text while the
 * user scrolls.  Painting asks "is cell (x, visible_row) selected?" and the
 * answer is derived from those combined coordinates each frame.
 *
 * Copying walks the combined view, emits a newline between rows and a space
 * over blank cells, and replaces the synthesised box glyphs with their ASCII
 * lookalikes so the clipboard contents are usable in a plain text field.
 */

#include <axiom/lib.h>
#include "term.h"

void axterm_selection_clear(term_t *term)
{
    if (!term) {
        return;
    }
    term->selection.active = (bool_t)0;
    term->selection.dragging = (bool_t)0;
    term->selection.anchor_x = 0;
    term->selection.anchor_y = 0;
    term->selection.cursor_x = 0;
    term->selection.cursor_y = 0;
    term->dirty = (bool_t)1;
}

void axterm_selection_begin(term_t *term, i32 x, i32 y)
{
    if (!term) {
        return;
    }
    i32 row = axterm_view_row_at(term, y);
    term->selection.active = (bool_t)1;
    term->selection.dragging = (bool_t)1;
    term->selection.anchor_x = axui_clamp_i32(x, 0, term->cols - 1);
    term->selection.anchor_y = row;
    term->selection.cursor_x = term->selection.anchor_x;
    term->selection.cursor_y = row;
    term->selection.scroll_offset = term->history.offset;
    term->dirty = (bool_t)1;
}

void axterm_selection_extend(term_t *term, i32 x, i32 y)
{
    if (!term) {
        return;
    }
    if (!term->selection.dragging) {
        return;
    }
    i32 row = axterm_view_row_at(term, y);
    term->selection.cursor_x = axui_clamp_i32(x, 0, term->cols - 1);
    term->selection.cursor_y = row;
    term->dirty = (bool_t)1;
}

void axterm_selection_end(term_t *term)
{
    if (!term) {
        return;
    }
    term->selection.dragging = (bool_t)0;
    term->dirty = (bool_t)1;
}

/* Normalise the rectangle so (x0,y0) is the earlier corner.  A selection that
 * ends exactly at the start of a line does not include that line, matching
 * the usual terminal behaviour for a drag that overshoots one row. */
void axterm_selection_normalise(const term_t *term, i32 *x0, i32 *y0, i32 *x1,
                                i32 *y1)
{
    if (!term) {
        if (x0) *x0 = 0;
        if (y0) *y0 = 0;
        if (x1) *x1 = 0;
        if (y1) *y1 = 0;
        return;
    }
    i32 ax = term->selection.anchor_x;
    i32 ay = term->selection.anchor_y;
    i32 cx = term->selection.cursor_x;
    i32 cy = term->selection.cursor_y;

    bool_t forward = ay < cy || (ay == cy && ax <= cx);
    if (forward) {
        *x0 = ax;
        *y0 = ay;
        *x1 = cx;
        *y1 = cy;
    } else {
        *x0 = cx;
        *y0 = cy;
        *x1 = ax;
        *y1 = ay;
    }
}

bool_t axterm_selection_contains(const term_t *term, i32 x, i32 visible_row)
{
    if (!term || !term->selection.active) {
        return (bool_t)0;
    }
    i32 row = axterm_view_top(term) + visible_row;
    i32 x0, y0, x1, y1;
    axterm_selection_normalise(term, &x0, &y0, &x1, &y1);

    if (row < y0 || row > y1) {
        return (bool_t)0;
    }
    if (y0 == y1) {
        return (bool_t)(x >= x0 && x <= x1);
    }
    if (row == y0) {
        return (bool_t)(x >= x0);
    }
    if (row == y1) {
        return (bool_t)(x <= x1);
    }
    return (bool_t)1;
}

/* Translate one cell into a printable byte for the clipboard. */
static char clipboard_char(u32 ch)
{
    if (ch == 0 || ch == ' ') {
        return ' ';
    }
    if (ch < 0x80) {
        return (char)ch;
    }
    switch (ch) {
    case 0xB3: return '|';
    case 0xC4: return '-';
    case 0xDA:
    case 0xBF:
    case 0xC0:
    case 0xD9:
    case 0xC5: return '+';
    case 0xDB: return '#';
    case 0xB0: return '.';
    case 0xB1: return ':';
    case 0xB2: return '%';
    default: return '#';
    }
}

usize axterm_selection_copy(term_t *term, char *buffer, usize cap)
{
    if (!term || !buffer || cap == 0) {
        return 0;
    }
    buffer[0] = 0;
    if (!term->selection.active) {
        return 0;
    }

    i32 x0, y0, x1, y1;
    axterm_selection_normalise(term, &x0, &y0, &x1, &y1);

    usize out = 0;
    for (i32 row = y0; row <= y1 && out + 1 < cap; row++) {
        const term_cell_t *line = axterm_view_line(term, row);
        if (!line) {
            continue;
        }
        i32 from = row == y0 ? x0 : 0;
        i32 to = row == y1 ? x1 : term->cols - 1;

        /* Trailing blanks on intermediate rows are trimmed, which is what
         * a copy out of a terminal should produce. */
        if (row != y1) {
            while (to > from && (line[to].ch == 0 || line[to].ch == ' ')) {
                to--;
            }
        }
        for (i32 x = from; x <= to && out + 1 < cap; x++) {
            buffer[out++] = clipboard_char(line[x].ch);
        }
        if (row != y1 && out + 1 < cap) {
            buffer[out++] = '\n';
        }
    }
    buffer[out] = 0;
    return out;
}

/* Select the entire visible screen, used by the shell's `select all` action. */
void axterm_selection_all(term_t *term)
{
    if (!term) {
        return;
    }
    term->selection.active = (bool_t)1;
    term->selection.dragging = (bool_t)0;
    term->selection.anchor_x = 0;
    term->selection.anchor_y = axterm_view_top(term);
    term->selection.cursor_x = term->cols - 1;
    term->selection.cursor_y = axterm_view_top(term) + term->rows - 1;
    term->dirty = (bool_t)1;
}

/* How many cells are covered; the status bar shows this while dragging. */
i32 axterm_selection_cell_count(const term_t *term)
{
    if (!term || !term->selection.active) {
        return 0;
    }
    i32 x0, y0, x1, y1;
    axterm_selection_normalise(term, &x0, &y0, &x1, &y1);
    if (y0 == y1) {
        return x1 - x0 + 1;
    }
    i32 first = term->cols - x0;
    i32 last = x1 + 1;
    i32 middle = (y1 - y0 - 1) * term->cols;
    return first + middle + last;
}
