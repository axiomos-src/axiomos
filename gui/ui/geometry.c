/* AxiomOS UI - geometry and damage tracking.
 *
 * Every widget in the toolkit works in absolute screen coordinates and reports
 * its damage as rectangles that the compositor unions into a bounded list.
 * Keeping the list capped (AXUI_DAMAGE_MAX) is deliberate: a pathological
 * application that dirties hundreds of rectangles would otherwise force the
 * compositor to repaint the whole desktop, which is what the cap triggers
 * anyway through the "full" flag.
 */

#include "ui.h"

/* ------------------------------------------------------------------ */
/* Constructors                                                        */
/* ------------------------------------------------------------------ */

axui_point_t axui_point(i32 x, i32 y)
{
    axui_point_t p;
    p.x = x;
    p.y = y;
    return p;
}

axui_size_t axui_size(i32 w, i32 h)
{
    axui_size_t s;
    s.w = w;
    s.h = h;
    return s;
}

axui_edge_t axui_edge(i32 all)
{
    axui_edge_t e;
    e.left = all;
    e.top = all;
    e.right = all;
    e.bottom = all;
    return e;
}

axui_edge_t axui_edge_trbl(i32 top, i32 right, i32 bottom, i32 left)
{
    axui_edge_t e;
    e.top = top;
    e.right = right;
    e.bottom = bottom;
    e.left = left;
    return e;
}

/* ------------------------------------------------------------------ */
/* Rectangles                                                          */
/* ------------------------------------------------------------------ */

gui_rect_t axui_rect_inset(gui_rect_t rect, axui_edge_t edge)
{
    rect.x += edge.left;
    rect.y += edge.top;
    rect.w -= edge.left + edge.right;
    rect.h -= edge.top + edge.bottom;
    if (rect.w < 0) {
        rect.w = 0;
    }
    if (rect.h < 0) {
        rect.h = 0;
    }
    return rect;
}

gui_rect_t axui_rect_inset_px(gui_rect_t rect, i32 px)
{
    return axui_rect_inset(rect, axui_edge(px));
}

gui_rect_t axui_rect_union(gui_rect_t a, gui_rect_t b)
{
    if (axui_rect_empty(a)) {
        return b;
    }
    if (axui_rect_empty(b)) {
        return a;
    }
    i32 x0 = axui_min_i32(a.x, b.x);
    i32 y0 = axui_min_i32(a.y, b.y);
    i32 x1 = axui_max_i32(a.x + a.w, b.x + b.w);
    i32 y1 = axui_max_i32(a.y + a.h, b.y + b.h);
    return gui_rect(x0, y0, x1 - x0, y1 - y0);
}

bool_t axui_rect_empty(gui_rect_t rect)
{
    return (bool_t)(rect.w <= 0 || rect.h <= 0);
}

bool_t axui_rect_overlaps(gui_rect_t a, gui_rect_t b)
{
    if (axui_rect_empty(a) || axui_rect_empty(b)) {
        return false;
    }
    return (bool_t)(a.x < b.x + b.w && b.x < a.x + a.w &&
                    a.y < b.y + b.h && b.y < a.y + a.h);
}

gui_rect_t axui_rect_center_in(gui_rect_t outer, i32 w, i32 h)
{
    return gui_rect(outer.x + (outer.w - w) / 2, outer.y + (outer.h - h) / 2, w, h);
}

i32 axui_rect_area(gui_rect_t rect)
{
    if (axui_rect_empty(rect)) {
        return 0;
    }
    return rect.w * rect.h;
}

/* The split helpers follow the GTK/Flutter convention: the returned rectangle
 * is the carved-out strip and the caller's rectangle is what remains. */
gui_rect_t axui_rect_split_left(gui_rect_t *rect, i32 amount)
{
    gui_rect_t out;
    if (amount < 0) {
        amount = 0;
    }
    if (amount > rect->w) {
        amount = rect->w;
    }
    out = gui_rect(rect->x, rect->y, amount, rect->h);
    rect->x += amount;
    rect->w -= amount;
    return out;
}

gui_rect_t axui_rect_split_right(gui_rect_t *rect, i32 amount)
{
    gui_rect_t out;
    if (amount < 0) {
        amount = 0;
    }
    if (amount > rect->w) {
        amount = rect->w;
    }
    out = gui_rect(rect->x + rect->w - amount, rect->y, amount, rect->h);
    rect->w -= amount;
    return out;
}

gui_rect_t axui_rect_split_top(gui_rect_t *rect, i32 amount)
{
    gui_rect_t out;
    if (amount < 0) {
        amount = 0;
    }
    if (amount > rect->h) {
        amount = rect->h;
    }
    out = gui_rect(rect->x, rect->y, rect->w, amount);
    rect->y += amount;
    rect->h -= amount;
    return out;
}

gui_rect_t axui_rect_split_bottom(gui_rect_t *rect, i32 amount)
{
    gui_rect_t out;
    if (amount < 0) {
        amount = 0;
    }
    if (amount > rect->h) {
        amount = rect->h;
    }
    out = gui_rect(rect->x, rect->y + rect->h - amount, rect->w, amount);
    rect->h -= amount;
    return out;
}

i32 axui_clamp_i32(i32 v, i32 lo, i32 hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

i32 axui_min_i32(i32 a, i32 b)
{
    return a < b ? a : b;
}

i32 axui_max_i32(i32 a, i32 b)
{
    return a > b ? a : b;
}

/* ------------------------------------------------------------------ */
/* Damage                                                              */
/* ------------------------------------------------------------------ */

void axui_damage_reset(axui_damage_t *damage)
{
    damage->count = 0;
    damage->full = false;
}

/* Insertion keeps the list merge-friendly: before appending, try to merge with
 * an existing rectangle when one contains the other, and otherwise coalesce
 * when the union is not much larger than the sum of the parts. */
void axui_damage_add(axui_damage_t *damage, gui_rect_t rect)
{
    if (axui_rect_empty(rect) || damage->full) {
        return;
    }

    for (u32 i = 0; i < damage->count; i++) {
        gui_rect_t *existing = &damage->rects[i];
        gui_rect_t inter = gui_rect_intersect(*existing, rect);

        if (!axui_rect_empty(inter)) {
            gui_rect_t merged = axui_rect_union(*existing, rect);
            i32 merged_area = axui_rect_area(merged);
            i32 sum_area = axui_rect_area(*existing) + axui_rect_area(rect);
            if (merged_area <= sum_area + sum_area / 4) {
                *existing = merged;
                return;
            }
        } else if (axui_rect_area(axui_rect_union(*existing, rect)) <=
                   (axui_rect_area(*existing) + axui_rect_area(rect)) * 5 / 4) {
            *existing = axui_rect_union(*existing, rect);
            return;
        }
    }

    if (damage->count >= AXUI_DAMAGE_MAX) {
        damage->full = true;
        damage->count = 0;
        return;
    }
    damage->rects[damage->count++] = rect;
}

void axui_damage_add_px(axui_damage_t *damage, i32 x, i32 y, i32 w, i32 h)
{
    axui_damage_add(damage, gui_rect(x, y, w, h));
}

void axui_damage_merge(axui_damage_t *dst, const axui_damage_t *src)
{
    if (dst->full) {
        return;
    }
    if (src->full) {
        dst->full = true;
        dst->count = 0;
        return;
    }
    for (u32 i = 0; i < src->count; i++) {
        axui_damage_add(dst, src->rects[i]);
    }
}

bool_t axui_damage_empty(const axui_damage_t *damage)
{
    return (bool_t)(!damage->full && damage->count == 0);
}

gui_rect_t axui_damage_bounds(const axui_damage_t *damage)
{
    gui_rect_t bounds = gui_rect(0, 0, 0, 0);
    for (u32 i = 0; i < damage->count; i++) {
        bounds = axui_rect_union(bounds, damage->rects[i]);
    }
    return bounds;
}
