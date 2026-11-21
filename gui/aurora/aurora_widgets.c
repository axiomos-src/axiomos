/*
 * AxiomOS - Aurora data widgets.
 *
 * Tables, trees, charts and gauges share one idea: the caller owns the data and
 * the widget owns only presentation state.  No widget allocates, and none caches
 * a copy of the rows handed to it, so an application can rebuild its model every
 * frame and still get stable scroll positions and selections.
 *
 * Charts draw from a value array with an explicit scale; they never guess a
 * "nice" axis from the data.  Guessing produces axes that jump between frames
 * when values barely change, which is worse than a caller choosing a range.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AW_TABLE_MAX_COLUMNS 6
#define AW_TABLE_MAX_ROWS    64
#define AW_TREE_MAX_NODES    64

/* ------------------------------------------------------------------ */
/* Table                                                               */
/* ------------------------------------------------------------------ */

typedef enum aw_align {
    AW_ALIGN_LEFT = 0,
    AW_ALIGN_RIGHT,
    AW_ALIGN_CENTER
} aw_align_t;

typedef struct aw_column {
    char       title[24];
    i32        width;        /* 0 means flexible share of the remainder */
    aw_align_t align;
} aw_column_t;

typedef struct aw_row {
    char        cells[AW_TABLE_MAX_COLUMNS][40];
    bool_t      selected;
    const char *glyph;
} aw_row_t;

typedef struct aw_table {
    gui_rect_t  rect;
    aw_column_t columns[AW_TABLE_MAX_COLUMNS];
    i32         column_count;
    aw_row_t    rows[AW_TABLE_MAX_ROWS];
    i32         row_count;
    i32         scroll;
    i32         hover;
    i32         selected;
    i32         header_height;
    i32         row_height;
    bool_t      striped;
    bool_t      compact;
} aw_table_t;

void aw_table_init(aw_table_t *table, gui_rect_t rect)
{
    if (!table) return;
    ax_memset(table, 0, sizeof(*table));
    table->rect = rect;
    table->header_height = 32;
    table->row_height = 30;
    table->striped = (bool_t)1;
    table->hover = -1;
    table->selected = -1;
}

i32 aw_table_add_column(aw_table_t *table, const char *title, i32 width,
                        aw_align_t align)
{
    if (!table || table->column_count >= AW_TABLE_MAX_COLUMNS) return -1;
    aw_column_t *column = &table->columns[table->column_count];
    ax_strlcpy(column->title, title ? title : "", sizeof(column->title));
    column->width = width;
    column->align = align;
    return table->column_count++;
}

i32 aw_table_add_row(aw_table_t *table)
{
    if (!table || table->row_count >= AW_TABLE_MAX_ROWS) return -1;
    aw_row_t *row = &table->rows[table->row_count];
    ax_memset(row, 0, sizeof(*row));
    return table->row_count++;
}

void aw_table_set(aw_table_t *table, i32 row, i32 column, const char *text)
{
    if (!table || row < 0 || row >= table->row_count ||
        column < 0 || column >= table->column_count) {
        return;
    }
    ax_strlcpy(table->rows[row].cells[column], text ? text : "",
               sizeof(table->rows[row].cells[column]));
}

void aw_table_set_glyph(aw_table_t *table, i32 row, const char *glyph)
{
    if (!table || row < 0 || row >= table->row_count) return;
    table->rows[row].glyph = glyph;
}

/* Column widths: fixed columns take their declared width, the remaining space
 * is shared by flexible columns.  The last flexible column absorbs rounding. */
static void aw_table_layout(const aw_table_t *table, i32 *widths)
{
    i32 fixed = 0;
    i32 flexible = 0;
    for (i32 i = 0; i < table->column_count; i++) {
        if (table->columns[i].width > 0) {
            widths[i] = table->columns[i].width;
            fixed += widths[i];
        } else {
            widths[i] = 0;
            flexible++;
        }
    }
    i32 available = table->rect.w - fixed;
    if (available < 0) available = 0;
    if (flexible > 0) {
        i32 each = available / flexible;
        for (i32 i = 0; i < table->column_count; i++) {
            if (widths[i] == 0) widths[i] = each;
        }
    }
}

void aw_table_paint(gui_surface_t *surface, aw_table_t *table,
                    const axau_palette_t *palette)
{
    if (!surface || !table || !palette) return;
    i32 widths[AW_TABLE_MAX_COLUMNS] = {0};
    aw_table_layout(table, widths);

    gui_rect_t header = gui_rect(table->rect.x, table->rect.y, table->rect.w,
                                 table->header_height);
    gui_fill_rect(surface, header,
                  axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    i32 x = table->rect.x + 8;
    for (i32 c = 0; c < table->column_count; c++) {
        axui_draw_text_clip(surface,
                            gui_rect(x, header.y, widths[c] - 8, header.h),
                            x, header.y + (header.h - 16) / 2,
                            table->columns[c].title,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_SECONDARY), 1));
        x += widths[c];
    }
    axau_paint_divider(surface, gui_rect(table->rect.x,
                                         table->rect.y + table->header_height - 1,
                                         table->rect.w, 1), palette);

    i32 visible = (table->rect.h - table->header_height) / table->row_height;
    i32 first = table->scroll;
    i32 painted = 0;
    for (i32 r = first; r < table->row_count && painted < visible; r++, painted++) {
        aw_row_t *row = &table->rows[r];
        i32 y = table->rect.y + table->header_height + painted * table->row_height;
        gui_rect_t band = gui_rect(table->rect.x, y, table->rect.w,
                                   table->row_height);
        if (row->selected || table->selected == r) {
            gui_fill_rect(surface, band,
                          axui_colour_opacity(
                              axau_colour(palette, AXAU_COLOUR_ACCENT), 16));
        } else if (table->striped && (painted % 2)) {
            gui_fill_rect(surface, band,
                          axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
        } else if (table->hover == r) {
            gui_fill_rect(surface, band,
                          axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
        }
        x = table->rect.x + 8;
        for (i32 c = 0; c < table->column_count; c++) {
            i32 text_x = x;
            if (c == 0 && row->glyph) {
                axau_icon_glyph(surface, x + 8, y + table->row_height / 2, 12,
                                row->glyph,
                                axau_colour(palette, AXAU_COLOUR_ACCENT));
                text_x += 22;
            }
            if (table->columns[c].align == AW_ALIGN_RIGHT) {
                axui_draw_text_right(surface,
                                     gui_rect(x, y, widths[c] - 12,
                                              table->row_height),
                                     row->cells[c],
                                     axui_style(axau_colour(palette,
                                                            AXAU_COLOUR_TEXT), 1));
            } else if (table->columns[c].align == AW_ALIGN_CENTER) {
                axui_draw_text_center(surface,
                                      gui_rect(x, y, widths[c], table->row_height),
                                      row->cells[c],
                                      axui_style(axau_colour(palette,
                                                             AXAU_COLOUR_TEXT), 1));
            } else {
                axui_draw_text_clip(surface,
                                    gui_rect(text_x, y, widths[c] - (text_x - x) - 8,
                                             table->row_height),
                                    text_x, y + (table->row_height - 16) / 2,
                                    row->cells[c],
                                    axui_style(axau_colour(palette,
                                                           AXAU_COLOUR_TEXT), 1));
            }
            x += widths[c];
        }
    }
    if (table->row_count > visible && visible > 0) {
        /* Scrollbar: the thumb height is proportional to the visible fraction. */
        i32 track_h = table->rect.h - table->header_height;
        i32 thumb_h = (visible * track_h) / table->row_count;
        if (thumb_h < 24) thumb_h = 24;
        i32 thumb_y = table->rect.y + table->header_height +
                      ((track_h - thumb_h) * table->scroll) /
                          (table->row_count - visible ? table->row_count - visible : 1);
        gui_fill_round_rect(surface,
                            gui_rect(table->rect.x + table->rect.w - 8, thumb_y,
                                     4, thumb_h), 2,
                            axau_colour(palette, AXAU_COLOUR_STROKE_STRONG));
    }
}

i32 aw_table_hit(aw_table_t *table, i32 x, i32 y)
{
    if (!table) return -1;
    if (y < table->rect.y + table->header_height) return -1;
    i32 index = table->scroll + (y - table->rect.y - table->header_height) /
                                 table->row_height;
    if (index < 0 || index >= table->row_count) return -1;
    table->selected = index;
    return index;
}

void aw_table_scroll(aw_table_t *table, i32 delta)
{
    if (!table) return;
    i32 visible = (table->rect.h - table->header_height) / table->row_height;
    table->scroll += delta;
    if (table->scroll < 0) table->scroll = 0;
    if (table->scroll + visible > table->row_count && visible < table->row_count) {
        table->scroll = table->row_count - visible;
    }
}

/* ------------------------------------------------------------------ */
/* Tree                                                                */
/* ------------------------------------------------------------------ */

typedef struct aw_tree_node {
    char     label[48];
    i32      depth;
    bool_t   expanded;
    bool_t   leaf;
    bool_t   selected;
    const char *glyph;
} aw_tree_node_t;

typedef struct aw_tree {
    gui_rect_t    rect;
    aw_tree_node_t nodes[AW_TREE_MAX_NODES];
    i32           node_count;
    i32           row_height;
    i32           scroll;
    i32           selected;
    bool_t        guide_lines;
} aw_tree_t;

void aw_tree_init(aw_tree_t *tree, gui_rect_t rect)
{
    if (!tree) return;
    ax_memset(tree, 0, sizeof(*tree));
    tree->rect = rect;
    tree->row_height = 26;
    tree->selected = -1;
    tree->guide_lines = (bool_t)1;
}

i32 aw_tree_add(aw_tree_t *tree, const char *label, i32 depth, bool_t leaf)
{
    if (!tree || tree->node_count >= AW_TREE_MAX_NODES) return -1;
    aw_tree_node_t *node = &tree->nodes[tree->node_count];
    ax_memset(node, 0, sizeof(*node));
    ax_strlcpy(node->label, label ? label : "", sizeof(node->label));
    node->depth = depth;
    node->leaf = leaf;
    node->expanded = (bool_t)1;
    return tree->node_count++;
}

void aw_tree_paint(gui_surface_t *surface, aw_tree_t *tree,
                   const axau_palette_t *palette)
{
    if (!surface || !tree || !palette) return;
    i32 visible = tree->rect.h / tree->row_height;
    i32 painted = 0;
    for (i32 i = tree->scroll; i < tree->node_count && painted < visible;
         i++, painted++) {
        aw_tree_node_t *node = &tree->nodes[i];
        i32 y = tree->rect.y + painted * tree->row_height;
        if (tree->selected == i) {
            gui_fill_rect(surface,
                          gui_rect(tree->rect.x, y, tree->rect.w, tree->row_height),
                          axui_colour_opacity(
                              axau_colour(palette, AXAU_COLOUR_ACCENT), 16));
        }
        i32 indent = node->depth * 18 + 10;
        if (tree->guide_lines && node->depth > 0) {
            for (i32 d = 1; d <= node->depth; d++) {
                gui_vline(surface, tree->rect.x + d * 18 + 4, y,
                          tree->row_height,
                          axau_colour(palette, AXAU_COLOUR_DIVIDER));
            }
        }
        if (!node->leaf) {
            /* A rotated chevron would need a transform; a small triangle is the
             * same idea in four fill calls. */
            i32 cx = tree->rect.x + indent + 4;
            i32 cy = y + tree->row_height / 2;
            if (node->expanded) {
                axui_fill_triangle(surface, axui_point(cx - 4, cy - 2),
                                   axui_point(cx + 4, cy - 2),
                                   axui_point(cx, cy + 3),
                                   axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY));
            } else {
                axui_fill_triangle(surface, axui_point(cx - 2, cy - 4),
                                   axui_point(cx - 2, cy + 4),
                                   axui_point(cx + 3, cy),
                                   axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY));
            }
        }
        if (node->glyph) {
            axau_icon_glyph(surface, tree->rect.x + indent + 18,
                            y + tree->row_height / 2, 12, node->glyph,
                            axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY));
        }
        axui_draw_text_clip(surface,
                            gui_rect(tree->rect.x + indent + 34, y,
                                     tree->rect.w - indent - 40, tree->row_height),
                            tree->rect.x + indent + 34,
                            y + (tree->row_height - 16) / 2,
                            node->label,
                            axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
    }
}

i32 aw_tree_hit(aw_tree_t *tree, i32 x, i32 y)
{
    if (!tree || !gui_rect_contains(tree->rect, x, y)) return -1;
    i32 index = tree->scroll + (y - tree->rect.y) / tree->row_height;
    if (index < 0 || index >= tree->node_count) return -1;
    tree->selected = index;
    return index;
}

/* ------------------------------------------------------------------ */
/* Charts                                                              */
/* ------------------------------------------------------------------ */

typedef struct aw_series {
    char  label[24];
    u32   colour;
    i32   count;
    i32   values[64];
} aw_series_t;

typedef struct aw_chart {
    gui_rect_t rect;
    aw_series_t series[4];
    i32         series_count;
    i32         minimum;
    i32         maximum;
    char        caption[40];
    bool_t      show_grid;
    bool_t      area_fill;
} aw_chart_t;

void aw_chart_init(aw_chart_t *chart, gui_rect_t rect, i32 minimum, i32 maximum)
{
    if (!chart) return;
    ax_memset(chart, 0, sizeof(*chart));
    chart->rect = rect;
    chart->minimum = minimum;
    chart->maximum = maximum > minimum ? maximum : minimum + 1;
    chart->show_grid = (bool_t)1;
}

i32 aw_chart_add_series(aw_chart_t *chart, const char *label, u32 colour)
{
    if (!chart || chart->series_count >= 4) return -1;
    aw_series_t *series = &chart->series[chart->series_count];
    ax_strlcpy(series->label, label ? label : "", sizeof(series->label));
    series->colour = colour;
    return chart->series_count++;
}

void aw_chart_push(aw_chart_t *chart, i32 series, i32 value)
{
    if (!chart || series < 0 || series >= chart->series_count) return;
    aw_series_t *target = &chart->series[series];
    if (target->count >= 64) {
        for (i32 i = 0; i + 1 < target->count; i++) {
            target->values[i] = target->values[i + 1];
        }
        target->values[target->count - 1] = value;
        return;
    }
    target->values[target->count++] = value;
}

static i32 aw_chart_y(const aw_chart_t *chart, gui_rect_t plot, i32 value)
{
    i32 span = chart->maximum - chart->minimum;
    i32 clamped = value < chart->minimum ? chart->minimum : value;
    if (clamped > chart->maximum) clamped = chart->maximum;
    return plot.y + plot.h - ((clamped - chart->minimum) * plot.h) / span;
}

void aw_chart_paint(gui_surface_t *surface, aw_chart_t *chart,
                    const axau_palette_t *palette)
{
    if (!surface || !chart || !palette) return;
    gui_rect_t plot = gui_rect(chart->rect.x + 40, chart->rect.y + 12,
                               chart->rect.w - 52, chart->rect.h - 40);
    if (plot.w <= 0 || plot.h <= 0) return;
    if (chart->show_grid) {
        for (i32 i = 0; i <= 4; i++) {
            i32 y = plot.y + (plot.h * i) / 4;
            gui_hline(surface, plot.x, y, plot.w,
                      axau_colour(palette, AXAU_COLOUR_DIVIDER));
            /* Axis labels are values at the gridline, not tick indices. */
            i32 value = chart->maximum -
                        ((chart->maximum - chart->minimum) * i) / 4;
            char label[16];
            ax_snprintf(label, sizeof(label), "%d", value);
            axui_draw_text_right(surface,
                                 gui_rect(chart->rect.x, y - 8, 34, 16), label,
                                 axui_style(axau_colour(palette,
                                                        AXAU_COLOUR_TEXT_TERTIARY), 1));
        }
    }
    for (i32 s = 0; s < chart->series_count; s++) {
        aw_series_t *series = &chart->series[s];
        if (series->count < 2) continue;
        i32 previous_x = 0, previous_y = 0;
        for (i32 i = 0; i < series->count; i++) {
            i32 x = plot.x + (i * plot.w) / (series->count - 1);
            i32 y = aw_chart_y(chart, plot, series->values[i]);
            if (chart->area_fill) {
                gui_vline(surface, x, y, plot.y + plot.h - y,
                          axui_colour_opacity(series->colour, 12));
            }
            if (i > 0) {
                axui_line_aa(surface, previous_x, previous_y, x, y, series->colour,
                             2);
            }
            previous_x = x;
            previous_y = y;
        }
    }
    if (chart->caption[0]) {
        axui_draw_text(surface, chart->rect.x + 40,
                       chart->rect.y + chart->rect.h - 20, chart->caption,
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
}

/* ------------------------------------------------------------------ */
/* Gauge, sparkline, bars                                              */
/* ------------------------------------------------------------------ */

typedef struct aw_gauge {
    gui_rect_t rect;
    i32        value;
    i32        maximum;
    char       label[32];
    char       unit[12];
    u32        colour;
    bool_t     show_value;
} aw_gauge_t;

void aw_gauge_init(aw_gauge_t *gauge, gui_rect_t rect, i32 maximum)
{
    if (!gauge) return;
    ax_memset(gauge, 0, sizeof(*gauge));
    gauge->rect = rect;
    gauge->maximum = maximum > 0 ? maximum : 100;
    gauge->show_value = (bool_t)1;
}

void aw_gauge_set(aw_gauge_t *gauge, i32 value)
{
    if (!gauge) return;
    gauge->value = value < 0 ? 0 : (value > gauge->maximum ? gauge->maximum
                                                           : value);
}

void aw_gauge_paint(gui_surface_t *surface, aw_gauge_t *gauge,
                    const axau_palette_t *palette)
{
    if (!surface || !gauge || !palette) return;
    i32 radius = gauge->rect.w < gauge->rect.h ? gauge->rect.w / 2
                                               : gauge->rect.h / 2;
    if (radius < 6) radius = 6;
    i32 cx = gauge->rect.x + gauge->rect.w / 2;
    i32 cy = gauge->rect.y + gauge->rect.h / 2;
    u32 track = axau_colour(palette, AXAU_COLOUR_CONTROL);
    u32 fill = gauge->colour ? gauge->colour
                             : axau_colour(palette, AXAU_COLOUR_ACCENT);
    /* A 270 degree arc, starting at the lower left and ending at the lower right. */
    axui_arc(surface, cx, cy, radius, 135, 270, 6, track);
    i32 sweep = (270 * gauge->value) / gauge->maximum;
    if (sweep > 0) {
        axui_arc(surface, cx, cy, radius, 135, sweep, 6, fill);
    }
    if (gauge->show_value) {
        char text[24];
        ax_snprintf(text, sizeof(text), "%d%s", gauge->value, gauge->unit);
        axui_draw_text_center(surface,
                              gui_rect(cx - radius, cy - 8, radius * 2, 16),
                              text,
                              axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
    }
    if (gauge->label[0]) {
        axui_draw_text_center(surface,
                              gui_rect(cx - radius, cy + 12, radius * 2, 16),
                              gauge->label,
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
}

/* A sparkline is a gauge without a frame: values only, no axis, no labels.  It
 * is deliberately unlabelled because it always sits next to its number. */
void aw_sparkline_paint(gui_surface_t *surface, gui_rect_t rect,
                        const i32 *values, i32 count, u32 colour)
{
    if (!surface || !values || count < 2 || rect.w < 4 || rect.h < 4) return;
    i32 low = values[0];
    i32 high = values[0];
    for (i32 i = 1; i < count; i++) {
        if (values[i] < low) low = values[i];
        if (values[i] > high) high = values[i];
    }
    if (high == low) high = low + 1;
    i32 px = 0, py = 0;
    for (i32 i = 0; i < count; i++) {
        i32 x = rect.x + (i * rect.w) / (count - 1);
        i32 y = rect.y + rect.h - ((values[i] - low) * rect.h) / (high - low);
        if (i > 0) axui_line_aa(surface, px, py, x, y, colour, 2);
        px = x;
        py = y;
    }
}

typedef struct aw_bars {
    gui_rect_t rect;
    i32        values[16];
    i32        count;
    i32        maximum;
    u32        colour;
    bool_t     horizontal;
} aw_bars_t;

void aw_bars_init(aw_bars_t *bars, gui_rect_t rect)
{
    if (!bars) return;
    ax_memset(bars, 0, sizeof(*bars));
    bars->rect = rect;
    bars->maximum = 100;
}

void aw_bars_push(aw_bars_t *bars, i32 value)
{
    if (!bars || bars->count >= 16) return;
    bars->values[bars->count++] = value;
}

void aw_bars_paint(gui_surface_t *surface, aw_bars_t *bars,
                   const axau_palette_t *palette)
{
    if (!surface || !bars || !palette || bars->count <= 0) return;
    u32 colour = bars->colour ? bars->colour
                              : axau_colour(palette, AXAU_COLOUR_ACCENT);
    if (bars->horizontal) {
        i32 row = bars->rect.h / bars->count;
        for (i32 i = 0; i < bars->count; i++) {
            i32 width = (bars->values[i] * bars->rect.w) / bars->maximum;
            if (width > bars->rect.w) width = bars->rect.w;
            gui_fill_round_rect(surface,
                                gui_rect(bars->rect.x, bars->rect.y + i * row,
                                         width, row - 4), 2,
                                axui_colour_opacity(colour, 200));
        }
    } else {
        i32 column = bars->rect.w / bars->count;
        for (i32 i = 0; i < bars->count; i++) {
            i32 height = (bars->values[i] * bars->rect.h) / bars->maximum;
            if (height > bars->rect.h) height = bars->rect.h;
            gui_fill_round_rect(surface,
                                gui_rect(bars->rect.x + i * column,
                                         bars->rect.y + bars->rect.h - height,
                                         column - 4, height), 2,
                                axui_colour_opacity(colour, 200));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Stat card and badge                                                 */
/* ------------------------------------------------------------------ */

typedef struct aw_stat {
    gui_rect_t rect;
    char       label[40];
    char       value[24];
    char       delta[24];
    const char *glyph;
    bool_t     positive;
} aw_stat_t;

void aw_stat_init(aw_stat_t *stat, gui_rect_t rect, const char *label,
                  const char *value)
{
    if (!stat) return;
    ax_memset(stat, 0, sizeof(*stat));
    stat->rect = rect;
    ax_strlcpy(stat->label, label ? label : "", sizeof(stat->label));
    ax_strlcpy(stat->value, value ? value : "", sizeof(stat->value));
}

void aw_stat_paint(gui_surface_t *surface, aw_stat_t *stat,
                   const axau_palette_t *palette)
{
    if (!surface || !stat || !palette) return;
    gui_fill_round_rect(surface, stat->rect, AXAU_RADIUS_CARD,
                        axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
    axui_round_rect_border(surface, stat->rect, AXAU_RADIUS_CARD, 1,
                           axau_colour(palette, AXAU_COLOUR_STROKE));
    if (stat->glyph) {
        axau_icon_glyph(surface, stat->rect.x + 20, stat->rect.y + 22, 14,
                        stat->glyph,
                        axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY));
    }
    axui_draw_text(surface, stat->rect.x + 20, stat->rect.y + 40, stat->label,
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1));
    axui_draw_text(surface, stat->rect.x + 20, stat->rect.y + 58, stat->value,
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 3));
    if (stat->delta[0]) {
        axui_draw_text(surface, stat->rect.x + 20, stat->rect.y + 96, stat->delta,
                       axui_style(stat->positive
                                      ? axau_colour(palette, AXAU_COLOUR_SUCCESS)
                                      : axau_colour(palette, AXAU_COLOUR_ERROR), 1));
    }
}

void aw_badge_paint(gui_surface_t *surface, gui_rect_t anchor, const char *text,
                    i32 count, const axau_palette_t *palette)
{
    if (!surface || !palette) return;
    char label[8];
    if (count > 0) {
        if (count > 99) ax_strlcpy(label, "99+", sizeof(label));
        else ax_snprintf(label, sizeof(label), "%d", count);
    } else {
        ax_strlcpy(label, text ? text : "", sizeof(label));
    }
    if (!label[0]) return;
    i32 width = (i32)ax_strlen(label) * 8 + 10;
    if (width < 16) width = 16;
    gui_rect_t badge = gui_rect(anchor.x + anchor.w - width / 2, anchor.y - 4,
                                width, 16);
    gui_fill_round_rect(surface, badge, 8,
                        axau_colour(palette, AXAU_COLOUR_ACCENT));
    axui_draw_text_center(surface, badge, label,
                          axui_style(axau_colour(palette, AXAU_COLOUR_ACCENT_TEXT), 1));
}

/* ------------------------------------------------------------------ */
/* Empty state                                                         */
/* ------------------------------------------------------------------ */

void aw_empty_state_paint(gui_surface_t *surface, gui_rect_t rect,
                          const char *glyph, const char *title,
                          const char *detail, const axau_palette_t *palette)
{
    if (!surface || !palette) return;
    i32 cx = rect.x + rect.w / 2;
    i32 cy = rect.y + rect.h / 2 - 30;
    axau_icon_glyph(surface, cx, cy, 32, glyph ? glyph : "#",
                    axau_colour(palette, AXAU_COLOUR_TEXT_DISABLED));
    axui_draw_text_center(surface, gui_rect(rect.x, cy + 28, rect.w, 20),
                          title ? title : "Nothing here",
                          axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
    if (detail) {
        axui_draw_text_wrapped(surface,
                               gui_rect(rect.x + rect.w / 4, cy + 52,
                                        rect.w / 2, 40),
                               detail,
                               axui_style(axau_colour(palette,
                                                      AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
}

