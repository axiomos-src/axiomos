/* AxiomOS UI - layout engine.
 *
 * A deliberately small flex/stack engine.  Widgets do not position themselves
 * in absolute coordinates; they are placed into a cursor-based layout that
 * walks either down a column or across a row.  Three properties cover every
 * screen in the shell:
 *
 *   - fixed size (width/height given),
 *   - fill (consume the remaining cross-axis space),
 *   - grow (a flex weight that divides the remaining main-axis space).
 *
 * The cursor-based form is allocation free and deterministic, which matters
 * because the compositor runs layout on every resize and the kernel has a
 * fixed heap.
 */

#include <axiom/lib.h>
#include "ui.h"

/* ------------------------------------------------------------------ */
/* Box specs                                                           */
/* ------------------------------------------------------------------ */

axui_box_spec_t axui_box_fixed(i32 w, i32 h)
{
    axui_box_spec_t spec;
    ax_memset(&spec, 0, sizeof(spec));
    spec.width = w;
    spec.height = h;
    spec.grow = 0;
    spec.basis = 0;
    spec.margin = axui_edge(0);
    spec.padding = axui_edge(0);
    return spec;
}

axui_box_spec_t axui_box_fill(void)
{
    axui_box_spec_t spec;
    ax_memset(&spec, 0, sizeof(spec));
    spec.width = -1;
    spec.height = -1;
    spec.grow = 1;
    spec.margin = axui_edge(0);
    spec.padding = axui_edge(0);
    return spec;
}

axui_box_spec_t axui_box_grow(i32 weight)
{
    axui_box_spec_t spec;
    ax_memset(&spec, 0, sizeof(spec));
    spec.width = -1;
    spec.height = -1;
    spec.grow = weight > 0 ? weight : 1;
    spec.margin = axui_edge(0);
    spec.padding = axui_edge(0);
    return spec;
}

/* Convenience constructors the widget code uses a lot. */
axui_box_spec_t axui_box_sized(i32 w, i32 h, i32 grow)
{
    axui_box_spec_t spec = axui_box_fixed(w, h);
    spec.grow = grow;
    return spec;
}

axui_box_spec_t axui_box_padded(axui_box_spec_t spec, axui_edge_t padding)
{
    spec.padding = padding;
    return spec;
}

axui_box_spec_t axui_box_margined(axui_box_spec_t spec, axui_edge_t margin)
{
    spec.margin = margin;
    return spec;
}

axui_box_spec_t axui_box_min(axui_box_spec_t spec, i32 min_w, i32 min_h)
{
    spec.min_width = min_w;
    spec.min_height = min_h;
    return spec;
}

/* ------------------------------------------------------------------ */
/* Layout cursors                                                      */
/* ------------------------------------------------------------------ */

void axui_layout_init(axui_layout_t *layout, gui_rect_t area, bool_t vertical,
                      i32 gap)
{
    if (!layout) {
        return;
    }
    layout->area = area;
    layout->cursor = vertical ? area.y : area.x;
    layout->gap = gap;
    layout->padding = axui_edge(0);
    layout->vertical = vertical;
}

void axui_layout_padding(axui_layout_t *layout, axui_edge_t padding)
{
    if (!layout) {
        return;
    }
    layout->padding = padding;
    layout->area = axui_rect_inset(layout->area, padding);
    layout->cursor = layout->vertical ? layout->area.y : layout->area.x;
}

i32 axui_layout_gap_total(i32 count, i32 gap)
{
    if (count <= 1) {
        return 0;
    }
    return (count - 1) * gap;
}

/* Place the next item with a fully specified box.  When the spec asks for
 * fill on the cross axis the returned rect spans the layout's width (or
 * height); the main axis advances by the item's size plus the gap. */
gui_rect_t axui_layout_next(axui_layout_t *layout, const axui_box_spec_t *spec,
                            i32 total_children, i32 *out_grow_total)
{
    gui_rect_t empty = gui_rect(0, 0, 0, 0);
    if (!layout) {
        return empty;
    }

    i32 main_size;
    i32 cross_size;
    i32 cross_extent = layout->vertical ? layout->area.w : layout->area.h;

    if (!spec) {
        main_size = 24;
        cross_size = cross_extent;
    } else {
        main_size = layout->vertical ? spec->height : spec->width;
        cross_size = layout->vertical ? spec->width : spec->height;
        if (main_size < 0) {
            /* "fill" on the main axis means "take what is left".  With a
             * single child that is the rest of the area; with several the
             * caller is expected to use axui_layout_place_grow instead. */
            i32 remaining = (layout->vertical ? layout->area.y + layout->area.h
                                             : layout->area.x + layout->area.w) -
                            layout->cursor;
            main_size = remaining > 0 ? remaining : 0;
        }
        if (cross_size < 0) {
            cross_size = cross_extent;
        }
    }

    if (out_grow_total && spec) {
        *out_grow_total += spec->grow;
    }

    /* Respect the declared minimums. */
    if (spec) {
        if (layout->vertical && spec->min_height > main_size) {
            main_size = spec->min_height;
        }
        if (!layout->vertical && spec->min_width > main_size) {
            main_size = spec->min_width;
        }
        if (layout->vertical && spec->min_width > cross_size) {
            cross_size = spec->min_width;
        }
        if (!layout->vertical && spec->min_height > cross_size) {
            cross_size = spec->min_height;
        }
    }

    gui_rect_t rect;
    if (layout->vertical) {
        rect = gui_rect(layout->area.x, layout->cursor, cross_size, main_size);
        layout->cursor += main_size + layout->gap;
    } else {
        rect = gui_rect(layout->cursor, layout->area.y, main_size, cross_size);
        layout->cursor += main_size + layout->gap;
    }

    /* Grow total bookkeeping: when the caller passes a pointer it is
     * accumulating the weights of all children, then calling
     * axui_layout_place_grow for the flexible ones. */
    (void)total_children;
    return rect;
}

gui_rect_t axui_layout_place(axui_layout_t *layout, i32 w, i32 h)
{
    axui_box_spec_t spec = axui_box_fixed(w, h);
    return axui_layout_next(layout, &spec, 1, 0);
}

/* Distribute the remaining main-axis space among flexible children.  weight
 * and total_weight come from the caller's own accumulation pass; the result
 * is proportional, with the last child absorbing the rounding error. */
gui_rect_t axui_layout_place_grow(axui_layout_t *layout, const axui_box_spec_t *spec,
                                  i32 weight, i32 total_weight,
                                  i32 remaining_main, i32 cross_size)
{
    if (!layout) {
        return gui_rect(0, 0, 0, 0);
    }
    if (total_weight <= 0) {
        total_weight = 1;
    }
    if (weight <= 0) {
        weight = 1;
    }

    i32 share = (remaining_main * weight) / total_weight;
    i32 minimum = spec ? (layout->vertical ? spec->min_height : spec->min_width) : 0;
    if (share < minimum) {
        share = minimum;
    }

    i32 cross = cross_size;
    if (cross <= 0 && spec) {
        cross = layout->vertical ? spec->width : spec->height;
    }
    if (cross <= 0) {
        cross = layout->vertical ? layout->area.w : layout->area.h;
    }

    gui_rect_t rect;
    if (layout->vertical) {
        rect = gui_rect(layout->area.x, layout->cursor, cross, share);
    } else {
        rect = gui_rect(layout->cursor, layout->area.y, share, cross);
    }
    layout->cursor += share + layout->gap;
    return rect;
}

/* ------------------------------------------------------------------ */
/* Alignment                                                           */
/* ------------------------------------------------------------------ */

/* The six alignments every container in the shell needs. */
gui_rect_t axui_layout_align(gui_rect_t container, i32 w, i32 h, i32 align_x,
                             i32 align_y, axui_edge_t margin)
{
    gui_rect_t inner = axui_rect_inset(container, margin);
    i32 x = inner.x;
    i32 y = inner.y;

    switch (align_x) {
    case 1:
        x = inner.x + (inner.w - w) / 2;
        break;
    case 2:
        x = inner.x + inner.w - w;
        break;
    default:
        break;
    }
    switch (align_y) {
    case 1:
        y = inner.y + (inner.h - h) / 2;
        break;
    case 2:
        y = inner.y + inner.h - h;
        break;
    default:
        break;
    }

    /* Never let an oversized child start left of the container. */
    if (x < inner.x) {
        x = inner.x;
    }
    if (y < inner.y) {
        y = inner.y;
    }
    return gui_rect(x, y, w, h);
}

/* Centre a box of w x h in a container. */
gui_rect_t axui_layout_center(gui_rect_t container, i32 w, i32 h)
{
    return axui_layout_align(container, w, h, 1, 1, axui_edge(0));
}

/* Stretch a child to the whole container minus a margin. */
gui_rect_t axui_layout_stretch(gui_rect_t container, axui_edge_t margin)
{
    return axui_rect_inset(container, margin);
}

/* ------------------------------------------------------------------ */
/* Grid                                                                */
/* ------------------------------------------------------------------ */

axui_grid_t axui_grid_make(gui_rect_t area, i32 columns, i32 rows, i32 gap)
{
    axui_grid_t grid;
    grid.area = area;
    grid.columns = columns > 0 ? columns : 1;
    grid.rows = rows > 0 ? rows : 1;
    grid.gap = gap;
    grid.padding = axui_edge(0);
    return grid;
}

/* Cell rectangle for a zero-based position.  The last column and row absorb
 * the remainder so the grid always exactly fills its area. */
gui_rect_t axui_grid_cell(const axui_grid_t *grid, i32 column, i32 row)
{
    if (!grid) {
        return gui_rect(0, 0, 0, 0);
    }
    gui_rect_t inner = axui_rect_inset(grid->area, grid->padding);
    i32 usable_w = inner.w - axui_layout_gap_total(grid->columns, grid->gap);
    i32 usable_h = inner.h - axui_layout_gap_total(grid->rows, grid->gap);
    if (usable_w < 0) {
        usable_w = 0;
    }
    if (usable_h < 0) {
        usable_h = 0;
    }

    i32 w = usable_w / grid->columns;
    i32 h = usable_h / grid->rows;
    i32 x = inner.x + column * (w + grid->gap);
    i32 y = inner.y + row * (h + grid->gap);

    if (column == grid->columns - 1) {
        w = inner.x + inner.w - x;
    }
    if (row == grid->rows - 1) {
        h = inner.y + inner.h - y;
    }
    return gui_rect(x, y, w, h);
}

bool_t axui_grid_position(const axui_grid_t *grid, i32 index, i32 *out_column,
                          i32 *out_row)
{
    if (!grid || index < 0) {
        return (bool_t)0;
    }
    i32 total = grid->columns * grid->rows;
    if (index >= total) {
        return (bool_t)0;
    }
    if (out_column) {
        *out_column = index % grid->columns;
    }
    if (out_row) {
        *out_row = index / grid->columns;
    }
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Split panes                                                         */
/* ------------------------------------------------------------------ */

axui_panes_t axui_panes_make(gui_rect_t area, i32 count, bool_t vertical, i32 gap)
{
    axui_panes_t panes;
    panes.area = area;
    panes.count = count > 0 ? count : 1;
    panes.vertical = vertical;
    panes.gap = gap;
    return panes;
}

/* Equal-weight split: the leftover pixels go to the last pane. */
gui_rect_t axui_panes_rect(const axui_panes_t *panes, i32 index)
{
    if (!panes || index < 0 || index >= panes->count) {
        return gui_rect(0, 0, 0, 0);
    }
    i32 total_gap = axui_layout_gap_total(panes->count, panes->gap);
    if (panes->vertical) {
        i32 h = (panes->area.h - total_gap) / panes->count;
        i32 y = panes->area.y + index * (h + panes->gap);
        if (index == panes->count - 1) {
            h = panes->area.y + panes->area.h - y;
        }
        return gui_rect(panes->area.x, y, panes->area.w, h);
    }
    i32 w = (panes->area.w - total_gap) / panes->count;
    i32 x = panes->area.x + index * (w + panes->gap);
    if (index == panes->count - 1) {
        w = panes->area.x + panes->area.w - x;
    }
    return gui_rect(x, panes->area.y, w, panes->area.h);
}

/* Weighted split for a sidebar layout: sizes are percentages that must sum to
 * 100; anything left over is added to the final pane. */
gui_rect_t axui_panes_weighted(const axui_panes_t *panes, i32 index, const i32 *weights,
                               i32 weight_count)
{
    if (!panes || !weights || index < 0 || index >= panes->count ||
        index >= weight_count) {
        return gui_rect(0, 0, 0, 0);
    }
    i32 offset = 0;
    for (i32 i = 0; i < index; i++) {
        offset += weights[i];
    }
    i32 extent = panes->vertical ? panes->area.h : panes->area.w;
    i32 start = (extent * offset) / 100;
    i32 size = (extent * weights[index]) / 100;
    if (panes->vertical) {
        return gui_rect(panes->area.x, panes->area.y + start, panes->area.w, size);
    }
    return gui_rect(panes->area.x + start, panes->area.y, size, panes->area.h);
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

axui_viewport_t axui_viewport_make(gui_rect_t visible, i32 content_w, i32 content_h)
{
    axui_viewport_t viewport;
    viewport.visible = visible;
    viewport.content_w = content_w;
    viewport.content_h = content_h;
    viewport.offset_x = 0;
    viewport.offset_y = 0;
    return viewport;
}

i32 axui_viewport_max_offset_y(const axui_viewport_t *viewport)
{
    if (!viewport) {
        return 0;
    }
    i32 delta = viewport->content_h - viewport->visible.h;
    return delta > 0 ? delta : 0;
}

i32 axui_viewport_max_offset_x(const axui_viewport_t *viewport)
{
    if (!viewport) {
        return 0;
    }
    i32 delta = viewport->content_w - viewport->visible.w;
    return delta > 0 ? delta : 0;
}

/* Clamp the offsets, then translate a content rectangle into screen space,
 * clipped to the visible area.  Returns an empty rect when the item is fully
 * outside, which lets list painters skip rows cheaply. */
gui_rect_t axui_viewport_rect(const axui_viewport_t *viewport, gui_rect_t content)
{
    if (!viewport) {
        return gui_rect(0, 0, 0, 0);
    }
    gui_rect_t moved = gui_rect(content.x - viewport->offset_x,
                                content.y - viewport->offset_y, content.w,
                                content.h);
    return gui_rect_intersect(moved, viewport->visible);
}

/* Scroll by a delta, clamped to both ends.  Returns 1 when the offset
 * actually changed so the caller can skip repainting. */
i32 axui_viewport_scroll(axui_viewport_t *viewport, i32 dx, i32 dy)
{
    if (!viewport) {
        return 0;
    }
    i32 old_x = viewport->offset_x;
    i32 old_y = viewport->offset_y;

    viewport->offset_x += dx;
    viewport->offset_y += dy;

    i32 max_x = axui_viewport_max_offset_x(viewport);
    i32 max_y = axui_viewport_max_offset_y(viewport);
    viewport->offset_x = axui_clamp_i32(viewport->offset_x, 0, max_x);
    viewport->offset_y = axui_clamp_i32(viewport->offset_y, 0, max_y);

    return (viewport->offset_x != old_x || viewport->offset_y != old_y) ? 1 : 0;
}

/* Make a row visible, keeping one row of context above and below. */
i32 axui_viewport_scroll_to(axui_viewport_t *viewport, i32 y, i32 row_height)
{
    if (!viewport) {
        return 0;
    }
    i32 top = viewport->offset_y;
    i32 bottom = top + viewport->visible.h;
    i32 changed = 0;

    if (y < top + row_height) {
        changed = axui_viewport_scroll(viewport, 0, y - row_height - top);
    } else if (y + row_height > bottom - row_height) {
        changed = axui_viewport_scroll(viewport, 0, y + row_height * 2 - bottom);
    }
    return changed;
}

/* Fraction of the content that is scrolled, in percent; the scrollbar uses
 * this directly and the list keeps its thumb in sync. */
i32 axui_viewport_percent(const axui_viewport_t *viewport)
{
    i32 max = axui_viewport_max_offset_y(viewport);
    if (max <= 0) {
        return 0;
    }
    return (viewport->offset_y * 100) / max;
}

/* ------------------------------------------------------------------ */
/* Aspect ratio and flow                                               */
/* ------------------------------------------------------------------ */

/* Fit a w:h box inside an area preserving the ratio. */
gui_rect_t axui_rect_fit_aspect(gui_rect_t area, i32 ratio_w, i32 ratio_h)
{
    if (ratio_w <= 0 || ratio_h <= 0) {
        return area;
    }
    i32 scaled_w = area.h * ratio_w / ratio_h;
    i32 scaled_h = area.w * ratio_h / ratio_w;
    if (scaled_w <= area.w) {
        return axui_layout_center(area, scaled_w, area.h);
    }
    return axui_layout_center(area, area.w, scaled_h);
}

/* A simple row-flow: items of varying width placed left to right and wrapped
 * at the container edge.  Returns the y of the last row's bottom. */
i32 axui_flow_row(gui_rect_t area, const i32 *widths, i32 count, i32 item_h,
                  i32 gap, gui_rect_t *out)
{
    if (!widths || !out || count <= 0) {
        return area.y;
    }
    i32 pen_x = area.x;
    i32 pen_y = area.y;
    i32 right = area.x + area.w;

    for (i32 i = 0; i < count; i++) {
        i32 w = widths[i];
        if (pen_x + w > right && pen_x > area.x) {
            pen_x = area.x;
            pen_y += item_h + gap;
        }
        out[i] = gui_rect(pen_x, pen_y, w, item_h);
        pen_x += w + gap;
    }
    return pen_y + item_h;
}

/* Full-width rows: used by every settings page. */
i32 axui_stack_rows(gui_rect_t area, i32 row_height, i32 gap, i32 count,
                    gui_rect_t *out)
{
    if (!out || count <= 0) {
        return area.y;
    }
    i32 y = area.y;
    for (i32 i = 0; i < count; i++) {
        out[i] = gui_rect(area.x, y, area.w, row_height);
        y += row_height + gap;
    }
    return y;
}

/* Sidebar + content split, the shell's standard two-pane frame.  The sidebar
 * keeps its requested width; the content pane absorbs whatever is left, and
 * collapses rather than going negative on a narrow window. */
void axui_dock_two(gui_rect_t area, i32 sidebar_w, i32 gap, gui_rect_t *sidebar,
                   gui_rect_t *content)
{
    if (sidebar_w < 0) {
        sidebar_w = 0;
    }
    if (sidebar_w > area.w) {
        sidebar_w = area.w;
    }
    i32 remaining = area.w - sidebar_w - gap;
    if (remaining < 0) {
        remaining = 0;
    }
    if (sidebar) {
        *sidebar = gui_rect(area.x, area.y, sidebar_w, area.h);
    }
    if (content) {
        *content = gui_rect(area.x + sidebar_w + gap, area.y, remaining, area.h);
    }
}