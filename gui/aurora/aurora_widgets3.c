/* AxiomOS Aurora - extended widget set (third batch).
 *
 * These widgets are the ones that appear inside complex panes rather than on the
 * shell itself: segmented controls, chips, tree views, split panes, breadcrumbs,
 * stat tiles, sparkline graphs and inline validation banners.  Every widget is
 * stateless in painting terms; interaction state lives in the caller.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include "gui.h"

#define AXW3_MAX_SEGMENTS     5
#define AXW3_MAX_CHIPS        8
#define AXW3_MAX_TREE_ROWS    12
#define AXW3_MAX_SPARK        24
#define AXW3_LABEL_LEN        32
#define AXW3_STACK_DEPTH      6

#define AXW3_COLOUR_SURFACE   GUI_RGB(0x1B, 0x1F, 0x27)
#define AXW3_COLOUR_SUNKEN    GUI_RGB(0x14, 0x17, 0x1D)
#define AXW3_COLOUR_RAISED    GUI_RGB(0x23, 0x28, 0x33)
#define AXW3_COLOUR_TEXT      GUI_RGB(0xE8, 0xEA, 0xF0)
#define AXW3_COLOUR_MUTED     GUI_RGB(0x8A, 0x92, 0xA1)
#define AXW3_COLOUR_ACCENT    GUI_RGB(0x5C, 0x8D, 0xF5)
#define AXW3_COLOUR_OK        GUI_RGB(0x51, 0xB9, 0x7A)
#define AXW3_COLOUR_WARN      GUI_RGB(0xF0, 0xB4, 0x4A)
#define AXW3_COLOUR_ERROR     GUI_RGB(0xE8, 0x6A, 0x6A)
#define AXW3_COLOUR_RULE      GUI_RGB(0x2A, 0x2F, 0x3A)

typedef struct axw3_segment
{
    char label[AXW3_LABEL_LEN];
    u32  weight;
    bool_t enabled;
} axw3_segment_t;

typedef struct axw3_chip
{
    char label[AXW3_LABEL_LEN];
    u32  colour;
    bool_t removable;
} axw3_chip_t;

typedef struct axw3_tree_row
{
    char label[AXW3_LABEL_LEN];
    u32  depth;
    u32  children;
    bool_t expanded;
    bool_t selected;
    bool_t folder;
} axw3_tree_row_t;

typedef struct axw3_sparkline
{
    u32 values[AXW3_MAX_SPARK];
    u32 count;
    u32 head;
    u32 minimum;
    u32 maximum;
    u32 colour;
} axw3_sparkline_t;

typedef struct axw3_state
{
    axw3_segment_t segments[AXW3_MAX_SEGMENTS];
    axw3_chip_t chips[AXW3_MAX_CHIPS];
    axw3_tree_row_t tree[AXW3_MAX_TREE_ROWS];
    axw3_sparkline_t spark;
    char breadcrumb[AXW3_STACK_DEPTH][AXW3_LABEL_LEN];
    u32  breadcrumb_count;
    u32  segment_count;
    u32  chip_count;
    u32  tree_count;
    u32  active_segment;
    u32  rows_painted;
    u32  banners;
    u32  split_ratio;
    u32  tree_scroll;
    bool_t ready;
} axw3_state_t;

static axw3_state_t axw3_state;

static void axw3_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axw3_copy_string(char *dst, const char *src, u32 limit)
{
    u32 i = 0;
    if (dst == NULL_PTR || limit == 0)
        return;
    while (src != NULL_PTR && src[i] != 0 && i + 1 < limit)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* -------------------------------------------------------------- public API */

void axw3_init(void)
{
    axw3_zero(&axw3_state, (u32)sizeof(axw3_state));
    axw3_state.split_ratio = 500;
    axw3_state.spark.colour = AXW3_COLOUR_ACCENT;
    axw3_state.ready = true;
}

bool_t axw3_add_segment(const char *label, u32 weight)
{
    axw3_segment_t *segment;
    if (axw3_state.segment_count >= AXW3_MAX_SEGMENTS || label == NULL_PTR)
        return false;
    segment = &axw3_state.segments[axw3_state.segment_count];
    axw3_copy_string(segment->label, label, AXW3_LABEL_LEN);
    segment->weight = weight == 0 ? 1 : weight;
    segment->enabled = true;
    axw3_state.segment_count++;
    return true;
}

u32 axw3_active_segment(void)
{
    return axw3_state.active_segment;
}

bool_t axw3_select_segment(u32 index)
{
    if (index >= axw3_state.segment_count)
        return false;
    if (!axw3_state.segments[index].enabled)
        return false;
    axw3_state.active_segment = index;
    return true;
}

bool_t axw3_add_chip(const char *label, u32 colour)
{
    axw3_chip_t *chip;
    if (axw3_state.chip_count >= AXW3_MAX_CHIPS || label == NULL_PTR)
        return false;
    chip = &axw3_state.chips[axw3_state.chip_count];
    axw3_copy_string(chip->label, label, AXW3_LABEL_LEN);
    chip->colour = colour == 0 ? AXW3_COLOUR_ACCENT : colour;
    chip->removable = true;
    axw3_state.chip_count++;
    return true;
}

bool_t axw3_remove_chip(u32 index)
{
    u32 i;
    if (index >= axw3_state.chip_count)
        return false;
    for (i = index; i + 1 < axw3_state.chip_count; i++)
        axw3_state.chips[i] = axw3_state.chips[i + 1];
    axw3_state.chip_count--;
    return true;
}

bool_t axw3_add_tree_row(const char *label, u32 depth, bool_t folder)
{
    axw3_tree_row_t *row;
    if (axw3_state.tree_count >= AXW3_MAX_TREE_ROWS || label == NULL_PTR)
        return false;
    if (depth >= AXW3_STACK_DEPTH)
        return false;
    row = &axw3_state.tree[axw3_state.tree_count];
    axw3_copy_string(row->label, label, AXW3_LABEL_LEN);
    row->depth = depth;
    row->folder = folder;
    axw3_state.tree_count++;
    return true;
}

bool_t axw3_toggle_tree_row(u32 index)
{
    if (index >= axw3_state.tree_count)
        return false;
    axw3_state.tree[index].expanded = !axw3_state.tree[index].expanded;
    return axw3_state.tree[index].expanded;
}

bool_t axw3_select_tree_row(u32 index)
{
    u32 i;
    if (index >= axw3_state.tree_count)
        return false;
    for (i = 0; i < axw3_state.tree_count; i++)
        axw3_state.tree[i].selected = (i == index);
    return true;
}

u32 axw3_selected_row(void)
{
    u32 i;
    for (i = 0; i < axw3_state.tree_count; i++)
    {
        if (axw3_state.tree[i].selected)
            return i;
    }
    return 0xFFFFFFFFu;
}

bool_t axw3_push_breadcrumb(const char *label)
{
    if (axw3_state.breadcrumb_count >= AXW3_STACK_DEPTH || label == NULL_PTR)
        return false;
    axw3_copy_string(axw3_state.breadcrumb[axw3_state.breadcrumb_count], label,
                     AXW3_LABEL_LEN);
    axw3_state.breadcrumb_count++;
    return true;
}

bool_t axw3_pop_breadcrumb(void)
{
    if (axw3_state.breadcrumb_count == 0)
        return false;
    axw3_state.breadcrumb_count--;
    return true;
}

u32 axw3_breadcrumb_depth(void)
{
    return axw3_state.breadcrumb_count;
}

void axw3_spark_push(u32 value)
{
    axw3_state.spark.values[axw3_state.spark.head] = value;
    axw3_state.spark.head = (axw3_state.spark.head + 1) % AXW3_MAX_SPARK;
    if (axw3_state.spark.count < AXW3_MAX_SPARK)
        axw3_state.spark.count++;
    if (axw3_state.spark.count == 1 || value < axw3_state.spark.minimum)
        axw3_state.spark.minimum = value;
    if (value > axw3_state.spark.maximum)
        axw3_state.spark.maximum = value;
}

u32 axw3_spark_min(void)
{
    return axw3_state.spark.minimum;
}

u32 axw3_spark_max(void)
{
    return axw3_state.spark.maximum;
}

bool_t axw3_set_split(u32 ratio)
{
    if (ratio < 200 || ratio > 800)
        return false;
    axw3_state.split_ratio = ratio;
    return true;
}

/* ----------------------------------------------------------------- painting */

void axw3_paint_segmented(gui_surface_t *surface, gui_rect_t rect)
{
    u32 i;
    u32 total_weight = 0;
    i32 x;
    if (rect.w <= 0 || rect.h <= 0)
        return;
    for (i = 0; i < axw3_state.segment_count; i++)
        total_weight += axw3_state.segments[i].weight;
    if (total_weight == 0)
        return;
    gui_fill_round_rect(surface, rect, rect.h / 2, AXW3_COLOUR_SUNKEN);
    x = rect.x + 3;
    for (i = 0; i < axw3_state.segment_count; i++)
    {
        i32 width = (i32)(((u32)(rect.w - 6) * axw3_state.segments[i].weight) / total_weight);
        gui_rect_t cell = gui_rect(x, rect.y + 3, width, rect.h - 6);
        u32 colour = (i == axw3_state.active_segment) ? AXW3_COLOUR_ACCENT : AXW3_COLOUR_RAISED;
        gui_fill_round_rect(surface, cell, (rect.h - 6) / 2, colour);
        gui_text(surface, cell.x + 10, cell.y + (cell.h - 16) / 2, axw3_state.segments[i].label,
                 i == axw3_state.active_segment ? GUI_RGB(0xFF, 0xFF, 0xFF) : AXW3_COLOUR_TEXT, 1);
        x += width;
    }
}

void axw3_paint_chips(gui_surface_t *surface, gui_rect_t rect)
{
    u32 i;
    i32 x = rect.x;
    i32 y = rect.y;
    for (i = 0; i < axw3_state.chip_count; i++)
    {
        i32 width = 16;
        u32 length = 0;
        while (axw3_state.chips[i].label[length] != 0 && length < AXW3_LABEL_LEN)
            length++;
        width += (i32)length * 8;
        if (x + width > rect.x + rect.w)
        {
            x = rect.x;
            y += 26;
        }
        if (y + 22 > rect.y + rect.h)
            break;
        {
            gui_rect_t chip = gui_rect(x, y, width, 22);
            gui_fill_round_rect(surface, chip, 11, AXW3_COLOUR_RAISED);
            gui_fill_round_rect(surface, gui_rect(chip.x + 6, chip.y + 9, 4, 4), 2,
                                axw3_state.chips[i].colour);
            gui_text(surface, chip.x + 16, chip.y + 4, axw3_state.chips[i].label,
                     AXW3_COLOUR_TEXT, 1);
        }
        x += width + 6;
    }
}

void axw3_paint_tree(gui_surface_t *surface, gui_rect_t rect)
{
    u32 i;
    u32 visible = (u32)((rect.h - 8) / 22);
    gui_fill_round_rect(surface, rect, 6, AXW3_COLOUR_SUNKEN);
    if (visible == 0)
        return;
    for (i = axw3_state.tree_scroll; i < axw3_state.tree_count && i < axw3_state.tree_scroll + visible; i++)
    {
        const axw3_tree_row_t *row = &axw3_state.tree[i];
        i32 y = rect.y + 6 + (i32)(i - axw3_state.tree_scroll) * 22;
        i32 indent = 8 + (i32)row->depth * 14;
        if (row->selected)
            gui_fill_rect(surface, gui_rect(rect.x + 3, y - 2, rect.w - 6, 20),
                          GUI_RGB(0x22, 0x2C, 0x44));
        if (row->folder)
        {
            gui_fill_rect(surface, gui_rect(rect.x + indent, y + 4, 8, 8), AXW3_COLOUR_ACCENT);
            gui_fill_rect(surface, gui_rect(rect.x + indent + 8, y + 2, 8, 4), AXW3_COLOUR_ACCENT);
        }
        gui_text(surface, rect.x + indent + 20, y + 3, row->label,
                 row->selected ? AXW3_COLOUR_ACCENT : AXW3_COLOUR_TEXT, 1);
        if (row->children != 0)
            gui_text(surface, rect.x + rect.w - 24, y + 3, row->expanded ? "-" : "+",
                     AXW3_COLOUR_MUTED, 1);
    }
    axw3_state.rows_painted += visible;
}

void axw3_paint_sparkline(gui_surface_t *surface, gui_rect_t rect)
{
    u32 i;
    u32 span;
    i32 step;
    if (axw3_state.spark.count < 2 || rect.w <= 0 || rect.h <= 0)
        return;
    span = axw3_state.spark.maximum - axw3_state.spark.minimum;
    if (span == 0)
        span = 1;
    step = rect.w / (i32)axw3_state.spark.count;
    gui_fill_round_rect(surface, rect, 4, AXW3_COLOUR_SUNKEN);
    for (i = 0; i < axw3_state.spark.count; i++)
    {
        u32 index = (axw3_state.spark.head + AXW3_MAX_SPARK - axw3_state.spark.count + i) % AXW3_MAX_SPARK;
        u32 value = axw3_state.spark.values[index];
        i32 height = (i32)(((value - axw3_state.spark.minimum) * (u32)(rect.h - 6)) / span);
        i32 x = rect.x + 3 + (i32)i * step;
        gui_fill_rect(surface, gui_rect(x, rect.y + rect.h - 3 - height, step > 2 ? step - 2 : 1,
                                        height),
                      axw3_state.spark.colour);
    }
}

void axw3_paint_breadcrumbs(gui_surface_t *surface, gui_rect_t rect)
{
    u32 i;
    i32 x = rect.x;
    for (i = 0; i < axw3_state.breadcrumb_count; i++)
    {
        u32 length = 0;
        u32 colour = (i + 1 == axw3_state.breadcrumb_count) ? AXW3_COLOUR_ACCENT : AXW3_COLOUR_MUTED;
        while (axw3_state.breadcrumb[i][length] != 0 && length < AXW3_LABEL_LEN)
            length++;
        gui_text(surface, x, rect.y + 4, axw3_state.breadcrumb[i], colour, 1);
        x += (i32)length * 8 + 8;
        if (i + 1 < axw3_state.breadcrumb_count)
        {
            gui_text(surface, x, rect.y + 4, "/", AXW3_COLOUR_RULE, 1);
            x += 8;
        }
        if (x > rect.x + rect.w)
            break;
    }
}

void axw3_paint_stat(gui_surface_t *surface, gui_rect_t rect, const char *label,
                    const char *value, u32 accent)
{
    gui_fill_round_rect(surface, rect, 8, AXW3_COLOUR_SURFACE);
    gui_fill_round_rect(surface, gui_rect(rect.x + 10, rect.y + 10, 3, rect.h - 20), 1, accent);
    gui_text(surface, rect.x + 22, rect.y + 10, label, AXW3_COLOUR_MUTED, 1);
    gui_text(surface, rect.x + 22, rect.y + 30, value, AXW3_COLOUR_TEXT, 2);
}

void axw3_paint_banner(gui_surface_t *surface, gui_rect_t rect, u32 severity,
                       const char *text)
{
    u32 background;
    u32 foreground;
    switch (severity)
    {
    case 1:
        background = GUI_RGB(0x1E, 0x2E, 0x22);
        foreground = AXW3_COLOUR_OK;
        break;
    case 2:
        background = GUI_RGB(0x33, 0x2A, 0x18);
        foreground = AXW3_COLOUR_WARN;
        break;
    case 3:
        background = GUI_RGB(0x33, 0x1D, 0x1D);
        foreground = AXW3_COLOUR_ERROR;
        break;
    default:
        background = GUI_RGB(0x1A, 0x23, 0x36);
        foreground = AXW3_COLOUR_ACCENT;
        break;
    }
    gui_fill_round_rect(surface, rect, 6, background);
    gui_fill_round_rect(surface, gui_rect(rect.x + 8, rect.y + 8, 16, 16), 8, foreground);
    gui_text(surface, rect.x + 32, rect.y + 8, text, foreground, 1);
    axw3_state.banners++;
}

void axw3_paint_split(gui_surface_t *surface, gui_rect_t rect, bool_t horizontal)
{
    i32 divider;
    if (horizontal)
    {
        divider = rect.x + (rect.w * (i32)axw3_state.split_ratio) / 1000;
        gui_fill_round_rect(surface, gui_rect(rect.x, rect.y, rect.w, rect.h), 8,
                            AXW3_COLOUR_SURFACE);
        gui_fill_rect(surface, gui_rect(divider - 2, rect.y + 6, 4, rect.h - 12),
                      AXW3_COLOUR_RULE);
        gui_fill_round_rect(surface, gui_rect(divider - 6, rect.y + rect.h / 2 - 10, 12, 20), 6,
                            AXW3_COLOUR_RAISED);
    }
    else
    {
        divider = rect.y + (rect.h * (i32)axw3_state.split_ratio) / 1000;
        gui_fill_round_rect(surface, gui_rect(rect.x, rect.y, rect.w, rect.h), 8,
                            AXW3_COLOUR_SURFACE);
        gui_fill_rect(surface, gui_rect(rect.x + 6, divider - 2, rect.w - 12, 4),
                      AXW3_COLOUR_RULE);
    }
}

void axw3_report(void)
{
    printk("axw3: segments=%u chips=%u tree=%u crumb=%u spark=%u banners=%u split=%u\n",
           (unsigned)axw3_state.segment_count, (unsigned)axw3_state.chip_count,
           (unsigned)axw3_state.tree_count, (unsigned)axw3_state.breadcrumb_count,
           (unsigned)axw3_state.spark.count, (unsigned)axw3_state.banners,
           (unsigned)axw3_state.split_ratio);
}

bool_t axw3_selftest(void)
{
    axw3_init();
    if (!axw3_add_segment("All", 1))
        return false;
    if (!axw3_add_segment("Open", 1))
        return false;
    if (!axw3_add_segment("Closed", 2))
        return false;
    if (!axw3_select_segment(1))
        return false;
    if (axw3_active_segment() != 1)
        return false;
    if (axw3_select_segment(9))
        return false;
    if (!axw3_add_chip("kernel", 0))
        return false;
    if (!axw3_add_chip("driver", 0))
        return false;
    if (!axw3_remove_chip(0))
        return false;
    if (axw3_state.chip_count != 1)
        return false;
    if (!axw3_add_tree_row("src", 0, true))
        return false;
    if (!axw3_add_tree_row("main.c", 1, false))
        return false;
    if (axw3_add_tree_row("too/deep", 9, false))
        return false;
    if (!axw3_select_tree_row(1))
        return false;
    if (axw3_selected_row() != 1)
        return false;
    if (!axw3_toggle_tree_row(0))
        return false;
    if (!axw3_push_breadcrumb("home"))
        return false;
    if (!axw3_push_breadcrumb("docs"))
        return false;
    if (axw3_breadcrumb_depth() != 2)
        return false;
    if (!axw3_pop_breadcrumb())
        return false;
    axw3_spark_push(10);
    axw3_spark_push(40);
    axw3_spark_push(25);
    if (axw3_spark_min() != 10 || axw3_spark_max() != 40)
        return false;
    if (!axw3_set_split(600))
        return false;
    if (axw3_set_split(900))
        return false;
    return true;
}
