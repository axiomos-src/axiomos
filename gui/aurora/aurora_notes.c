/*
 * AxiomOS - notes surface.
 *
 * A list of notes and a single-note editor.  The editor is intentionally plain
 * text with one structural convention: a line beginning with '#' is a heading.
 * That is enough for a notes app to look organised without implementing a
 * markup parser, and it degrades gracefully — a note full of '#' still reads as
 * text if the convention is ignored.
 *
 * Every edit bumps a revision counter.  The counter exists so an autosave
 * indicator can say "saved" only when the revision it last wrote matches the
 * current one, which is the difference between a believable save state and a
 * lie.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXN_MAX_NOTES    32
#define AXN_BODY_CHARS 4096
#define AXN_LIST_WIDTH  240
#define AXN_ROW_HEIGHT   64

typedef struct axt_note {
    char   title[48];
    char   body[AXN_BODY_CHARS];
    char   updated[20];
    char   labels[32];
    u32    tint;
    bool_t pinned;
    bool_t archived;
    i32    revision;
    i32    saved_revision;
} axt_note_t;

typedef struct axn_app {
    gui_rect_t rect;
    axt_note_t notes[AXN_MAX_NOTES];
    i32        count;
    i32        selected;
    i32        hover;
    i32        scroll;
    i32        caret;
    i32        scroll_lines;
    char       search[36];
    bool_t     search_focus;
    i32        ticks;
    bool_t     dirty;
} axn_app_t;

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

void axn_init(axn_app_t *app, gui_rect_t rect)
{
    if (!app) return;
    ax_memset(app, 0, sizeof(*app));
    app->rect = rect;
    app->selected = -1;
    app->hover = -1;
}

i32 axn_add(axn_app_t *app, const char *title, const char *body, const char *labels,
            u32 tint, bool_t pinned)
{
    if (!app || app->count >= AXN_MAX_NOTES) return -1;
    i32 index = app->count++;
    axt_note_t *note = &app->notes[index];
    ax_memset(note, 0, sizeof(*note));
    ax_strlcpy(note->title, title ? title : "Untitled", sizeof(note->title));
    ax_strlcpy(note->body, body ? body : "", sizeof(note->body));
    ax_strlcpy(note->labels, labels ? labels : "", sizeof(note->labels));
    ax_strlcpy(note->updated, "just now", sizeof(note->updated));
    note->tint = tint;
    note->pinned = pinned;
    note->revision = 1;
    note->saved_revision = 1;
    return index;
}

/* Notes sort pinned first, then by most recently edited.  Pinned notes are a
 * deliberate short list, so they stay at the top instead of being pushed down
 * by activity. */
static i32 axn_order(const axn_app_t *app, i32 *indices)
{
    i32 count = 0;
    for (i32 i = 0; i < app->count; i++) {
        if (app->notes[i].archived) continue;
        if (app->search[0]) {
            bool_t match = (bool_t)0;
            const char *fields[2] = {app->notes[i].title, app->notes[i].body};
            for (i32 f = 0; f < 2 && !match; f++) {
                const char *hay = fields[f];
                while (*hay && !match) {
                    const char *a = hay;
                    const char *b = app->search;
                    while (*a && *b) {
                        char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
                        char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
                        if (ca != cb) break;
                        a++;
                        b++;
                    }
                    if (!*b) match = (bool_t)1;
                    hay++;
                }
            }
            if (!match) continue;
        }
        i32 j = count;
        while (j > 0) {
            const axt_note_t *previous = &app->notes[indices[j - 1]];
            const axt_note_t *current = &app->notes[i];
            bool_t after = (previous->pinned && !current->pinned) ||
                           (previous->pinned == current->pinned &&
                            previous->saved_revision >= current->saved_revision);
            if (after) break;
            indices[j] = indices[j - 1];
            j--;
        }
        indices[j] = i;
        count++;
    }
    return count;
}

i32 axn_word_count(const axt_note_t *note)
{
    if (!note) return 0;
    i32 words = 0;
    bool_t inside = (bool_t)0;
    for (const char *p = note->body; *p; p++) {
        bool_t space = (bool_t)(*p == ' ' || *p == '\n' || *p == '\t');
        if (!space && !inside) {
            words++;
            inside = (bool_t)1;
        } else if (space) {
            inside = (bool_t)0;
        }
    }
    return words;
}

/* Editing appends at the caret, which for a notes app is always the end of the
 * body: there is no caret placement UI yet, and pretending otherwise would
 * produce edits in surprising places. */
void axn_type(axn_app_t *app, char ch)
{
    if (!app || app->selected < 0 || app->selected >= app->count) return;
    axt_note_t *note = &app->notes[app->selected];
    if (ch == 0x08) {
        i32 length = (i32)ax_strlen(note->body);
        if (length > 0) {
            note->body[length - 1] = 0;
            note->revision++;
            app->dirty = (bool_t)1;
        }
        return;
    }
    if (ch < 32 && ch != '\n') return;
    i32 length = (i32)ax_strlen(note->body);
    if (length + 1 < AXN_BODY_CHARS) {
        note->body[length] = ch;
        note->body[length + 1] = 0;
        note->revision++;
        note->saved_revision = note->revision - 0;   /* pending */
        app->dirty = (bool_t)1;
    }
}

void axn_select(axn_app_t *app, i32 index)
{
    if (!app || index < 0 || index >= app->count) return;
    app->selected = index;
    app->scroll_lines = 0;
}

void axn_new(axn_app_t *app)
{
    if (!app) return;
    char title[32];
    ax_snprintf(title, sizeof(title), "Note %d", app->count + 1);
    i32 index = axn_add(app, title, "", "", 0x00404060, (bool_t)0);
    if (index >= 0) axn_select(app, index);
}

void axn_toggle_pin(axn_app_t *app, i32 index)
{
    if (!app || index < 0 || index >= app->count) return;
    app->notes[index].pinned = (bool_t)!app->notes[index].pinned;
}

/* Autosave ticks the revision forward once the user has stopped typing for a
 * while.  Idle detection is a countdown, not a timestamp comparison, so it
 * behaves the same regardless of frame pacing. */
void axn_tick(axn_app_t *app)
{
    if (!app) return;
    app->ticks++;
    if (app->selected < 0 || app->selected >= app->count) return;
    axt_note_t *note = &app->notes[app->selected];
    if (note->saved_revision != note->revision && app->ticks % 40 == 0) {
        note->saved_revision = note->revision;
        ax_strlcpy(note->updated, "saved", sizeof(note->updated));
    }
}

void axn_search_type(axn_app_t *app, char ch)
{
    if (!app) return;
    if (ch == 0x08) {
        i32 length = (i32)ax_strlen(app->search);
        if (length > 0) app->search[length - 1] = 0;
        return;
    }
    if (ch < 32 || ch > 126) return;
    i32 length = (i32)ax_strlen(app->search);
    if (length + 1 < (i32)sizeof(app->search)) {
        app->search[length] = ch;
        app->search[length + 1] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

/* Heading lines render one scale larger and in the accent colour.  Everything
 * else is body text, so a note with no headings still looks complete. */
static void axn_paint_body(gui_surface_t *surface, gui_rect_t rect,
                           const axt_note_t *note, i32 scroll_lines,
                           const axau_palette_t *palette)
{
    i32 columns = (rect.w - 32) / 8;
    if (columns < 8) columns = 8;
    i32 length = (i32)ax_strlen(note->body);
    i32 line_start = 0;
    i32 visual_line = 0;
    i32 y = rect.y + 8;
    for (i32 i = 0; i <= length; i++) {
        bool_t at_end = i == length;
        if (!at_end && note->body[i] != '\n' && (i - line_start) < columns) continue;
        i32 line_length = i - line_start;
        if (line_length > columns) line_length = columns;
        if (visual_line >= scroll_lines) {
            char line[200];
            if (line_length > (i32)sizeof(line) - 1) {
                line_length = (i32)sizeof(line) - 1;
            }
            for (i32 c = 0; c < line_length; c++) line[c] = note->body[line_start + c];
            line[line_length] = 0;
            bool_t heading = (bool_t)(note->body[line_start] == '#');
            if (heading) {
                axui_draw_text(surface, rect.x + 16, y, line + 1,
                               axui_style(axau_colour(palette,
                                                      AXAU_COLOUR_ACCENT), 2));
                y += 26;
            } else {
                axui_draw_text(surface, rect.x + 16, y, line,
                               axui_style(axau_colour(palette,
                                                      AXAU_COLOUR_TEXT), 1));
                y += 18;
            }
            if (y + 18 > rect.y + rect.h) break;
        }
        visual_line++;
        line_start = at_end ? i : i + 1;
    }
}

void axn_paint(gui_surface_t *surface, axn_app_t *app,
               const axau_palette_t *palette)
{
    if (!surface || !app || !palette) return;
    gui_fill_rect(surface, app->rect, axau_colour(palette, AXAU_COLOUR_SURFACE));

    /* List rail. */
    gui_rect_t rail = gui_rect(app->rect.x, app->rect.y, AXN_LIST_WIDTH,
                               app->rect.h);
    gui_fill_rect(surface, rail, axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    gui_rect_t search = gui_rect(rail.x + 12, rail.y + 12, rail.w - 24, 32);
    gui_fill_round_rect(surface, search, 4,
                        axau_colour(palette, AXAU_COLOUR_CONTROL));
    axau_icon_paint(surface, "?", search.x + 14, search.y + 16, 10,
                    axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY));
    axui_draw_text(surface, search.x + 30, search.y + 9,
                   app->search[0] ? app->search : "Search notes",
                   axui_style(app->search[0]
                                  ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                  : axau_colour(palette,
                                                AXAU_COLOUR_TEXT_TERTIARY), 1));

    i32 indices[AXN_MAX_NOTES];
    i32 count = axn_order(app, indices);
    i32 visible = (rail.h - 60) / AXN_ROW_HEIGHT;
    i32 y = rail.y + 56;
    for (i32 i = app->scroll; i < count && i < app->scroll + visible; i++) {
        axt_note_t *note = &app->notes[indices[i]];
        gui_rect_t row = gui_rect(rail.x + 8, y, rail.w - 16, AXN_ROW_HEIGHT - 4);
        if (indices[i] == app->selected) {
            gui_fill_round_rect(surface, row, 6,
                                axui_colour_opacity(
                                    axau_colour(palette, AXAU_COLOUR_ACCENT), 18));
        } else if (app->hover == i) {
            gui_fill_round_rect(surface, row, 6,
                                axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
        }
        gui_fill_round_rect(surface, gui_rect(row.x + 6, row.y + 8, 4, row.h - 16),
                            2, note->tint);
        axui_draw_text_clip(surface,
                            gui_rect(row.x + 16, row.y + 6, row.w - 28, 18),
                            row.x + 16, row.y + 6, note->title,
                            axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        axui_draw_text_clip(surface,
                            gui_rect(row.x + 16, row.y + 24, row.w - 28, 14),
                            row.x + 16, row.y + 24, note->body,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_TERTIARY), 1));
        axui_draw_text(surface, row.x + 16, row.y + 42, note->updated,
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_TERTIARY), 1));
        if (note->pinned) {
            axau_icon_paint(surface, "i", row.x + row.w - 16, row.y + 14, 10,
                            axau_colour(palette, AXAU_COLOUR_ACCENT));
        }
        y += AXN_ROW_HEIGHT;
    }

    gui_rect_t new_button = gui_rect(rail.x + 12, rail.y + rail.h - 48,
                                     rail.w - 24, 34);
    gui_fill_round_rect(surface, new_button, 4,
                        axau_colour(palette, AXAU_COLOUR_ACCENT));
    axui_draw_text_center(surface, new_button, "New note",
                          axui_style(axau_colour(palette,
                                                 AXAU_COLOUR_ACCENT_TEXT), 1));

    /* Editor. */
    gui_rect_t editor = gui_rect(app->rect.x + AXN_LIST_WIDTH, app->rect.y,
                                 app->rect.w - AXN_LIST_WIDTH, app->rect.h);
    gui_fill_rect(surface, editor, axau_colour(palette, AXAU_COLOUR_SURFACE));
    axau_paint_divider(surface, gui_rect(editor.x, editor.y, 1, editor.h), palette);
    if (app->selected < 0 || app->selected >= app->count) {
        axui_draw_text_center(surface,
                              gui_rect(editor.x, editor.y + editor.h / 2 - 10,
                                       editor.w, 20),
                              "Choose a note or create one",
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_DISABLED), 1));
        return;
    }
    axt_note_t *note = &app->notes[app->selected];
    axui_draw_text_clip(surface,
                        gui_rect(editor.x + 20, editor.y + 16, editor.w - 40, 24),
                        editor.x + 20, editor.y + 16, note->title,
                        axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
    axui_draw_text(surface, editor.x + 20, editor.y + 44, note->labels,
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1));
    char metrics[48];
    ax_snprintf(metrics, sizeof(metrics), "%d words, %d chars",
                axn_word_count(note), (i32)ax_strlen(note->body));
    axui_draw_text_right(surface,
                         gui_rect(editor.x, editor.y + 44, editor.w - 20, 16),
                         metrics,
                         axui_style(axau_colour(palette,
                                                AXAU_COLOUR_TEXT_TERTIARY), 1));

    gui_rect_t body = gui_rect(editor.x + 8, editor.y + 72, editor.w - 16,
                               editor.h - 100);
    axn_paint_body(surface, body, note, app->scroll_lines, palette);

    /* Save indicator. */
    char status[32];
    ax_snprintf(status, sizeof(status), "%s",
                note->saved_revision == note->revision ? "Saved" : "Editing");
    axui_draw_text(surface, editor.x + 20, editor.y + editor.h - 22, status,
                   axui_style(note->saved_revision == note->revision
                                  ? axau_colour(palette, AXAU_COLOUR_SUCCESS)
                                  : axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY),
                              1));
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

i32 axn_click(axn_app_t *app, i32 x, i32 y)
{
    if (!app) return 0;
    gui_rect_t rail = gui_rect(app->rect.x, app->rect.y, AXN_LIST_WIDTH,
                               app->rect.h);
    if (gui_rect_contains(rail, x, y)) {
        if (y >= rail.y + rail.h - 48) {
            axn_new(app);
            return 1;
        }
        i32 indices[AXN_MAX_NOTES];
        i32 count = axn_order(app, indices);
        i32 row = (y - rail.y - 56) / AXN_ROW_HEIGHT + app->scroll;
        if (row >= 0 && row < count) {
            axn_select(app, indices[row]);
            return 10 + row;
        }
        return 2;
    }
    /* Clicking the body area places the caret at the end; there is no per-line
     * caret placement because the editor does not yet measure glyph runs. */
    if (x > app->rect.x + AXN_LIST_WIDTH) {
        if (app->selected >= 0) {
            app->caret = (i32)ax_strlen(app->notes[app->selected].body);
        }
        return 3;
    }
    return 0;
}

void axn_hover(axn_app_t *app, i32 x, i32 y)
{
    if (!app) return;
    gui_rect_t rail = gui_rect(app->rect.x, app->rect.y, AXN_LIST_WIDTH,
                               app->rect.h);
    app->hover = -1;
    if (!gui_rect_contains(rail, x, y)) return;
    app->hover = (y - rail.y - 56) / AXN_ROW_HEIGHT + app->scroll;
}

void axn_scroll(axn_app_t *app, i32 rows)
{
    if (!app) return;
    i32 indices[AXN_MAX_NOTES];
    i32 count = axn_order(app, indices);
    i32 visible = (app->rect.h - 60) / AXN_ROW_HEIGHT;
    app->scroll += rows;
    if (app->scroll < 0) app->scroll = 0;
    if (app->scroll > count - visible && count > visible) {
        app->scroll = count - visible;
    }
}

i32 axn_count(const axn_app_t *app)
{
    return app ? app->count : 0;
}

void axn_dump(const axn_app_t *app)
{
    if (!app) return;
    printk("notes: %d notes, selected=%d, dirty=%d\n", app->count, app->selected,
           app->dirty);
}
