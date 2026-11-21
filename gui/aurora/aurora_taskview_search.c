/*
 * AxiomOS - task view and universal search.
 *
 * Both surfaces are overlays: they sit above the window stack, dim the desk
 * behind them, and own the pointer while they are open.  Task view answers
 * "what is running" with live thumbnails of each window's last painted content,
 * plus a workspace strip.  Search answers "where is it" by filtering a flat
 * catalogue and grouping results by their declared section.
 *
 * Neither overlay keeps its own copy of window state.  Task view reads the
 * workspace on every frame, so a window closing behind it simply disappears
 * from the grid instead of leaving a stale card behind.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"
#include "aurora_apps.h"

/* ------------------------------------------------------------------ */
/* Task view                                                           */
/* ------------------------------------------------------------------ */

void axau_taskview_init(axau_taskview_t *view, gui_rect_t rect)
{
    if (!view) return;
    ax_memset(view, 0, sizeof(*view));
    view->rect = rect;
    view->hover = -1;
    view->selected = -1;
}

void axau_taskview_open(axau_taskview_t *view)
{
    if (!view) return;
    view->open = (bool_t)1;
    view->closing = (bool_t)0;
    view->progress = 0;
    view->hover = -1;
}

void axau_taskview_close(axau_taskview_t *view)
{
    if (!view) return;
    if (!view->open) return;
    view->closing = (bool_t)1;
}

/* The progress value drives two things at once: the dim behind the overlay and
 * the vertical offset of the cards.  Cards rise eight pixels as the overlay
 * fades in, which is enough motion to signal arrival without being theatrical. */
bool_t axau_taskview_tick(axau_taskview_t *view)
{
    if (!view) return (bool_t)0;
    if (view->closing) {
        view->progress -= 120;
        if (view->progress <= 0) {
            view->progress = 0;
            view->open = (bool_t)0;
            view->closing = (bool_t)0;
            return (bool_t)0;
        }
        return (bool_t)1;
    }
    if (view->open && view->progress < 1000) {
        view->progress += 140;
        if (view->progress > 1000) view->progress = 1000;
        return (bool_t)1;
    }
    return (bool_t)0;
}

/* The card grid: up to four columns depending on width, one card per visible
 * window.  Cards are 16:10 to match the frames they preview. */
static i32 taskview_grid(const axau_taskview_t *view, i32 *card_w, i32 *card_h,
                         i32 *gap)
{
    i32 columns = view->rect.w / 300;
    if (columns < 2) columns = 2;
    if (columns > 4) columns = 4;
    i32 spacing = 20;
    *gap = spacing;
    *card_w = (view->rect.w - spacing * (columns + 1)) / columns;
    if (*card_w > 360) *card_w = 360;
    *card_h = (*card_w * 10) / 16 + 44;
    return columns;
}

void axau_taskview_paint(gui_surface_t *surface, axau_taskview_t *view,
                         axau_workspace_t *workspace,
                         const axau_palette_t *palette)
{
    if (!surface || !view || !workspace || !palette) return;
    if (!view->open) return;
    i32 progress = view->progress;
    u8 dim = (u8)((progress * 150) / 1000);
    gui_blend_rect(surface, view->rect,
                   axui_colour_opacity(axau_colour(palette, AXAU_COLOUR_SHADOW),
                                       (i32)dim));

    axui_draw_text(surface, view->rect.x + 32,
                   view->rect.y + 24 + (1000 - progress) / 40,
                   "Running now",
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));

    i32 card_w, card_h, gap;
    i32 columns = taskview_grid(view, &card_w, &card_h, &gap);
    i32 index = 0;
    i32 top = view->rect.y + 80;
    for (i32 i = 0; i < workspace->count; i++) {
        axau_window_t *window = &workspace->windows[i];
        if (window->workspace != workspace->current) continue;
        if (window->state == AXAU_WINDOW_CLOSING ||
            window->state == AXAU_WINDOW_HIDDEN) {
            continue;
        }
        i32 column = index % columns;
        i32 row = index / columns;
        gui_rect_t card = gui_rect(view->rect.x + gap + column * (card_w + gap),
                                   top + row * (card_h + gap) +
                                   (1000 - progress) / 30,
                                   card_w, card_h);
        bool_t hot = index == view->hover;
        gui_fill_round_rect(surface, card, AXAU_RADIUS_CARD,
                            axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
        if (hot) {
            axui_round_rect_border(surface, card, AXAU_RADIUS_CARD, 2,
                                   axau_colour(palette, AXAU_COLOUR_ACCENT));
        }
        /* Thumbnail area is filled with the window's accent at low alpha:
         * a stand-in for a live surface the compositor will provide later. */
        gui_rect_t thumb = gui_rect(card.x + 8, card.y + 8, card.w - 16,
                                    card.h - 56);
        gui_fill_round_rect(surface, thumb, 4,
                            axui_colour_opacity(axau_colour(palette,
                                                            AXAU_COLOUR_ACCENT), 10));
        axau_icon_glyph(surface, thumb.x + thumb.w / 2, thumb.y + thumb.h / 2,
                        24, window->glyph ? window->glyph : "#",
                        axau_colour(palette, AXAU_COLOUR_ACCENT));
        axui_draw_text_clip(surface,
                            gui_rect(card.x + 10, card.y + card.h - 40,
                                     card.w - 20, 20),
                            card.x + 10, card.y + card.h - 40,
                            window->title,
                            axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        if (window->state == AXAU_WINDOW_MINIMISED) {
            axui_draw_text(surface, card.x + 10, card.y + card.h - 22,
                           "Minimised",
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT_TERTIARY), 1));
        }
        index++;
    }

    /* Workspace strip along the bottom. */
    i32 strip_y = view->rect.y + view->rect.h - 48;
    for (i32 w = 0; w < workspace->workspace_count; w++) {
        gui_rect_t chip = gui_rect(view->rect.x + 32 + w * 54, strip_y, 40, 28);
        bool_t current = w == workspace->current;
        gui_fill_round_rect(surface, chip, 6,
                            current
                                ? axui_colour_opacity(
                                      axau_colour(palette, AXAU_COLOUR_ACCENT), 26)
                                : axau_colour(palette, AXAU_COLOUR_CONTROL));
    }
}

/* ------------------------------------------------------------------ */
/* Search                                                              */
/* ------------------------------------------------------------------ */

void axau_search_init(axau_search_panel_t *panel, gui_rect_t rect)
{
    if (!panel) return;
    ax_memset(panel, 0, sizeof(*panel));
    panel->rect = rect;
    panel->hover = -1;
    panel->selected = -1;
}

void axau_search_add(axau_search_panel_t *panel, const char *title,
                     const char *detail, const char *glyph, const char *section)
{
    if (!panel || panel->item_count >= 24) return;
    axau_search_item_t *item = &panel->items[panel->item_count++];
    ax_strlcpy(item->title, title ? title : "", sizeof(item->title));
    ax_strlcpy(item->detail, detail ? detail : "", sizeof(item->detail));
    item->glyph = glyph ? glyph : "#";
    item->section = section ? section : "Results";
}

void axau_search_open(axau_search_panel_t *panel, const char *query)
{
    if (!panel) return;
    panel->open = (bool_t)1;
    panel->progress = 0;
    panel->hover = -1;
    panel->selected = -1;
    ax_strlcpy(panel->query, query ? query : "", sizeof(panel->query));
    panel->caret = (i32)ax_strlen(panel->query);
    axau_search_perform(panel);
}

void axau_search_close(axau_search_panel_t *panel)
{
    if (!panel) return;
    panel->open = (bool_t)0;
    panel->progress = 0;
}

/* Case-insensitive substring ranking: an exact prefix beats a match at a word
 * boundary, which beats a match anywhere.  Ranking is stored implicitly by
 * inserting results into the match list in order of score. */
static i32 search_score(const char *haystack, const char *needle, i32 *match_at)
{
    if (!*needle) return 0;
    i32 score = 0;
    const char *h = haystack;
    i32 position = 0;
    while (*h) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b) {
            char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
            char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            a++;
            b++;
        }
        if (!*b) {
            score = position == 0 ? 3 : (h[-1] == ' ' ? 2 : 1);
            if (match_at) *match_at = position;
            return score;
        }
        h++;
        position++;
    }
    return 0;
}

i32 axau_search_perform(axau_search_panel_t *panel)
{
    if (!panel) return 0;
    panel->match_count = 0;
    if (!panel->query[0]) {
        for (i32 i = 0; i < panel->item_count && i < 24; i++) {
            panel->matches[panel->match_count++] = i;
        }
        return panel->match_count;
    }
    i32 score_of[24];
    for (i32 i = 0; i < panel->item_count; i++) {
        i32 at = 0;
        i32 score = search_score(panel->items[i].title, panel->query, &at);
        if (score == 0) {
            score = search_score(panel->items[i].detail, panel->query, &at);
            if (score > 0) score = 1;
        }
        score_of[i] = score;
    }
    /* Insertion sort by descending score, stable within equal scores. */
    for (i32 i = 0; i < panel->item_count; i++) {
        if (score_of[i] == 0) continue;
        i32 j = panel->match_count++;
        while (j > 0 && score_of[panel->matches[j - 1]] < score_of[i]) {
            panel->matches[j] = panel->matches[j - 1];
            j--;
        }
        panel->matches[j] = i;
    }
    if (panel->selected < 0 && panel->match_count > 0) {
        panel->selected = panel->matches[0];
    }
    return panel->match_count;
}

void axau_search_type(axau_search_panel_t *panel, char ch)
{
    if (!panel) return;
    if (ch < 32 || ch > 126) return;
    i32 length = (i32)ax_strlen(panel->query);
    if (length >= (i32)sizeof(panel->query) - 1) return;
    if (panel->caret < 0) panel->caret = 0;
    if (panel->caret > length) panel->caret = length;
    for (i32 i = length; i > panel->caret; i--) {
        panel->query[i] = panel->query[i - 1];
    }
    panel->query[panel->caret] = ch;
    panel->query[length + 1] = 0;
    panel->caret++;
    axau_search_perform(panel);
}

void axau_search_backspace(axau_search_panel_t *panel)
{
    if (!panel) return;
    i32 length = (i32)ax_strlen(panel->query);
    if (panel->caret <= 0) return;
    for (i32 i = panel->caret - 1; i < length; i++) {
        panel->query[i] = panel->query[i + 1];
    }
    panel->caret--;
    axau_search_perform(panel);
}

void axau_search_paint(gui_surface_t *surface, axau_search_panel_t *panel,
                       const axau_palette_t *palette, const axau_metrics_t *metrics)
{
    if (!surface || !panel || !palette) return;
    if (!panel->open && panel->progress <= 0) return;
    (void)metrics;
    gui_fill_round_rect(surface, panel->rect, AXAU_RADIUS_FLYOUT,
                        axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
    gui_rect_t field = gui_rect(panel->rect.x + 16, panel->rect.y + 16,
                                panel->rect.w - 32, 40);
    gui_fill_round_rect(surface, field, 6,
                        axau_colour(palette, AXAU_COLOUR_CONTROL));
    axau_icon_glyph(surface, field.x + 16, field.y + field.h / 2, 12, "?",
                    axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY));
    axui_draw_text(surface, field.x + 32, field.y + 12,
                   panel->query[0] ? panel->query : "Search apps and files",
                   axui_style(panel->query[0]
                                  ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                  : axau_colour(palette,
                                                AXAU_COLOUR_TEXT_TERTIARY), 1));
    /* Caret sits after the measured text so it follows the real glyph advance. */
    i32 caret_x = field.x + 32 + panel->caret * 8;
    gui_fill_rect(surface, gui_rect(caret_x, field.y + 12, 1, 16),
                  axau_colour(palette, AXAU_COLOUR_TEXT));

    i32 y = panel->rect.y + 68;
    i32 drawn = 0;
    for (i32 i = 0; i < panel->match_count && drawn < 8; i++) {
        i32 index = panel->matches[i];
        axau_search_item_t *item = &panel->items[index];
        gui_rect_t row = gui_rect(panel->rect.x + 8, y, panel->rect.w - 16, 48);
        bool_t selected = index == panel->selected;
        bool_t hot = index == panel->hover;
        if (selected) {
            gui_fill_round_rect(surface, row, 4,
                                axui_colour_opacity(
                                    axau_colour(palette, AXAU_COLOUR_ACCENT), 18));
        } else if (hot) {
            gui_fill_round_rect(surface, row, 4,
                                axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
        }
        axau_icon_glyph(surface, row.x + 20, row.y + 24, 14, item->glyph,
                        axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY));
        axui_draw_text_clip(surface, gui_rect(row.x + 40, row.y + 6, row.w - 60, 18),
                            row.x + 40, row.y + 6, item->title,
                            axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        axui_draw_text_clip(surface, gui_rect(row.x + 40, row.y + 24, row.w - 60, 16),
                            row.x + 40, row.y + 24, item->detail,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_TERTIARY), 1));
        axui_draw_text_right(surface,
                             gui_rect(row.x, row.y, row.w - 12, row.h),
                             item->section,
                             axui_style(axau_colour(palette,
                                                    AXAU_COLOUR_TEXT_TERTIARY), 1));
        y += 48;
        drawn++;
    }
    if (panel->match_count == 0 && panel->query[0]) {
        axui_draw_text(surface, panel->rect.x + 24, y + 8,
                       "Nothing matched. Try a shorter word.",
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
}

i32 axau_search_hit(axau_search_panel_t *panel, i32 x, i32 y)
{
    if (!panel || !panel->open) return -1;
    if (!gui_rect_contains(panel->rect, x, y)) return -1;
    i32 y0 = panel->rect.y + 68;
    i32 row_index = (y - y0) / 48;
    if (row_index < 0 || row_index >= panel->match_count) return -1;
    if (row_index > 7) return -1;
    panel->selected = panel->matches[row_index];
    return panel->selected;
}

bool_t axau_search_tick(axau_search_panel_t *panel)
{
    if (!panel) return (bool_t)0;
    if (panel->open && panel->progress < 1000) {
        panel->progress += 150;
        if (panel->progress > 1000) panel->progress = 1000;
        return (bool_t)1;
    }
    return (bool_t)0;
}

/* Hit test for the card grid.  The geometry mirrors taskview_grid() and the
 * paint loop exactly, so hovering a card highlights the window it previews.
 * Returns the workspace index of the hit window, or -1 for empty space. */
i32 axau_taskview_hit(axau_taskview_t *view, axau_workspace_t *workspace,
                      i32 x, i32 y)
{
    if (!view || !workspace) return -1;
    if (!view->open) return -1;
    if (!gui_rect_contains(view->rect, x, y)) return -1;

    i32 card_w = 0, card_h = 0, gap = 0;
    i32 columns = taskview_grid(view, &card_w, &card_h, &gap);
    i32 top = view->rect.y + 80;
    i32 index = 0;

    for (i32 i = 0; i < workspace->count; i++) {
        axau_window_t *window = &workspace->windows[i];
        if (window->workspace != workspace->current) continue;
        if (window->state == AXAU_WINDOW_CLOSING ||
            window->state == AXAU_WINDOW_HIDDEN) {
            continue;
        }
        i32 column = index % columns;
        i32 row = index / columns;
        gui_rect_t card = gui_rect(view->rect.x + gap + column * (card_w + gap),
                                   top + row * (card_h + gap), card_w, card_h);
        if (gui_rect_contains(card, x, y)) return i;
        index++;
    }
    return -1;
}
