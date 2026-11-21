/*
 * AxiomOS - Aurora compositor.
 *
 * The compositor owns the frame: what is damaged, what is occluded, what order
 * layers paint in, and how much of a frame the shell is allowed to spend.  It
 * does not own any pixels of its own — every layer borrows a surface from its
 * application or from the shell.
 *
 * Damage tracking is the only real optimisation here, and it is deliberately
 * conservative.  A dirty rectangle is merged with the damaged list instead of
 * being clipped against it; merging until the list is full and then collapsing
 * to a single bounding box is cheaper and more predictable than an exact region
 * algebra that would need allocation.  A falsely large damage rect costs fill
 * rate; a missed one costs correctness.  This chooses fill rate.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXC_MAX_LAYERS      24
#define AXC_MAX_DAMAGE      16
#define AXC_FRAME_BUDGET_US 16666

typedef enum axc_layer_kind {
    AXC_LAYER_DESKTOP = 0,
    AXC_LAYER_WINDOW,
    AXC_LAYER_OVERLAY,
    AXC_LAYER_CURSOR
} axc_layer_kind_t;

typedef struct axc_layer {
    gui_surface_t      *surface;      /* borrowed; never freed here */
    gui_rect_t          bounds;       /* screen-space, after transform */
    i32                 z;
    i32                 opacity;      /* 0..255 */
    axc_layer_kind_t    kind;
    bool_t              visible;
    bool_t              opaque;       /* fully covers its bounds */
    bool_t              accepts_input;
    i32                 scale;        /* percent, 100 = native */
    gui_rect_t          clip;         /* additional clip, zero means none */
    bool_t              has_clip;
} axc_layer_t;

typedef struct axc_compositor {
    gui_rect_t   screen;
    axc_layer_t  layers[AXC_MAX_LAYERS];
    i32          count;
    gui_rect_t   damage[AXC_MAX_DAMAGE];
    i32          damage_count;
    bool_t       full_repaint;
    i32          frame_index;
    u64          last_frame_ns;
    i32          last_fill_pixels;
    i32          last_layer_count;
    bool_t       stats_enabled;
    i32          cursor_x;
    i32          cursor_y;
    bool_t       cursor_visible;
    i32          budget_used;
} axc_compositor_t;

/* Damage is declared first because adding or moving a layer reports it. */
void axc_damage(axc_compositor_t *compositor, gui_rect_t rect);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void axc_init(axc_compositor_t *compositor, gui_rect_t screen)
{
    if (!compositor) return;
    ax_memset(compositor, 0, sizeof(*compositor));
    compositor->screen = screen;
    compositor->full_repaint = (bool_t)1;
    compositor->cursor_visible = (bool_t)1;
    compositor->cursor_x = screen.x + screen.w / 2;
    compositor->cursor_y = screen.y + screen.h / 2;
}

i32 axc_add_layer(axc_compositor_t *compositor, gui_surface_t *surface,
                  gui_rect_t bounds, axc_layer_kind_t kind)
{
    if (!compositor || compositor->count >= AXC_MAX_LAYERS) return -1;
    i32 index = compositor->count++;
    axc_layer_t *layer = &compositor->layers[index];
    ax_memset(layer, 0, sizeof(*layer));
    layer->surface = surface;
    layer->bounds = bounds;
    layer->kind = kind;
    layer->z = index;
    layer->opacity = 255;
    layer->visible = (bool_t)1;
    layer->accepts_input = (bool_t)(kind != AXC_LAYER_CURSOR);
    layer->scale = 100;
    axc_damage(compositor, bounds);
    return index;
}

void axc_remove_layer(axc_compositor_t *compositor, i32 index)
{
    if (!compositor || index < 0 || index >= compositor->count) return;
    axc_damage(compositor, compositor->layers[index].bounds);
    for (i32 i = index; i + 1 < compositor->count; i++) {
        compositor->layers[i] = compositor->layers[i + 1];
    }
    compositor->count--;
}

void axc_set_bounds(axc_compositor_t *compositor, i32 index, gui_rect_t bounds)
{
    if (!compositor || index < 0 || index >= compositor->count) return;
    axc_layer_t *layer = &compositor->layers[index];
    /* Damage both the old and the new rect: moving a layer leaves a hole unless
     * the vacated area is repainted. */
    axc_damage(compositor, layer->bounds);
    layer->bounds = bounds;
    axc_damage(compositor, bounds);
}

void axc_set_opacity(axc_compositor_t *compositor, i32 index, i32 opacity)
{
    if (!compositor || index < 0 || index >= compositor->count) return;
    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;
    if (compositor->layers[index].opacity != opacity) {
        compositor->layers[index].opacity = opacity;
        axc_damage(compositor, compositor->layers[index].bounds);
    }
}

void axc_raise(axc_compositor_t *compositor, i32 index)
{
    if (!compositor || index < 0 || index >= compositor->count) return;
    i32 z = compositor->layers[index].z;
    for (i32 i = 0; i < compositor->count; i++) {
        if (i == index) continue;
        if (compositor->layers[i].z > z) compositor->layers[i].z--;
    }
    compositor->layers[index].z = compositor->count - 1;
    axc_damage(compositor, compositor->layers[index].bounds);
}

/* ------------------------------------------------------------------ */
/* Damage                                                              */
/* ------------------------------------------------------------------ */

static bool_t axc_rects_touch(gui_rect_t a, gui_rect_t b)
{
    return gui_rect_intersect(a, b).w > 0 && gui_rect_intersect(a, b).h > 0;
}

static gui_rect_t axc_rect_union(gui_rect_t a, gui_rect_t b)
{
    i32 x0 = a.x < b.x ? a.x : b.x;
    i32 y0 = a.y < b.y ? a.y : b.y;
    i32 x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    i32 y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return gui_rect(x0, y0, x1 - x0, y1 - y0);
}

/* Crop damage to the screen before storing it: drawing outside the screen is
 * wasted work and makes the damage list lie about its area. */
void axc_damage(axc_compositor_t *compositor, gui_rect_t rect)
{
    if (!compositor || rect.w <= 0 || rect.h <= 0) return;
    gui_rect_t clipped = gui_rect_intersect(rect, compositor->screen);
    if (clipped.w <= 0 || clipped.h <= 0) return;
    if (compositor->full_repaint) return;
    for (i32 i = 0; i < compositor->damage_count; i++) {
        if (axc_rects_touch(compositor->damage[i], clipped)) {
            compositor->damage[i] = axc_rect_union(compositor->damage[i], clipped);
            /* Merge any other rectangles the newly grown one now touches, so the
             * list does not accumulate overlapping entries. */
            for (i32 j = i + 1; j < compositor->damage_count; j++) {
                if (axc_rects_touch(compositor->damage[i],
                                    compositor->damage[j])) {
                    compositor->damage[i] =
                        axc_rect_union(compositor->damage[i],
                                       compositor->damage[j]);
                    for (i32 k = j; k + 1 < compositor->damage_count; k++) {
                        compositor->damage[k] = compositor->damage[k + 1];
                    }
                    compositor->damage_count--;
                    j--;
                }
            }
            return;
        }
    }
    if (compositor->damage_count >= AXC_MAX_DAMAGE) {
        /* Collapse everything into the bounding box of all damage plus this rect.
         * One big rectangle is honest: the frame will repaint more than needed,
         * and the next frame goes back to precise damage. */
        gui_rect_t all = clipped;
        for (i32 i = 0; i < compositor->damage_count; i++) {
            all = axc_rect_union(all, compositor->damage[i]);
        }
        compositor->damage[0] = all;
        compositor->damage_count = 1;
        return;
    }
    compositor->damage[compositor->damage_count++] = clipped;
}

i32 axc_damage_area(const axc_compositor_t *compositor)
{
    if (!compositor) return 0;
    if (compositor->full_repaint) {
        return compositor->screen.w * compositor->screen.h;
    }
    i32 area = 0;
    for (i32 i = 0; i < compositor->damage_count; i++) {
        area += compositor->damage[i].w * compositor->damage[i].h;
    }
    return area;
}

void axc_invalidate_all(axc_compositor_t *compositor)
{
    if (!compositor) return;
    compositor->full_repaint = (bool_t)1;
    compositor->damage_count = 0;
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

/* Layer order: cursor last, overlays above windows, desktop first.  A stable
 * sort by (kind, z) keeps the result deterministic when two layers share a z
 * value, which happens after a raise on a freshly added layer. */
static i32 axc_order(axc_compositor_t *compositor, i32 *indices)
{
    i32 count = 0;
    for (i32 i = 0; i < compositor->count; i++) {
        if (!compositor->layers[i].visible) continue;
        if (compositor->layers[i].opacity <= 0) continue;
        indices[count++] = i;
    }
    for (i32 i = 1; i < count; i++) {
        i32 key = indices[i];
        i32 j = i - 1;
        while (j >= 0) {
            axc_layer_t *a = &compositor->layers[indices[j]];
            axc_layer_t *b = &compositor->layers[key];
            bool_t after = (a->kind < b->kind) ||
                           (a->kind == b->kind && a->z <= b->z);
            if (after) break;
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = key;
    }
    return count;
}

/* An opaque desktop layer lets the compositor skip painting anything under it,
 * which is where most of the savings come from on a full repaint. */
static i32 axc_first_opaque(const axc_compositor_t *compositor, const i32 *order,
                            i32 count)
{
    i32 widest = -1;
    for (i32 i = 0; i < count; i++) {
        const axc_layer_t *layer = &compositor->layers[order[i]];
        if (layer->kind != AXC_LAYER_DESKTOP) continue;
        if (!layer->opaque) continue;
        gui_rect_t overlap = gui_rect_intersect(layer->bounds, compositor->screen);
        if (overlap.w == compositor->screen.w && overlap.h == compositor->screen.h) {
            widest = i;
        }
    }
    return widest;
}

/* Present one frame.  The driver calls this once per vertical blank; the return
 * value is the number of layers actually painted, which the statistics overlay
 * reports. */
i32 axc_frame(axc_compositor_t *compositor, gui_surface_t *target, u64 now_ns)
{
    if (!compositor || !target) return 0;
    i32 order[AXC_MAX_LAYERS];
    i32 count = axc_order(compositor, order);
    i32 start = 0;
    if (!compositor->full_repaint) {
        i32 opaque = axc_first_opaque(compositor, order, count);
        if (opaque > 0) start = opaque;
    }

    i32 painted = 0;
    i32 fill_pixels = 0;
    for (i32 i = start; i < count; i++) {
        axc_layer_t *layer = &compositor->layers[order[i]];
        if (!layer->surface) continue;
        painted++;
        if (compositor->full_repaint) {
            /* Full-screen copy path: the caller's surface already sits at the
             * layer bounds, so only the offset differs. */
            for (i32 row = 0; row < layer->bounds.h; row++) {
                i32 dy = layer->bounds.y + row;
                if (dy < 0 || dy >= target->height) continue;
                i32 sy = row;
                if (sy >= layer->surface->height) break;
                u32 *src = (u32 *)((u8 *)layer->surface->pixels +
                                   (u32)sy * layer->surface->stride);
                u32 *dst = (u32 *)((u8 *)target->pixels +
                                   (u32)dy * target->stride) + layer->bounds.x;
                for (i32 x = 0; x < layer->bounds.w; x++) {
                    if (x >= layer->surface->width) break;
                    if (layer->opacity >= 255) {
                        dst[x] = src[x];
                    } else {
                        dst[x] = axui_blend_alpha(dst[x], src[x], (u8)layer->opacity);
                    }
                }
                fill_pixels += layer->bounds.w;
            }
        } else {
            for (i32 d = 0; d < compositor->damage_count; d++) {
                gui_rect_t part = gui_rect_intersect(compositor->damage[d],
                                                     layer->bounds);
                if (part.w <= 0 || part.h <= 0) continue;
                for (i32 row = part.y; row < part.y + part.h; row++) {
                    i32 sy = row - layer->bounds.y;
                    if (sy < 0 || sy >= layer->surface->height) continue;
                    u32 *src = (u32 *)((u8 *)layer->surface->pixels +
                                       (u32)sy * layer->surface->stride);
                    u32 *dst = (u32 *)((u8 *)target->pixels +
                                       (u32)row * target->stride) + part.x;
                    i32 sx = part.x - layer->bounds.x;
                    if (sx < 0) sx = 0;
                    for (i32 x = 0; x < part.w; x++) {
                        i32 source_x = sx + x;
                        if (source_x >= layer->surface->width) break;
                        dst[x] = axui_blend_alpha(dst[x], src[source_x],
                                                   (u8)layer->opacity);
                    }
                    fill_pixels += part.w;
                }
            }
        }
    }

    compositor->frame_index++;
    compositor->last_frame_ns = now_ns;
    compositor->last_fill_pixels = fill_pixels;
    compositor->last_layer_count = painted;
    compositor->damage_count = 0;
    compositor->full_repaint = (bool_t)0;
    return painted;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/* Hit-testing walks the layer order backwards so the topmost interactive layer
 * wins.  The cursor layer never accepts input. */
i32 axc_layer_at(axc_compositor_t *compositor, i32 x, i32 y)
{
    if (!compositor) return -1;
    i32 order[AXC_MAX_LAYERS];
    i32 count = axc_order(compositor, order);
    for (i32 i = count - 1; i >= 0; i--) {
        axc_layer_t *layer = &compositor->layers[order[i]];
        if (!layer->accepts_input) continue;
        if (!gui_rect_contains(layer->bounds, x, y)) continue;
        if (layer->has_clip && !gui_rect_contains(layer->clip, x, y)) continue;
        return order[i];
    }
    return -1;
}

/* Occlusion test: if a layer is completely covered by an opaque layer above it, a
 * caller may skip updating its surface.  The answer is conservative — "not
 * occluded" is always safe — so a partial cover reports false. */
bool_t axc_is_occluded(axc_compositor_t *compositor, i32 index)
{
    if (!compositor || index < 0 || index >= compositor->count) return (bool_t)0;
    axc_layer_t *target = &compositor->layers[index];
    for (i32 i = 0; i < compositor->count; i++) {
        axc_layer_t *other = &compositor->layers[i];
        if (i == index || !other->visible || other->opacity < 255) continue;
        if (!other->opaque) continue;
        if (other->z <= target->z && other->kind == target->kind) continue;
        if (other->kind < target->kind) continue;
        gui_rect_t overlap = gui_rect_intersect(other->bounds, target->bounds);
        if (overlap.w >= target->bounds.w && overlap.h >= target->bounds.h) {
            return (bool_t)1;
        }
    }
    return (bool_t)0;
}

void axc_cursor(axc_compositor_t *compositor, i32 x, i32 y)
{
    if (!compositor) return;
    gui_rect_t before = gui_rect(compositor->cursor_x - 8,
                                 compositor->cursor_y - 8, 16, 16);
    gui_rect_t after = gui_rect(x - 8, y - 8, 16, 16);
    if (before.x != after.x || before.y != after.y) {
        axc_damage(compositor, before);
        axc_damage(compositor, after);
    }
    compositor->cursor_x = x;
    compositor->cursor_y = y;
}

/* The cursor is composited last, after all layers, so it is never hidden by an
 * un-maximised window edge.  Its shape is a simple arrow drawn from two
 * triangles: a filled body and a notch where the tail would be. */
void axc_cursor_paint(gui_surface_t *surface, axc_compositor_t *compositor,
                      const axau_palette_t *palette)
{
    if (!surface || !compositor || !palette) return;
    if (!compositor->cursor_visible) return;
    i32 x = compositor->cursor_x;
    i32 y = compositor->cursor_y;
    u32 outline = 0x00202020;
    u32 fill = 0x00F0F0F0;
    axui_fill_triangle(surface, axui_point(x, y), axui_point(x, y + 16),
                       axui_point(x + 5, y + 11), fill);
    axui_fill_triangle(surface, axui_point(x, y), axui_point(x + 5, y + 11),
                       axui_point(x + 10, y + 10), fill);
    axui_line_aa(surface, x, y, x, y + 16, outline, 1);
    axui_line_aa(surface, x, y + 16, x + 5, y + 11, outline, 1);
    axui_line_aa(surface, x + 5, y + 11, x + 10, y + 10, outline, 1);
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

typedef struct axc_stats {
    i32 frames;
    i64 total_fill;
    i32 worst_fill;
    i32 worst_layers;
    u64 first_frame_ns;
    u64 last_frame_ns;
} axc_stats_t;

void axc_stats_reset(axc_stats_t *stats)
{
    if (!stats) return;
    ax_memset(stats, 0, sizeof(*stats));
    stats->worst_fill = 0;
    stats->worst_layers = 0;
}

void axc_stats_accumulate(axc_stats_t *stats, const axc_compositor_t *compositor)
{
    if (!stats || !compositor) return;
    stats->frames++;
    stats->total_fill += compositor->last_fill_pixels;
    if (compositor->last_fill_pixels > stats->worst_fill) {
        stats->worst_fill = compositor->last_fill_pixels;
    }
    if (compositor->last_layer_count > stats->worst_layers) {
        stats->worst_layers = compositor->last_layer_count;
    }
    if (!stats->first_frame_ns) stats->first_frame_ns = compositor->last_frame_ns;
    stats->last_frame_ns = compositor->last_frame_ns;
}

/* Average fill per frame, in pixels.  Dividing by zero frames yields zero rather
 * than an error, because the overlay asks before the first frame. */
i32 axc_stats_average_fill(const axc_stats_t *stats)
{
    if (!stats || stats->frames <= 0) return 0;
    return (i32)(stats->total_fill / stats->frames);
}

void axc_dump(const axc_compositor_t *compositor)
{
    if (!compositor) return;
    printk("compositor: %d layers, frame %d, damage %d, last fill %d px\n",
           compositor->count, compositor->frame_index,
           compositor->damage_count, compositor->last_fill_pixels);
}


