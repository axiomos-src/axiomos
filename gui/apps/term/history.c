/* AxiomOS terminal - scrollback ring.
 *
 * The ring stores whole rows as they leave the top of the screen.  Indexing is
 * chronological (0 is the oldest retained line) so the painter can walk it
 * with a simple loop, while the write side stays O(1) except when the ring is
 * full, where it copies the remaining rows down by one.
 *
 * Copying 120 cells is cheap enough that a true circular index would only add
 * complexity; the choice also means the oldest line is always at index 0,
 * which the selection maths depends on.
 */

#include <axiom/lib.h>
#include "term.h"

void axterm_history_init(term_t *term)
{
    if (!term) {
        return;
    }
    ax_memset(&term->history, 0, sizeof(term->history));
    term->history.head = 0;
    term->history.count = 0;
    term->history.offset = 0;
    term->history.cols = term->cols;
}

void axterm_history_clear(term_t *term)
{
    if (!term) {
        return;
    }
    term->history.head = 0;
    term->history.count = 0;
    term->history.offset = 0;
}

/* Append one row.  `cells` may point anywhere; only `cols` entries are read. */
void axterm_history_push_line(term_t *term, const term_cell_t *cells, i32 cols)
{
    if (!term || !cells || cols <= 0) {
        return;
    }
    if (cols > TERM_MAX_COLS) {
        cols = TERM_MAX_COLS;
    }

    if (term->history.count == TERM_SCROLLBACK) {
        /* Full: drop the oldest row by shifting the whole ring down.  The
         * memmove moves count-1 rows, which is a few hundred KB in the worst
         * case but happens once per line and only after the buffer is full. */
        ax_memmove(&term->history.lines[0][0], &term->history.lines[1][0],
                   (usize)(TERM_SCROLLBACK - 1) * TERM_MAX_COLS *
                       sizeof(term_cell_t));
        term->history.count = TERM_SCROLLBACK - 1;
        term->history.head = TERM_SCROLLBACK - 1;
    }

    i32 slot = term->history.count;
    if (slot >= TERM_SCROLLBACK) {
        slot = TERM_SCROLLBACK - 1;
    }

    term_cell_t *dst = term->history.lines[slot];
    for (i32 x = 0; x < cols; x++) {
        dst[x] = cells[x];
        /* A stored blank keeps its background so a themed terminal replays
         * correctly; the code point is normalised to zero. */
        if (dst[x].ch == ' ') {
            dst[x].ch = 0;
        }
    }
    for (i32 x = cols; x < TERM_MAX_COLS; x++) {
        dst[x].ch = 0;
        dst[x].bg = term->default_bg;
        dst[x].fg = term->default_fg;
        dst[x].flags = 0;
        dst[x].wide = 0;
    }

    term->history.count++;
    term->history.head = term->history.count;
    term->history.cols = cols;

    /* A new line pushes the view back to the live screen, which is what a
     * user typing at the prompt expects. */
    term->history.offset = 0;
}

/* Push the current top line; used when a line is about to be scrolled away
 * without the caller holding a pointer to the run. */
void axterm_history_push(term_t *term)
{
    if (!term || !term->cells) {
        return;
    }
    axterm_history_push_line(term, &term->cells[0], term->cols);
}

i32 axterm_history_count(const term_t *term)
{
    if (!term) {
        return 0;
    }
    return term->history.count;
}

/* Row accessor: index 0 is the oldest line still buffered. */
term_cell_t *axterm_history_line(const term_t *term, i32 index)
{
    if (!term) {
        return 0;
    }
    if (index < 0 || index >= term->history.count) {
        return 0;
    }
    return (term_cell_t *)term->history.lines[index];
}

/* How many lines above the live screen the view is scrolled. */
i32 axterm_history_offset(const term_t *term)
{
    if (!term) {
        return 0;
    }
    return term->history.offset;
}

/* ------------------------------------------------------------------ */
/* Combined view                                                       */
/* ------------------------------------------------------------------ */

/* Total lines addressable in the combined view (scrollback plus screen). */
i32 axterm_view_lines(const term_t *term)
{
    if (!term) {
        return 0;
    }
    return term->history.count + term->rows;
}

/* Resolve a combined-view row to a cell pointer: rows below history.count
 * come from the live grid.  The pointer stays valid until the next scroll. */
const term_cell_t *axterm_view_line(const term_t *term, i32 row)
{
    if (!term) {
        return 0;
    }
    if (row < 0) {
        return 0;
    }
    if (row < term->history.count) {
        return term->history.lines[row];
    }
    i32 screen_row = row - term->history.count;
    if (screen_row >= term->rows) {
        return 0;
    }
    return &term->cells[(usize)screen_row * TERM_MAX_COLS];
}

/* Line index of the first visible combined row.  When the view is scrolled
 * back by N lines, that is history.count - N. */
i32 axterm_view_top(const term_t *term)
{
    if (!term) {
        return 0;
    }
    i32 top = term->history.count - term->history.offset;
    return top < 0 ? 0 : top;
}

/* Convert a click in the visible area (row 0 = top of the viewport) into a
 * combined-view row number. */
i32 axterm_view_row_at(const term_t *term, i32 visible_row)
{
    return axterm_view_top(term) + visible_row;
}

/* Extract text from one combined-view row, trimming trailing blanks. */
usize axterm_view_line_text(const term_t *term, i32 row, char *buffer, usize cap)
{
    const term_cell_t *line = axterm_view_line(term, row);
    if (!line || !buffer || cap == 0) {
        if (buffer && cap) {
            buffer[0] = 0;
        }
        return 0;
    }
    i32 cols = term->cols;
    i32 end = cols;
    while (end > 0 && (line[end - 1].ch == 0 || line[end - 1].ch == ' ')) {
        end--;
    }
    usize out = 0;
    for (i32 x = 0; x < end && out + 1 < cap; x++) {
        u32 ch = line[x].ch ? line[x].ch : ' ';
        if (ch < 0x80) {
            buffer[out++] = (char)ch;
        } else {
            buffer[out++] = '#';
        }
    }
    buffer[out] = 0;
    return out;
}

/* ------------------------------------------------------------------ */
/* Search                                                              */
/* ------------------------------------------------------------------ */

/* Find the next occurrence of `needle` at or after `from_row`.  The comparison
 * is case insensitive and ASCII only, which is enough for the shell's
 * incremental search (Ctrl+Shift+F). */
i32 axterm_search(const term_t *term, const char *needle, i32 from_row)
{
    if (!term || !needle || !*needle) {
        return -1;
    }
    i32 total = axterm_view_lines(term);
    char line[TERM_MAX_COLS + 1];

    for (i32 row = from_row < 0 ? 0 : from_row; row < total; row++) {
        usize len = axterm_view_line_text(term, row, line, sizeof(line));
        (void)len;
        /* Simple linear scan; the lines are short and the search is rare. */
        for (usize i = 0; line[i]; i++) {
            usize j = 0;
            while (needle[j] && line[i + j] &&
                   ax_tolower(line[i + j]) == ax_tolower(needle[j])) {
                j++;
            }
            if (!needle[j]) {
                return row;
            }
        }
    }
    return -1;
}

/* Scroll so that a combined row sits in the middle of the viewport. */
void axterm_view_center(term_t *term, i32 row)
{
    if (!term) {
        return;
    }
    i32 desired_top = row - term->rows / 2;
    i32 history_needed = term->history.count + term->rows - term->rows;
    /* The offset is how far back from the end we are. */
    i32 offset = term->history.count + term->rows - (desired_top + term->rows);
    if (offset < 0) {
        offset = 0;
    }
    if (offset > term->history.count) {
        offset = term->history.count;
    }
    (void)history_needed;
    term->history.offset = offset;
    term->dirty = (bool_t)1;
}

/* Percentage through the buffer, for the scrollbar thumb. */
i32 axterm_view_percent(const term_t *term)
{
    if (!term) {
        return 0;
    }
    i32 total = axterm_history_count(term);
    if (total <= 0) {
        return 100;
    }
    i32 scrolled = term->history.count - term->history.offset;
    return (scrolled * 100) / (total + 1);
}
