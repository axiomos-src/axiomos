/*
 * AxiomOS - System monitor page.
 *
 * A live dashboard over samples supplied by the kernel: CPU load per core,
 * memory pressure, disk and network throughput.  The page keeps a rolling
 * ring of samples per series and draws sparkline bands, plus a process table
 * built on the widget table.
 *
 * The kernel hands in values; this file decides how they look.  Nothing here
 * reads hardware directly, which keeps the page testable against synthetic
 * data (`monitor_seed_demo`).
 */

#include <axiom/lib.h>
#include <gui.h>
#include "ui.h"
#include "widgets.h"
#include "monitor_view.h"

/* ------------------------------------------------------------------ */
/* Series                                                              */
/* ------------------------------------------------------------------ */

#define MON_SERIES_MAX 4
#define MON_SAMPLES 96

/* Push a 0..100 sample, keeping one slot per pixel column. */
static void series_push(mon_series_t *series, i32 value)
{
    if (!series) {
        return;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > 100) {
        value = 100;
    }
    series->samples[series->head] = (char)value;
    series->head = (series->head + 1) % MON_SAMPLES;
    if (series->count < MON_SAMPLES) {
        series->count++;
    }
    series->current = value;
    if (value > series->peak) {
        series->peak = value;
    }
}

static i32 series_at(const mon_series_t *series, i32 index)
{
    if (!series || index < 0 || index >= series->count) {
        return 0;
    }
    i32 slot = (series->head - series->count + index + MON_SAMPLES * 2) % MON_SAMPLES;
    return series->samples[slot];
}

/* Average over the visible window, used for the headline number. */
static i32 series_average(const mon_series_t *series)
{
    if (!series || series->count == 0) {
        return 0;
    }
    i32 sum = 0;
    for (i32 i = 0; i < series->count; i++) {
        sum += series_at(series, i);
    }
    return sum / series->count;
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

void monitor_init(monitor_view_t *view, gui_rect_t bounds)
{
    if (!view) {
        return;
    }
    ax_memset(view, 0, sizeof(*view));
    view->bounds = bounds;

    /* Four stat cards along the top. */
    i32 gap = 12;
    i32 card_w = (bounds.w - 28 * 2 - gap * 3) / 4;
    if (card_w < 120) {
        card_w = 120;
    }
    for (i32 i = 0; i < MON_SERIES_MAX; i++) {
        view->cards[i] = gui_rect(bounds.x + 28 + i * (card_w + gap), bounds.y + 16,
                                  card_w, 92);
    }

    view->cpu.name = "cpu";
    view->memory.name = "memory";
    view->disk.name = "disk";
    view->network.name = "net";

    view->graph = gui_rect(bounds.x + 28, bounds.y + 124, bounds.w - 56, 150);
    axw_table_init(&view->processes,
                   gui_rect(bounds.x + 28, bounds.y + 296, bounds.w - 56,
                            bounds.y + bounds.h - 28 - (bounds.y + 296)),
                   4, 22);
    axw_table_header(&view->processes, 0, "pid", 60);
    axw_table_header(&view->processes, 1, "name", 160);
    axw_table_header(&view->processes, 2, "state", 100);
    axw_table_header(&view->processes, 3, "cpu %", 80);

    view->history = (bool_t)1;
    view->graph_mode = 0;
}

/* ------------------------------------------------------------------ */
/* Data ingestion                                                      */
/* ------------------------------------------------------------------ */

void monitor_sample(monitor_view_t *view, i32 cpu, i32 memory, i32 disk, i32 network)
{
    if (!view) {
        return;
    }
    series_push(&view->cpu, cpu);
    series_push(&view->memory, memory);
    series_push(&view->disk, disk);
    series_push(&view->network, network);
}

/* Populate a process row; called by the shell when it walks the task list. */
i32 monitor_add_process(monitor_view_t *view, i32 row, i32 pid, const char *name,
                        const char *state, i32 cpu)
{
    if (!view || row < 0 || row >= AXW_LIST_MAX) {
        return -1;
    }
    char pid_text[16];
    ax_itoa(pid, pid_text);
    char cpu_text[16];
    ax_itoa(cpu, cpu_text);
    ax_strlcat(cpu_text, "%", sizeof(cpu_text));

    axw_table_cell(&view->processes, row, 0, pid_text);
    axw_table_cell(&view->processes, row, 1, name);
    axw_table_cell(&view->processes, row, 2, state);
    axw_table_cell(&view->processes, row, 3, cpu_text);
    return row;
}

/* Deterministic demo data so the page shows something before the kernel's
 * counters are wired up.  The waveform is a sum of two triangle waves, which
 * looks like load without needing a random number generator. */
void monitor_seed_demo(monitor_view_t *view, i32 samples)
{
    if (!view) {
        return;
    }
    for (i32 i = 0; i < samples; i++) {
        i32 phase1 = (i * 7) % 100;
        i32 phase2 = (i * 13) % 100;
        i32 triangle1 = phase1 < 50 ? phase1 * 2 : (100 - phase1) * 2;
        i32 triangle2 = phase2 < 50 ? phase2 * 2 : (100 - phase2) * 2;
        i32 cpu = 20 + (triangle1 + triangle2) / 4;
        i32 memory = 40 + (triangle2 * 3) / 8;
        i32 disk = triangle1 / 6;
        i32 net = 10 + (triangle1 + triangle2) / 6;
        monitor_sample(view, cpu, memory, disk, net);
    }
}

/* ------------------------------------------------------------------ */
/* Sparkline                                                           */
/* ------------------------------------------------------------------ */

/* Draw one series into a band.  Filled area plus a bright leading edge: the
 * classic monitoring look, drawn with thousands of 1 px spans. */
static void paint_series(gui_surface_t *surface, const mon_series_t *series,
                         gui_rect_t band, u32 ink)
{
    if (!surface || !series) {
        return;
    }
    i32 columns = band.w;
    if (columns <= 1 || series->count < 2) {
        return;
    }
    i32 step = series->count > columns ? series->count / columns : 1;
    if (step < 1) {
        step = 1;
    }

    i32 previous_x = 0;
    i32 previous_y = 0;
    for (i32 column = 0; column < columns; column++) {
        i32 index = column * step;
        if (index >= series->count) {
            break;
        }
        i32 value = series_at(series, index);
        i32 height = (value * (band.h - 2)) / 100;
        i32 x = band.x + column;
        i32 y = band.y + band.h - 2 - height;

        /* Fill from the curve down to the baseline, at 90 alpha. */
        gui_fill_rect(surface, gui_rect(x, y, 1, band.y + band.h - 1 - y),
                      axui_colour_alpha(ink, 90));
        if (column > 0) {
            axui_line_aa(surface, previous_x, previous_y, x, y, ink, 1);
        }
        previous_x = x;
        previous_y = y;
    }

    /* Baseline and maximum gridline for scale. */
    gui_fill_rect(surface, gui_rect(band.x, band.y + band.h - 1, band.w, 1),
                  axui_colour_alpha(ink, 60));
}

/* A stat card: label, big number, unit and a small sparkline in the corner. */
static void paint_card(gui_surface_t *surface, gui_rect_t rect, const char *label,
                       const mon_series_t *series, const char *unit, u32 ink)
{
    const gui_theme_t *theme = axui_theme_current();
    axw_card_paint(surface, rect, 10);

    axui_text_style_t label_style = axui_style(theme->text_dim, 1);
    axui_draw_text(surface, rect.x + 14, rect.y + 16, label, label_style);

    char number[16];
    ax_itoa(series->current, number);
    axui_text_style_t number_style = axui_style(ink, 2);
    number_style.bold = (bool_t)1;
    axui_draw_text(surface, rect.x + 14, rect.y + 38, number, number_style);

    i32 number_w = axui_measure_text(number, 2);
    axui_text_style_t unit_style = axui_style(theme->text_dim, 1);
    axui_draw_text(surface, rect.x + 18 + number_w, rect.y + 50, unit, unit_style);

    gui_rect_t spark = gui_rect(rect.x + 14, rect.y + rect.h - 26, rect.w - 28, 16);
    paint_series(surface, series, spark, ink);

    /* Peak, small, in the card's top right. */
    char peak[24];
    ax_snprintf(peak, sizeof(peak), "peak %d", series->peak);
    axui_draw_text_right(surface, gui_rect(rect.x, rect.y + 8, rect.w - 14, 16), peak,
                         axui_style(theme->text_dim, 1));
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

void monitor_paint(gui_surface_t *surface, monitor_view_t *view)
{
    if (!surface || !view) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    gui_fill_rect(surface, view->bounds, theme->window_bg);

    paint_card(surface, view->cards[0], "CPU", &view->cpu, "%", theme->accent);
    paint_card(surface, view->cards[1], "Memory", &view->memory, "%",
               axui_lighten(theme->accent, 25));
    paint_card(surface, view->cards[2], "Disk", &view->disk, "MB/s", theme->warning);
    paint_card(surface, view->cards[3], "Network", &view->network, "Mb/s",
               theme->success);

    /* Combined graph: either all four overlaid or one at full height, chosen
     * by view->graph_mode.  Axis labels are fractions, not values. */
    axw_card_paint(surface, view->graph, 10);
    axui_text_style_t title = axui_style(theme->text, 1);
    title.bold = (bool_t)1;
    axui_draw_text(surface, view->graph.x + 16, view->graph.y + 18,
                   view->graph_mode == 0 ? "combined load" : "single series",
                   title);

    gui_rect_t plot = gui_rect(view->graph.x + 16, view->graph.y + 32,
                               view->graph.w - 32, view->graph.h - 48);
    axui_grid_pattern(surface, plot, 32, axui_colour_alpha(theme->border, 40));

    if (view->graph_mode == 0) {
        paint_series(surface, &view->cpu, plot, theme->accent);
        paint_series(surface, &view->memory, gui_rect(plot.x, plot.y, plot.w,
                                                      plot.h - 6),
                     axui_lighten(theme->accent, 25));
        paint_series(surface, &view->network,
                     gui_rect(plot.x, plot.y + plot.h / 3, plot.w, plot.h * 2 / 3),
                     theme->success);
    } else {
        const mon_series_t *series = &view->cpu;
        u32 ink = theme->accent;
        switch (view->graph_mode) {
        case 2: series = &view->memory; ink = axui_lighten(theme->accent, 25); break;
        case 3: series = &view->disk;   ink = theme->warning; break;
        case 4: series = &view->network; ink = theme->success; break;
        default: break;
        }
        paint_series(surface, series, plot, ink);
    }

    /* Averages underneath the plot. */
    char summary[96];
    ax_snprintf(summary, sizeof(summary), "avg cpu %d%%  mem %d%%  disk %d%%  net %d%%",
                series_average(&view->cpu), series_average(&view->memory),
                series_average(&view->disk), series_average(&view->network));
    axui_draw_text(surface, plot.x, view->graph.y + view->graph.h - 12, summary,
                   axui_style(theme->text_dim, 1));

    axw_table_paint(surface, &view->processes);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

u32 monitor_event(monitor_view_t *view, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!view) {
        return AXW_NONE;
    }
    u32 events = axw_table_event(&view->processes, x, y, buttons, pressed);
    if (events & AXW_SELECT) {
        /* A process row click asks the shell for details, reported through the
         * return value's payload bits. */
        return AXW_SELECT | (u32)(view->processes.selected & 0xFFFF);
    }
    if (pressed && axw_hit(view->graph, x, y)) {
        view->graph_mode = (view->graph_mode + 1) % 5;
        return AXW_CLICK | AXW_CHANGED;
    }
    return events;
}

/* Keyboard: page through the process table. */
u32 monitor_key(monitor_view_t *view, char key, u8 scancode)
{
    if (!view) {
        return AXW_NONE;
    }
    return axw_table_key(&view->processes, scancode);
}
