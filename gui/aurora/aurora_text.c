/*
 * AxiomOS - text editing core.
 *
 * A document is a flat character buffer with a caret, an anchor and a bounded
 * undo stack.  Wrapping is computed, never stored: every time the buffer or the
 * width changes, the line table is rebuilt.  That sounds wasteful and it is
 * not — rebuilding a 4000-character wrap table costs less than validating an
 * incremental one, and it removes a whole class of invalidation bugs.
 *
 * The cell width is assumed to be eight pixels per character at scale one.  The
 * real font is a bitmap glyph table without per-glyph advances, so eight is the
 * advance, not an approximation of a proportional font.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXTD_MAX_CHARS   4096
#define AXTD_MAX_LINES    512
#define AXTD_UNDO_DEPTH    32
#define AXTD_CELL          8
#define AXTD_LINE_HEIGHT  18
#define AXTD_TAB_WIDTH     4

typedef struct axtd_undo {
    i32  position;
    i32  removed;      /* characters removed */
    i32  inserted;     /* characters inserted */
    char before[32];
    char after[32];
    bool_t valid;
} axtd_undo_t;

typedef struct axtd_line {
    i32 start;         /* character index of the first cell */
    i32 end;           /* one past the last cell */
    i32 visual;        /* visual row index, for scrolling */
} axtd_line_t;

typedef struct axtd_doc {
    char          text[AXTD_MAX_CHARS];
    i32           length;
    i32           caret;
    i32           anchor;        /* selection origin; equal to caret when empty */
    i32           scroll;
    i32           width;         /* wrap width in cells, 0 means no wrapping */
    axtd_line_t   lines[AXTD_MAX_LINES];
    i32           line_count;
    axtd_undo_t   undo[AXTD_UNDO_DEPTH];
    i32           undo_count;
    i32           undo_position;
    bool_t        read_only;
    bool_t        dirty;
    i32           preferred_column;  /* remembered column for up/down motion */
    bool_t        overwrite;
    bool_t        suppress_undo;
} axtd_doc_t;

/* Defined below, needed by init and every mutation. */
void axtd_reflow(axtd_doc_t *doc);

/* ------------------------------------------------------------------ */
/* Buffer primitives                                                   */
/* ------------------------------------------------------------------ */

void axtd_init(axtd_doc_t *doc, const char *initial, i32 wrap_columns)
{
    if (!doc) return;
    ax_memset(doc, 0, sizeof(*doc));
    doc->width = wrap_columns;
    if (initial) {
        ax_strlcpy(doc->text, initial, sizeof(doc->text));
        doc->length = (i32)ax_strlen(doc->text);
    }
    axtd_reflow(doc);
}

i32 axtd_length(const axtd_doc_t *doc)
{
    return doc ? doc->length : 0;
}

bool_t axtd_has_selection(const axtd_doc_t *doc)
{
    return doc && doc->caret != doc->anchor;
}

void axtd_selection_range(const axtd_doc_t *doc, i32 *start, i32 *end)
{
    if (!doc) {
        if (start) *start = 0;
        if (end) *end = 0;
        return;
    }
    i32 a = doc->caret;
    i32 b = doc->anchor;
    if (start) *start = a < b ? a : b;
    if (end) *end = a < b ? b : a;
}

static void axtd_push_undo(axtd_doc_t *doc, i32 position, const char *before,
                           const char *after)
{
    if (doc->suppress_undo) return;
    /* A fresh edit after an undo discards the redo branch, exactly like a text
     * editor: linear history is easier to reason about than a tree. */
    if (doc->undo_position < doc->undo_count) {
        doc->undo_count = doc->undo_position;
    }
    if (doc->undo_count >= AXTD_UNDO_DEPTH) {
        for (i32 i = 0; i + 1 < doc->undo_count; i++) {
            doc->undo[i] = doc->undo[i + 1];
        }
        doc->undo_count--;
        doc->undo_position = doc->undo_count;
    }
    axtd_undo_t *record = &doc->undo[doc->undo_count++];
    ax_memset(record, 0, sizeof(*record));
    record->position = position;
    record->removed = before ? (i32)ax_strlen(before) : 0;
    record->inserted = after ? (i32)ax_strlen(after) : 0;
    if (before) ax_strlcpy(record->before, before, sizeof(record->before));
    if (after) ax_strlcpy(record->after, after, sizeof(record->after));
    record->valid = (bool_t)1;
    doc->undo_position = doc->undo_count;
}

/* Delete [start,end) and insert `insert` at `start`.  This is the single
 * mutation path: every editing operation funnels through it, which is why
 * selection replacement, typing over a selection, and paste all agree. */
i32 axtd_replace(axtd_doc_t *doc, i32 start, i32 end, const char *insert)
{
    if (!doc || doc->read_only) return 0;
    if (start < 0) start = 0;
    if (end < start) end = start;
    if (end > doc->length) end = doc->length;
    i32 insert_length = insert ? (i32)ax_strlen(insert) : 0;
    i32 removed = end - start;
    if (doc->length - removed + insert_length > AXTD_MAX_CHARS) {
        insert_length = AXTD_MAX_CHARS - (doc->length - removed);
        if (insert_length < 0) insert_length = 0;
    }
    if (removed == 0 && insert_length == 0) return 0;

    char before[32];
    i32 before_length = removed < 31 ? removed : 31;
    if (before_length < 0) before_length = 0;
    for (i32 i = 0; i < before_length; i++) before[i] = doc->text[start + i];
    before[before_length] = 0;
    char after[32];
    i32 after_length = insert_length < 31 ? insert_length : 31;
    if (after_length < 0) after_length = 0;
    for (i32 i = 0; i < after_length; i++) after[i] = insert[i];
    after[after_length] = 0;
    axtd_push_undo(doc, start, before, after);

    /* Move the tail first when the insertion is larger than the removal, so the
     * copy never overwrites characters it still needs. */
    i32 delta = insert_length - removed;
    if (delta > 0) {
        for (i32 i = doc->length - 1; i >= end; i--) {
            doc->text[i + delta] = doc->text[i];
        }
    } else if (delta < 0) {
        for (i32 i = end; i < doc->length; i++) {
            doc->text[i + delta] = doc->text[i];
        }
    }
    for (i32 i = 0; i < insert_length; i++) {
        doc->text[start + i] = insert[i];
    }
    doc->length += delta;
    if (doc->length < 0) doc->length = 0;
    doc->text[doc->length] = 0;
    doc->caret = start + insert_length;
    doc->anchor = doc->caret;
    doc->dirty = (bool_t)1;
    axtd_reflow(doc);
    return insert_length;
}

void axtd_insert(axtd_doc_t *doc, const char *text)
{
    if (!doc || !text) return;
    i32 start, end;
    axtd_selection_range(doc, &start, &end);
    if (doc->overwrite && start == end && *text && start < doc->length &&
        doc->text[start] != '\n') {
        axtd_replace(doc, start, start + 1, text);
        return;
    }
    axtd_replace(doc, start, end, text);
}

void axtd_delete_selection(axtd_doc_t *doc)
{
    if (!doc) return;
    i32 start, end;
    axtd_selection_range(doc, &start, &end);
    if (start == end) return;
    axtd_replace(doc, start, end, 0);
}

/* Backspace removes one character, or the selection when there is one.  A
 * backspace at a line start joins it with the previous line, which is one delete
 * because the newline is the character being removed. */
void axtd_backspace(axtd_doc_t *doc)
{
    if (!doc || doc->read_only) return;
    i32 start, end;
    axtd_selection_range(doc, &start, &end);
    if (start != end) {
        axtd_replace(doc, start, end, 0);
        return;
    }
    if (doc->caret <= 0) return;
    axtd_replace(doc, doc->caret - 1, doc->caret, 0);
}

void axtd_delete_forward(axtd_doc_t *doc)
{
    if (!doc || doc->read_only) return;
    i32 start, end;
    axtd_selection_range(doc, &start, &end);
    if (start != end) {
        axtd_replace(doc, start, end, 0);
        return;
    }
    if (doc->caret >= doc->length) return;
    axtd_replace(doc, doc->caret, doc->caret + 1, 0);
}

bool_t axtd_undo(axtd_doc_t *doc)
{
    if (!doc || doc->undo_position <= 0) return (bool_t)0;
    if (doc->undo_position <= 0 || doc->undo_position > doc->undo_count) {
        return (bool_t)0;
    }
    axtd_undo_t *record = &doc->undo[doc->undo_position - 1];
    if (!record->valid) return (bool_t)0;
    i32 start = record->position;
    /* Undo is the inverse mutation: remove what was inserted, put back what was
     * removed.  The undo record itself is consumed, not re-recorded. */
    bool_t read_only = doc->read_only;
    doc->read_only = (bool_t)0;
    doc->suppress_undo = (bool_t)1;
    axtd_replace(doc, start, start + record->inserted, record->before);
    doc->suppress_undo = (bool_t)0;
    doc->read_only = read_only;
    doc->caret = start;
    doc->anchor = doc->caret;
    doc->undo_position = doc->undo_position - 1;
    doc->undo_count = doc->undo_position;
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Motion                                                              */
/* ------------------------------------------------------------------ */

static i32 axtd_line_of(const axtd_doc_t *doc, i32 position)
{
    for (i32 i = 0; i < doc->line_count; i++) {
        if (position >= doc->lines[i].start && position <= doc->lines[i].end) {
            return i;
        }
    }
    return doc->line_count - 1;
}

void axtd_move(axtd_doc_t *doc, i32 delta, bool_t extend)
{
    if (!doc) return;
    i32 target = doc->caret + delta;
    if (target < 0) target = 0;
    if (target > doc->length) target = doc->length;
    doc->caret = target;
    if (!extend) doc->anchor = target;
}

/* Vertical motion uses the wrap table rather than counting newlines, so moving
 * up inside a wrapped paragraph lands on the right visual row. */
void axtd_move_vertical(axtd_doc_t *doc, i32 delta, bool_t extend)
{
    if (!doc || doc->line_count <= 0) return;
    i32 line = axtd_line_of(doc, doc->caret);
    i32 column = doc->caret - doc->lines[line].start;
    if (delta < 0) {
        doc->preferred_column = doc->preferred_column > 0
                                    ? doc->preferred_column : column;
    }
    i32 target_line = line + delta;
    if (target_line < 0) target_line = 0;
    if (target_line >= doc->line_count) target_line = doc->line_count - 1;
    i32 width = doc->lines[target_line].end - doc->lines[target_line].start;
    i32 column_target = doc->preferred_column;
    if (column_target > width) column_target = width;
    doc->caret = doc->lines[target_line].start + column_target;
    if (!extend) doc->anchor = doc->caret;
}

void axtd_home(axtd_doc_t *doc, bool_t extend)
{
    if (!doc) return;
    i32 line = axtd_line_of(doc, doc->caret);
    doc->caret = doc->lines[line].start;
    doc->preferred_column = 0;
    if (!extend) doc->anchor = doc->caret;
}

void axtd_end(axtd_doc_t *doc, bool_t extend)
{
    if (!doc) return;
    i32 line = axtd_line_of(doc, doc->caret);
    doc->caret = doc->lines[line].end;
    doc->preferred_column = 0;
    if (!extend) doc->anchor = doc->caret;
}

void axtd_select_all(axtd_doc_t *doc)
{
    if (!doc) return;
    doc->anchor = 0;
    doc->caret = doc->length;
}

void axtd_set_caret(axtd_doc_t *doc, i32 position, bool_t extend)
{
    if (!doc) return;
    if (position < 0) position = 0;
    if (position > doc->length) position = doc->length;
    doc->caret = position;
    if (!extend) doc->anchor = position;
    doc->preferred_column = 0;
}

/* ------------------------------------------------------------------ */
/* Wrapping                                                            */
/* ------------------------------------------------------------------ */

/* Rebuild the visual line table.  Tabs expand to the next multiple of four
 * cells, and wrapping happens at a cell budget rather than at words: a word-wrap
 * algorithm that splits long identifiers looks broken in a terminal, and this
 * document model is also used by one. */
void axtd_reflow(axtd_doc_t *doc)
{
    if (!doc) return;
    doc->line_count = 0;
    i32 line_start = 0;
    i32 cells = 0;
    i32 limit = doc->width > 0 ? doc->width : AXTD_MAX_CHARS;
    for (i32 i = 0; i <= doc->length; i++) {
        bool_t at_end = i == doc->length;
        char ch = at_end ? '\n' : doc->text[i];
        if (!at_end) {
            i32 advance = ch == '\t' ? (AXTD_TAB_WIDTH - (cells % AXTD_TAB_WIDTH))
                                     : 1;
            cells += advance;
        }
        if (at_end || ch == '\n' || cells > limit) {
            if (doc->line_count < AXTD_MAX_LINES) {
                axtd_line_t *line = &doc->lines[doc->line_count];
                line->start = line_start;
                line->end = i;
                line->visual = doc->line_count;
                doc->line_count++;
            }
            line_start = ch == '\n' ? i + 1 : i;
            cells = 0;
        }
    }
    if (doc->caret > doc->length) doc->caret = doc->length;
}

void axtd_set_width(axtd_doc_t *doc, i32 columns)
{
    if (!doc) return;
    doc->width = columns;
    axtd_reflow(doc);
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

void axtd_paint(gui_surface_t *surface, axtd_doc_t *doc, gui_rect_t rect,
                const axau_palette_t *palette, bool_t focused)
{
    if (!surface || !doc || !palette) return;
    gui_fill_rect(surface, rect, axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
    if (focused) {
        axui_round_rect_border(surface, rect, 4, 1,
                               axau_colour(palette, AXAU_COLOUR_FOCUS));
    } else {
        axui_round_rect_border(surface, rect, 4, 1,
                               axau_colour(palette, AXAU_COLOUR_STROKE));
    }

    i32 start, end;
    axtd_selection_range(doc, &start, &end);
    i32 visible_rows = (rect.h - 8) / AXTD_LINE_HEIGHT;
    i32 line_count = doc->line_count;

    for (i32 line = 0; line < line_count; line++) {
        i32 row = line - doc->scroll;
        if (row < 0 || row >= visible_rows) continue;
        axtd_line_t *entry = &doc->lines[line];
        i32 y = rect.y + 4 + row * AXTD_LINE_HEIGHT;
        i32 cell = 0;

        /* Selection highlight is drawn per-cell run, not per-character, so a
         * wrapped selection is one band per visual line instead of a hundred
         * small rectangles. */
        i32 band_start = -1;
        for (i32 i = entry->start; i <= entry->end; i++) {
            bool_t inside = i >= start && i < end;
            if (inside && band_start < 0) band_start = cell;
            if (!inside && band_start >= 0) {
                gui_fill_rect(surface,
                              gui_rect(rect.x + 6 + band_start * AXTD_CELL, y,
                                       (cell - band_start) * AXTD_CELL,
                                       AXTD_LINE_HEIGHT),
                              axui_colour_opacity(
                                  axau_colour(palette, AXAU_COLOUR_ACCENT), 24));
                band_start = -1;
            }
            if (i == entry->end) break;
            char ch = doc->text[i];
            if (ch == '\t') {
                cell += AXTD_TAB_WIDTH - (cell % AXTD_TAB_WIDTH);
                continue;
            }
            char glyph[2];
            glyph[0] = ch;
            glyph[1] = 0;
            gui_text(surface, rect.x + 6 + cell * AXTD_CELL, y, glyph,
                     axau_colour(palette, AXAU_COLOUR_TEXT), 1);
            cell++;
        }
        if (band_start >= 0) {
            gui_fill_rect(surface,
                          gui_rect(rect.x + 6 + band_start * AXTD_CELL, y,
                                   (cell - band_start) * AXTD_CELL,
                                   AXTD_LINE_HEIGHT),
                          axui_colour_opacity(
                              axau_colour(palette, AXAU_COLOUR_ACCENT), 24));
        }
    }

    if (focused) {
        i32 line = axtd_line_of(doc, doc->caret);
        i32 row = line - doc->scroll;
        if (row >= 0 && row < visible_rows) {
            i32 column = doc->caret - doc->lines[line].start;
            i32 x = rect.x + 6 + column * AXTD_CELL;
            i32 y = rect.y + 4 + row * AXTD_LINE_HEIGHT;
            gui_fill_rect(surface, gui_rect(x, y, 1, AXTD_LINE_HEIGHT),
                          axau_colour(palette, AXAU_COLOUR_TEXT));
        }
    }

    /* Scrollbar is only shown when the content exceeds the box, because a
     * permanent empty track reads as a disabled control. */
    if (line_count > visible_rows && visible_rows > 0) {
        i32 thumb_h = (visible_rows * rect.h) / line_count;
        if (thumb_h < 20) thumb_h = 20;
        i32 offset = (rect.h - thumb_h) * doc->scroll /
                     (line_count - visible_rows ? line_count - visible_rows : 1);
        gui_fill_round_rect(surface,
                            gui_rect(rect.x + rect.w - 6, rect.y + offset, 4,
                                     thumb_h), 2,
                            axau_colour(palette, AXAU_COLOUR_STROKE_STRONG));
    }
}

/* Hit-test a caret position from a point, snapping to the nearest cell. */
i32 axtd_hit(axtd_doc_t *doc, gui_rect_t rect, i32 x, i32 y)
{
    if (!doc) return 0;
    i32 row = (y - rect.y - 4) / AXTD_LINE_HEIGHT;
    if (row < 0) row = 0;
    i32 line = doc->scroll + row;
    if (line >= doc->line_count) line = doc->line_count - 1;
    if (line < 0) return 0;
    i32 column = (x - rect.x - 6) / AXTD_CELL;
    if (column < 0) column = 0;
    axtd_line_t *entry = &doc->lines[line];
    i32 width = entry->end - entry->start;
    if (column > width) column = width;
    doc->caret = entry->start + column;
    doc->anchor = doc->caret;
    return doc->caret;
}

void axtd_scroll_to_caret(axtd_doc_t *doc, i32 visible_rows)
{
    if (!doc || visible_rows <= 0) return;
    i32 line = axtd_line_of(doc, doc->caret);
    if (line < doc->scroll) doc->scroll = line;
    if (line >= doc->scroll + visible_rows) doc->scroll = line - visible_rows + 1;
    if (doc->scroll < 0) doc->scroll = 0;
}

/* Return the whole document in a caller-owned buffer, useful for saving or for
 * handing the text to the shell's command parser. */
i32 axtd_text(const axtd_doc_t *doc, char *out, i32 capacity)
{
    if (!doc || !out || capacity <= 0) return 0;
    i32 count = doc->length < capacity - 1 ? doc->length : capacity - 1;
    for (i32 i = 0; i < count; i++) out[i] = doc->text[i];
    out[count] = 0;
    return count;
}

void axtd_dump(const axtd_doc_t *doc)
{
    if (!doc) return;
    printk("text: %d chars, %d lines, caret=%d anchor=%d undo=%d/%d\n",
           doc->length, doc->line_count, doc->caret, doc->anchor,
           doc->undo_position, doc->undo_count);
}



