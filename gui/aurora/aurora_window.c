/*
 * AxiomOS - window chrome and workspace management.
 *
 * The window frame is drawn by Aurora, not by the application: a rounded
 * translucent shell, a titlebar whose caption buttons follow the palette, and a
 * hairline border that exists only so two adjacent windows stay distinguishable.
 * Applications receive a plain client rect and never learn that the frame is
 * drawn behind them.
 *
 * Two decisions are worth stating because they are deliberate rather than
 * incidental.  First, the client rect is computed from the *unscaled* frame and
 * the opening animation scales only the painted pixels, so an application never
 * sees its layout change while the window animates in.  Second, maximising
 * keeps the restore rect forever; there is no "remember where it was" heuristic
 * that guesses from drag distance.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"
#include "aurora_apps.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static u32 clamp_u32(i32 value, i32 low, i32 high)
{
    if (value < low) return (u32)low;
    if (value > high) return (u32)high;
    return (u32)value;
}


/* Ease the animation clock.  Both the open and close animations use a curve
 * that starts fast and settles, which reads as "arriving" rather than "growing". */
static i32 eased_step(i32 progress, i32 step)
{
    i32 next = progress + step;
    if (next > 1000) next = 1000;
    return next;
}

/* ------------------------------------------------------------------ */
/* Init and geometry                                                   */
/* ------------------------------------------------------------------ */

void axau_window_init(axau_window_t *window, const char *title, gui_rect_t frame,
                      const char *glyph)
{
    if (!window) return;
    ax_memset(window, 0, sizeof(*window));
    window->frame = frame;
    window->restore = frame;
    ax_strlcpy(window->title, title ? title : "Window", sizeof(window->title));
    window->glyph = glyph ? glyph : "#";
    window->state = AXAU_WINDOW_NORMAL;
    window->resizable = (bool_t)1;
    window->closable = (bool_t)1;
    window->maximisable = (bool_t)1;
    window->content = axau_window_content_rect(window);
}

void axau_window_set_subtitle(axau_window_t *window, const char *subtitle)
{
    if (!window) return;
    ax_strlcpy(window->subtitle, subtitle ? subtitle : "",
               sizeof(window->subtitle));
}

gui_rect_t axau_window_content_rect(const axau_window_t *window)
{
    if (!window) return gui_rect(0, 0, 0, 0);
    i32 border = window->frame.w > 0 ? 1 : 0;
    gui_rect_t content = gui_rect(window->frame.x + border,
                                  window->frame.y + border + AXAU_TITLEBAR_HEIGHT,
                                  window->frame.w - border * 2,
                                  window->frame.h - border * 2 -
                                  AXAU_TITLEBAR_HEIGHT);
    if (content.w < 0) content.w = 0;
    if (content.h < 0) content.h = 0;
    return content;
}

void axau_window_move(axau_window_t *window, i32 x, i32 y)
{
    if (!window) return;
    window->frame.x = x;
    window->frame.y = y;
    window->content = axau_window_content_rect(window);
    window->dirty = (bool_t)1;
}

void axau_window_resize_to(axau_window_t *window, gui_rect_t frame,
                           gui_rect_t bounds)
{
    if (!window) return;
    i32 minimum_w = 240;
    i32 minimum_h = 160;
    if (frame.w < minimum_w) frame.w = minimum_w;
    if (frame.h < minimum_h) frame.h = minimum_h;
    if (bounds.w > 0) {
        if (frame.w > bounds.w) frame.w = bounds.w;
        if (frame.h > bounds.h + AXAU_TITLEBAR_HEIGHT) {
            frame.h = bounds.h + AXAU_TITLEBAR_HEIGHT;
        }
        if (frame.x + frame.w > bounds.x + bounds.w) {
            frame.x = bounds.x + bounds.w - frame.w;
        }
        if (frame.y + frame.h > bounds.y + bounds.h) {
            frame.y = bounds.y + bounds.h - frame.h;
        }
    }
    window->frame = frame;
    window->content = axau_window_content_rect(window);
    window->dirty = (bool_t)1;
}

void axau_window_maximise(axau_window_t *window, gui_rect_t bounds)
{
    if (!window || window->state == AXAU_WINDOW_MAXIMISED) return;
    window->restore = window->frame;
    window->frame = gui_rect(bounds.x, bounds.y, bounds.w, bounds.h);
    window->state = AXAU_WINDOW_MAXIMISED;
    window->snap = AXAU_SNAP_NONE;
    window->content = axau_window_content_rect(window);
    window->dirty = (bool_t)1;
}

void axau_window_restore(axau_window_t *window)
{
    if (!window) return;
    if (window->state == AXAU_WINDOW_MINIMISED ||
        window->state == AXAU_WINDOW_MAXIMISED) {
        window->frame = window->restore;
    }
    window->state = AXAU_WINDOW_NORMAL;
    window->snap = AXAU_SNAP_NONE;
    window->content = axau_window_content_rect(window);
    window->dirty = (bool_t)1;
}

/* Snap keeps the restore rect from *before* the snap, so a user who snaps twice
 * still returns to the original floating position rather than to the previous
 * snapped one. */
void axau_window_snap(axau_window_t *window, axau_snap_edge_t edge,
                      gui_rect_t bounds)
{
    if (!window) return;
    if (window->state != AXAU_WINDOW_MAXIMISED &&
        window->snap == AXAU_SNAP_NONE) {
        window->restore = window->frame;
    }
    i32 half_w = bounds.w / 2;
    i32 half_h = bounds.h / 2;
    switch (edge) {
    case AXAU_SNAP_LEFT:
        window->frame = gui_rect(bounds.x, bounds.y, half_w, bounds.h);
        break;
    case AXAU_SNAP_RIGHT:
        window->frame = gui_rect(bounds.x + half_w, bounds.y, half_w, bounds.h);
        break;
    case AXAU_SNAP_TOP:
        window->frame = gui_rect(bounds.x, bounds.y, bounds.w, bounds.h);
        window->state = AXAU_WINDOW_MAXIMISED;
        break;
    case AXAU_SNAP_BOTTOM_LEFT:
    case AXAU_SNAP_QUARTER_LEFT:
        window->frame = gui_rect(bounds.x, bounds.y, half_w, half_h);
        break;
    case AXAU_SNAP_BOTTOM_RIGHT:
    case AXAU_SNAP_QUARTER_RIGHT:
        window->frame = gui_rect(bounds.x + half_w, bounds.y, half_w, half_h);
        break;
    case AXAU_SNAP_NONE:
    default:
        break;
    }
    window->snap = edge;
    window->content = axau_window_content_rect(window);
    window->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Animation                                                           */
/* ------------------------------------------------------------------ */

void axau_window_begin_open(axau_window_t *window)
{
    if (!window) return;
    window->state = AXAU_WINDOW_OPENING;
    window->open_progress = 0;
    window->close_progress = 0;
}

void axau_window_begin_close(axau_window_t *window)
{
    if (!window) return;
    window->state = AXAU_WINDOW_CLOSING;
    window->close_progress = 0;
}

/* Returns true while the window still wants frames.  A closing window that has
 * finished reports true on the tick it completes so the workspace can remove it
 * in the same pass that erased it. */
bool_t axau_window_tick(axau_window_t *window)
{
    if (!window) return (bool_t)0;
    if (window->state == AXAU_WINDOW_OPENING) {
        window->open_progress = eased_step(window->open_progress, 90);
        if (window->open_progress >= 1000) {
            window->state = AXAU_WINDOW_NORMAL;
            return (bool_t)0;
        }
        window->dirty = (bool_t)1;
        return (bool_t)1;
    }
    if (window->state == AXAU_WINDOW_CLOSING) {
        window->close_progress = eased_step(window->close_progress, 110);
        window->dirty = (bool_t)1;
        return (bool_t)1;
    }
    if (window->snap_preview > 0 && !window->dragging) {
        window->snap_preview -= 60;
        if (window->snap_preview < 0) window->snap_preview = 0;
        window->dirty = (bool_t)1;
    }
    return (bool_t)0;
}

/* Percentage of the frame size used while opening: 92% growing to 100%. */
i32 axau_window_scale(const axau_window_t *window)
{
    if (!window) return 100;
    if (window->state == AXAU_WINDOW_OPENING) {
        return 92 + (window->open_progress * 8) / 1000;
    }
    return 100;
}

/* Alpha in percent, so the caller can use it with gui_blend_rect. */
i32 axau_window_alpha(const axau_window_t *window)
{
    if (!window) return 100;
    if (window->state == AXAU_WINDOW_OPENING) {
        return (window->open_progress * 100) / 1000;
    }
    if (window->state == AXAU_WINDOW_CLOSING) {
        i32 alpha = 100 - (window->close_progress * 100) / 1000;
        return alpha < 0 ? 0 : alpha;
    }
    return 100;
}

/* ------------------------------------------------------------------ */
/* Hit testing                                                         */
/* ------------------------------------------------------------------ */

static void caption_rects(const axau_window_t *window, gui_rect_t *close_rect,
                         gui_rect_t *max_rect, gui_rect_t *min_rect)
{
    i32 button_w = 44;
    i32 titlebar_y = window->frame.y + 1;
    gui_rect_t close = gui_rect(window->frame.x + window->frame.w - button_w,
                                titlebar_y, button_w, AXAU_TITLEBAR_HEIGHT);
    gui_rect_t maximise = gui_rect(close.x - button_w, titlebar_y, button_w,
                                   AXAU_TITLEBAR_HEIGHT);
    gui_rect_t minimise = gui_rect(maximise.x - button_w, titlebar_y, button_w,
                                   AXAU_TITLEBAR_HEIGHT);
    if (close_rect) *close_rect = close;
    if (max_rect) *max_rect = maximise;
    if (min_rect) *min_rect = minimise;
}

axau_frame_region_t axau_window_hit(const axau_window_t *window, i32 x, i32 y,
                                    bool_t *on_close, bool_t *on_max,
                                    bool_t *on_min)
{
    if (on_close) *on_close = (bool_t)0;
    if (on_max) *on_max = (bool_t)0;
    if (on_min) *on_min = (bool_t)0;
    if (!window) return AXAU_REGION_OUTSIDE;
    if (!gui_rect_contains(window->frame, x, y)) return AXAU_REGION_OUTSIDE;

    gui_rect_t close_rect, max_rect, min_rect;
    caption_rects(window, &close_rect, &max_rect, &min_rect);
    if (window->closable && gui_rect_contains(close_rect, x, y)) {
        if (on_close) *on_close = (bool_t)1;
        return AXAU_REGION_CAPTION_BUTTONS;
    }
    if (window->maximisable && gui_rect_contains(max_rect, x, y)) {
        if (on_max) *on_max = (bool_t)1;
        return AXAU_REGION_CAPTION_BUTTONS;
    }
    if (gui_rect_contains(min_rect, x, y)) {
        if (on_min) *on_min = (bool_t)1;
        return AXAU_REGION_CAPTION_BUTTONS;
    }

    gui_rect_t content = axau_window_content_rect(window);
    if (gui_rect_contains(content, x, y)) return AXAU_REGION_CLIENT;

    if (window->resizable && window->state != AXAU_WINDOW_MAXIMISED) {
        i32 margin = AXAU_RESIZE_MARGIN;
        bool_t left = x < window->frame.x + margin;
        bool_t right = x > window->frame.x + window->frame.w - margin;
        bool_t bottom = y > window->frame.y + window->frame.h - margin;
        bool_t top = y < window->frame.y + margin;
        if (left && bottom) return AXAU_REGION_BOTTOM_LEFT;
        if (right && bottom) return AXAU_REGION_BOTTOM_RIGHT;
        if (left) return AXAU_REGION_BORDER_LEFT;
        if (right) return AXAU_REGION_BORDER_RIGHT;
        if (bottom) return AXAU_REGION_BOTTOM;
        if (top) return AXAU_REGION_BORDER_TOP;
    }
    if (y < window->frame.y + AXAU_TITLEBAR_HEIGHT) return AXAU_REGION_TITLEBAR;
    return AXAU_REGION_CLIENT;
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

void axau_window_paint_shadow(gui_surface_t *surface, const axau_window_t *window,
                              const axau_palette_t *palette)
{
    if (!surface || !window || !palette) return;
    u8 strength = (u8)(axui_shadow_strength() + (window->active ? 4 : 0));
    (void)palette;
    gui_shadow(surface, window->frame, window->active ? 18 : 12, strength);
}

/* The caption buttons use glyph strokes rather than a font: three marks that
 * stay crisp at any DPI because they are geometric. */
static void paint_caption_buttons(gui_surface_t *surface,
                                  const axau_window_t *window,
                                  const axau_palette_t *palette, i32 hover_close)
{
    gui_rect_t close_rect, max_rect, min_rect;
    caption_rects(window, &close_rect, &max_rect, &min_rect);
    u32 glyph_colour = axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY);

    if (hover_close == 1) {
        gui_fill_round_rect(surface, gui_rect(close_rect.x + 4, close_rect.y + 6,
                                              close_rect.w - 8, close_rect.h - 12),
                            4, axui_colour_opacity(0x00C04030, 40));
    }
    i32 cx = close_rect.x + close_rect.w / 2;
    i32 cy = close_rect.y + close_rect.h / 2;
    axui_line_aa(surface, cx - 5, cy - 5, cx + 5, cy + 5, glyph_colour, 2);
    axui_line_aa(surface, cx + 5, cy - 5, cx - 5, cy + 5, glyph_colour, 2);

    if (hover_close == 2) {
        gui_fill_round_rect(surface, gui_rect(max_rect.x + 4, max_rect.y + 6,
                                              max_rect.w - 8, max_rect.h - 12),
                            4, axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
    }
    i32 mx = max_rect.x + max_rect.w / 2;
    i32 my = max_rect.y + max_rect.h / 2;
    i32 half = window->state == AXAU_WINDOW_MAXIMISED ? 4 : 5;
    axui_round_rect_border(surface, gui_rect(mx - half, my - half, half * 2,
                                             half * 2), 2, 1, glyph_colour);
    if (window->state == AXAU_WINDOW_MAXIMISED) {
        axui_round_rect_border(surface, gui_rect(mx - half + 3, my - half - 3,
                                                 half * 2, half * 2), 2, 1,
                               glyph_colour);
    }

    i32 nx = min_rect.x + min_rect.w / 2;
    i32 ny = min_rect.y + min_rect.h / 2;
    axui_line_aa(surface, nx - 5, ny, nx + 5, ny, glyph_colour, 2);

    (void)clamp_u32;
}

void axau_window_paint_frame_only(gui_surface_t *surface, axau_window_t *window,
                                  const axau_palette_t *palette)
{
    if (!surface || !window || !palette) return;
    gui_fill_round_rect(surface, window->frame, AXAU_RADIUS_WINDOW,
                        axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
    gui_rect_t titlebar = gui_rect(window->frame.x, window->frame.y,
                                   window->frame.w, AXAU_TITLEBAR_HEIGHT);
    gui_fill_round_rect(surface, titlebar, AXAU_RADIUS_WINDOW,
                        axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    axau_icon_glyph(surface, window->frame.x + 18,
                    window->frame.y + AXAU_TITLEBAR_HEIGHT / 2, 12,
                    window->glyph ? window->glyph : "#",
                    window->active ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                                   : axau_colour(palette,
                                                 AXAU_COLOUR_TEXT_SECONDARY));
    axui_draw_text(surface, window->frame.x + 34,
                   window->frame.y + AXAU_TITLEBAR_HEIGHT / 2 - 8,
                   window->title,
                   axui_style(window->active
                                  ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                  : axau_colour(palette,
                                                AXAU_COLOUR_TEXT_SECONDARY), 1));
    paint_caption_buttons(surface, window, palette, -1);
    axui_round_rect_border(surface, window->frame, AXAU_RADIUS_WINDOW, 1,
                           axau_colour(palette, AXAU_COLOUR_STROKE));
}

void axau_window_paint(gui_surface_t *surface, axau_window_t *window,
                       const axau_palette_t *palette, const axau_metrics_t *metrics)
{
    if (!surface || !window || !palette) return;
    (void)metrics;
    axau_window_paint_shadow(surface, window, palette);
    axau_window_paint_frame_only(surface, window, palette);

    if (window->subtitle[0]) {
        axui_draw_text(surface, window->frame.x + 34,
                       window->frame.y + AXAU_TITLEBAR_HEIGHT - 10,
                       window->subtitle,
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_TERTIARY), 1));
    }

    gui_rect_t content = axau_window_content_rect(window);
    gui_fill_rect(surface, content, axau_colour(palette, AXAU_COLOUR_SURFACE));

}

/* ------------------------------------------------------------------ */
/* Workspace                                                           */
/* ------------------------------------------------------------------ */

void axau_workspace_init(axau_workspace_t *workspace, gui_rect_t bounds)
{
    if (!workspace) return;
    ax_memset(workspace, 0, sizeof(*workspace));
    workspace->bounds = bounds;
    workspace->active = -1;
    workspace->current = 0;
    workspace->workspace_count = 1;
    workspace->next_id = 1;
}

i32 axau_workspace_add(axau_workspace_t *workspace, const char *title,
                       gui_rect_t frame, const char *glyph)
{
    if (!workspace || workspace->count >= AXAU_MAX_WINDOWS) return -1;
    i32 index = workspace->count++;
    axau_window_t *window = &workspace->windows[index];
    axau_window_init(window, title, frame, glyph);
    window->id = workspace->next_id++;
    window->z_order = index;
    window->workspace = workspace->current;
    axau_window_begin_open(window);
    axau_workspace_activate(workspace, index);
    return index;
}

/* Topmost window under the pointer.  Iteration runs from the front so the
 * result is the window a user would expect to click. */
i32 axau_workspace_find(axau_workspace_t *workspace, i32 x, i32 y)
{
    if (!workspace) return -1;
    i32 best = -1;
    i32 best_z = -1;
    for (i32 i = 0; i < workspace->count; i++) {
        axau_window_t *window = &workspace->windows[i];
        if (window->workspace != workspace->current) continue;
        if (window->state == AXAU_WINDOW_HIDDEN ||
            window->state == AXAU_WINDOW_MINIMISED) continue;
        if (!gui_rect_contains(window->frame, x, y)) continue;
        if (window->z_order > best_z) {
            best_z = window->z_order;
            best = i;
        }
    }
    return best;
}

void axau_workspace_raise(axau_workspace_t *workspace, i32 index)
{
    if (!workspace || index < 0 || index >= workspace->count) return;
    /* Shift everything above the target down one slot; the target takes the
     * highest z.  z values are small integers and never overflow. */
    i32 target_z = workspace->windows[index].z_order;
    for (i32 i = 0; i < workspace->count; i++) {
        if (i == index) continue;
        if (workspace->windows[i].z_order > target_z) {
            workspace->windows[i].z_order--;
        }
    }
    workspace->windows[index].z_order = workspace->count - 1;
}

void axau_workspace_activate(axau_workspace_t *workspace, i32 index)
{
    if (!workspace) return;
    for (i32 i = 0; i < workspace->count; i++) {
        workspace->windows[i].active = (bool_t)(i == index);
    }
    workspace->active = index;
    if (index >= 0) axau_workspace_raise(workspace, index);
}

/* Closing marks the window as closing; the actual removal happens in tick so
 * the animation has a chance to play. */
void axau_workspace_close(axau_workspace_t *workspace, i32 index)
{
    if (!workspace || index < 0 || index >= workspace->count) return;
    axau_window_begin_close(&workspace->windows[index]);
    workspace->windows[index].active = (bool_t)0;
    if (workspace->active == index) workspace->active = -1;
}

void axau_workspace_tick(axau_workspace_t *workspace)
{
    if (!workspace) return;
    i32 victim = -1;
    for (i32 i = 0; i < workspace->count; i++) {
        axau_window_t *window = &workspace->windows[i];
        axau_window_tick(window);
        if (window->state == AXAU_WINDOW_CLOSING &&
            window->close_progress >= 1000) {
            victim = i;
        }
    }
    if (victim >= 0) {
        /* Compact the array, which invalidates indices held by the application
         * layer.  The shell re-resolves windows by id after any close. */
        for (i32 i = victim; i + 1 < workspace->count; i++) {
            workspace->windows[i] = workspace->windows[i + 1];
        }
        workspace->count--;
        if (workspace->active >= workspace->count) {
            workspace->active = workspace->count - 1;
        }
        if (workspace->active >= 0) {
            workspace->windows[workspace->active].active = (bool_t)1;
        }
    }
}

i32 axau_workspace_visible_count(const axau_workspace_t *workspace)
{
    if (!workspace) return 0;
    i32 count = 0;
    for (i32 i = 0; i < workspace->count; i++) {
        const axau_window_t *window = &workspace->windows[i];
        if (window->workspace == workspace->current &&
            window->state != AXAU_WINDOW_HIDDEN &&
            window->state != AXAU_WINDOW_MINIMISED) {
            count++;
        }
    }
    return count;
}

void axau_workspace_paint(gui_surface_t *surface, axau_workspace_t *workspace,
                          const axau_palette_t *palette, const axau_metrics_t *metrics)
{
    if (!surface || !workspace || !palette) return;
    /* Paint by z order: find the lowest unpainted z each pass.  Sixteen windows
     * make an O(n^2) pass cheaper than maintaining a sorted array. */
    for (i32 painted = 0; painted < workspace->count; painted++) {
        i32 best = -1;
        i32 best_z = 0x7FFFFFFF;
        for (i32 i = 0; i < workspace->count; i++) {
            axau_window_t *window = &workspace->windows[i];
            if (window->workspace != workspace->current) continue;
            if (window->state == AXAU_WINDOW_HIDDEN ||
                window->state == AXAU_WINDOW_MINIMISED) continue;
            if (window->z_order < best_z) {
                best_z = window->z_order;
                best = i;
            }
        }
        if (best < 0) break;
        axau_window_paint(surface, &workspace->windows[best], palette, metrics);
        workspace->windows[best].z_order = 0x10000 + painted;
        workspace->windows[best].dirty = (bool_t)0;
    }
    /* Restore the original order after painting. */
    for (i32 i = 0; i < workspace->count; i++) {
        if (workspace->windows[i].z_order >= 0x10000) {
            workspace->windows[i].z_order -= 0x10000;
        }
    }
}

bool_t axau_workspace_cycle(axau_workspace_t *workspace, i32 delta)
{
    if (!workspace || workspace->count < 2) return (bool_t)0;
    i32 start = workspace->active >= 0 ? workspace->active : 0;
    for (i32 step = 1; step <= workspace->count; step++) {
        i32 index = (start + delta * step + workspace->count * 4) %
                    workspace->count;
        if (workspace->windows[index].state != AXAU_WINDOW_CLOSING &&
            workspace->windows[index].workspace == workspace->current) {
            axau_workspace_activate(workspace, index);
            return (bool_t)1;
        }
    }
    return (bool_t)0;
}

bool_t axau_workspace_snap_active(axau_workspace_t *workspace,
                                  axau_snap_edge_t edge)
{
    if (!workspace || workspace->active < 0) return (bool_t)0;
    axau_window_snap(&workspace->windows[workspace->active], edge,
                     workspace->bounds);
    return (bool_t)1;
}



