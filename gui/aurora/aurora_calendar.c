/*
 * AxiomOS - calendar surface.
 *
 * A month grid and an agenda list over one shared event array.  The grid answers
 * "what does this month look like", the agenda answers "what is next", and both
 * read the same events so a change in one is visible in the other without a
 * synchronisation step.
 *
 * Dates are plain (year, month, day) triples and comparisons are lexicographic
 * on packed day numbers.  No epoch conversion is involved, because the kernel
 * has no agreed epoch and a surface that invents one would disagree with every
 * other surface that invented a different one.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXCA_MAX_EVENTS   32
#define AXCA_DAYS         42        /* six weeks, always */
#define AXCA_CELL_HEIGHT  72
#define AXCA_WEEKDAYS      7

typedef enum axcal_kind {
    AXCA_EVENT_MEETING = 0,
    AXCA_EVENT_TASK,
    AXCA_EVENT_REMINDER,
    AXCA_EVENT_PERSONAL
} axcal_kind_t;

typedef struct axcal_event {
    char        title[48];
    char        place[32];
    i32         year;
    i32         month;
    i32         day;
    i32         minute_of_day;
    i32         duration_minutes;
    axcal_kind_t kind;
    bool_t      done;
} axcal_event_t;

typedef struct axcal_app {
    gui_rect_t  rect;
    axcal_event_t events[AXCA_MAX_EVENTS];
    i32         event_count;
    i32         year;
    i32         month;
    i32         selected_day;
    i32         today_day;
    i32         hover_day;
    i32         first_weekday;   /* 0 = Sunday */
    i32         scroll;
    bool_t      agenda_open;
    gui_rect_t  cells[AXCA_DAYS];
    i32         cell_count;
} axcal_app_t;

/* ------------------------------------------------------------------ */
/* Date arithmetic                                                     */
/* ------------------------------------------------------------------ */

static bool_t axcal_is_leap(i32 year)
{
    return (bool_t)((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
}

static i32 axcal_days_in_month(i32 year, i32 month)
{
    static const i32 days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2 && axcal_is_leap(year)) return 29;
    return days[month - 1];
}

/* Sakamoto's day-of-week.  The month offset table encodes the accumulated drift
 * so no leap-year branch is needed in the hot path. */
static i32 axcal_weekday(i32 year, i32 month, i32 day)
{
    static const i32 offsets[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    i32 adjusted = month < 3 ? year - 1 : year;
    return (adjusted + adjusted / 4 - adjusted / 100 + adjusted / 400 +
            offsets[month - 1] + day) % 7;
}

/* Pack a date into a sortable integer.  The packing is not an epoch: it is
 * year*512 + month*32 + day, which sorts correctly within a year and is only
 * compared, never converted back. */
static i32 axcal_pack(i32 year, i32 month, i32 day)
{
    return year * 512 + month * 32 + day;
}

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

void axcal_init(axcal_app_t *app, gui_rect_t rect, i32 year, i32 month, i32 day)
{
    if (!app) return;
    ax_memset(app, 0, sizeof(*app));
    app->rect = rect;
    app->year = year;
    app->month = month;
    app->today_day = day;
    app->selected_day = day;
    app->hover_day = -1;
    app->first_weekday = 0;
}

void axcal_add_event(axcal_app_t *app, axcal_kind_t kind, const char *title,
                    const char *place, i32 day, i32 minute_of_day,
                    i32 duration_minutes)
{
    if (!app || app->event_count >= AXCA_MAX_EVENTS) return;
    axcal_event_t *event = &app->events[app->event_count++];
    ax_memset(event, 0, sizeof(*event));
    event->kind = kind;
    ax_strlcpy(event->title, title ? title : "", sizeof(event->title));
    ax_strlcpy(event->place, place ? place : "", sizeof(event->place));
    event->year = app->year;
    event->month = app->month;
    event->day = day;
    event->minute_of_day = minute_of_day;
    event->duration_minutes = duration_minutes < 15 ? 15 : duration_minutes;
}

i32 axcal_events_on(const axcal_app_t *app, i32 day)
{
    i32 count = 0;
    for (i32 i = 0; i < app->event_count; i++) {
        if (app->events[i].day == day && app->events[i].month == app->month &&
            app->events[i].year == app->year) {
            count++;
        }
    }
    return count;
}

/* Agenda order: by day, then by start time.  Sorting an index list rather than
 * the events keeps the caller's order intact, which matters when a new event is
 * appended and its index must remain stable. */
static i32 axcal_agenda_order(const axcal_app_t *app, i32 *indices)
{
    i32 count = 0;
    for (i32 i = 0; i < app->event_count; i++) {
        i32 packed = axcal_pack(app->events[i].year, app->events[i].month,
                               app->events[i].day);
        i32 j = count;
        while (j > 0) {
            const axcal_event_t *previous = &app->events[indices[j - 1]];
            i32 previous_packed = axcal_pack(previous->year, previous->month,
                                            previous->day);
            bool_t after = previous_packed < packed ||
                           (previous_packed == packed &&
                            previous->minute_of_day < app->events[i].minute_of_day);
            if (after) break;
            indices[j] = indices[j - 1];
            j--;
        }
        indices[j] = i;
        count++;
    }
    return count;
}

void axcal_navigate(axcal_app_t *app, i32 delta)
{
    if (!app) return;
    app->month += delta;
    while (app->month > 12) {
        app->month -= 12;
        app->year++;
    }
    while (app->month < 1) {
        app->month += 12;
        app->year--;
    }
    if (app->selected_day > axcal_days_in_month(app->year, app->month)) {
        app->selected_day = axcal_days_in_month(app->year, app->month);
    }
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* The grid always shows six weeks.  A month can start on the last day of a
 * week and span five rows plus one day, and a five-row grid that suddenly grows
 * to six on some months is a layout jump nobody wants. */
static i32 axcal_build_cells(axcal_app_t *app)
{
    i32 first = axcal_weekday(app->year, app->month, 1);
    i32 lead = (first - app->first_weekday + 7) % 7;
    (void)lead;
    i32 previous_month = app->month - 1;
    i32 previous_year = app->year;
    if (previous_month < 1) {
        previous_month = 12;
        previous_year--;
    }
    i32 previous_days = axcal_days_in_month(previous_year, previous_month);

    gui_rect_t grid = gui_rect(app->rect.x + 16, app->rect.y + 92,
                               app->rect.w - 32,
                               app->rect.h - 116);
    i32 cell_w = grid.w / AXCA_WEEKDAYS;
    app->cell_count = AXCA_DAYS;
    for (i32 i = 0; i < AXCA_DAYS; i++) {
        i32 column = i % AXCA_WEEKDAYS;
        i32 row = i / AXCA_WEEKDAYS;
        app->cells[i] = gui_rect(grid.x + column * cell_w,
                                 grid.y + row * AXCA_CELL_HEIGHT, cell_w - 2,
                                 AXCA_CELL_HEIGHT - 2);
        (void)lead;
        (void)previous_days;
/**/    }
    return app->cell_count;
}

/* Which day number a cell represents, accounting for the lead-in from the
 * previous month and the overflow into the next. */
static i32 axcal_cell_day(const axcal_app_t *app, i32 index, i32 *month_delta)
{
    i32 first = axcal_weekday(app->year, app->month, 1);
    i32 lead = (first - app->first_weekday + 7) % 7;
    i32 offset = index - lead;
    if (month_delta) *month_delta = 0;
    if (offset < 0) {
        i32 previous_month = app->month - 1;
        i32 previous_year = app->year;
        if (previous_month < 1) {
            previous_month = 12;
            previous_year--;
        }
        i32 days = axcal_days_in_month(previous_year, previous_month);
        if (month_delta) *month_delta = -1;
        return days + offset + 1;
    }
    i32 days = axcal_days_in_month(app->year, app->month);
    if (offset >= days) {
        if (month_delta) *month_delta = 1;
        return offset - days + 1;
    }
    return offset + 1;
}

/* ------------------------------------------------------------------ */
/* Paint                                                               */
/* ------------------------------------------------------------------ */

static u32 axcal_kind_colour(axcal_kind_t kind, const axau_palette_t *palette)
{
    switch (kind) {
    case AXCA_EVENT_TASK: return axau_colour(palette, AXAU_COLOUR_SUCCESS);
    case AXCA_EVENT_REMINDER: return axau_colour(palette, AXAU_COLOUR_WARNING);
    case AXCA_EVENT_PERSONAL: return axui_lighten(
        axau_colour(palette, AXAU_COLOUR_ACCENT), 30);
    case AXCA_EVENT_MEETING:
    default: return axau_colour(palette, AXAU_COLOUR_ACCENT);
    }
}

void axcal_paint(gui_surface_t *surface, axcal_app_t *app,
                const axau_palette_t *palette)
{
    if (!surface || !app || !palette) return;
    gui_fill_rect(surface, app->rect, axau_colour(palette, AXAU_COLOUR_SURFACE));

    char title[40];
    ax_snprintf(title, sizeof(title), "%d / %02d", app->year, app->month);
    axui_draw_text(surface, app->rect.x + 16, app->rect.y + 14, title,
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 3));
    /* Month stepper: two chevrons drawn as text glyphs, which keeps the
     * header independent of the icon table for one surface. */
    gui_rect_t previous = gui_rect(app->rect.x + app->rect.w - 72,
                                   app->rect.y + 12, 26, 26);
    gui_rect_t next = gui_rect(app->rect.x + app->rect.w - 42,
                               app->rect.y + 12, 26, 26);
    gui_fill_round_rect(surface, previous, 4,
                        axau_colour(palette, AXAU_COLOUR_CONTROL));
    gui_fill_round_rect(surface, next, 4,
                        axau_colour(palette, AXAU_COLOUR_CONTROL));
    axui_draw_text_center(surface, previous, "<",
                          axui_style(axau_colour(palette,
                                                 AXAU_COLOUR_TEXT_SECONDARY), 1));
    axui_draw_text_center(surface, next, ">",
                          axui_style(axau_colour(palette,
                                                 AXAU_COLOUR_TEXT_SECONDARY), 1));

    const char *weekdays[7] = {"S", "M", "T", "W", "T", "F", "S"};
    i32 cell_w = (app->rect.w - 32) / AXCA_WEEKDAYS;
    for (i32 i = 0; i < AXCA_WEEKDAYS; i++) {
        i32 label_index = (app->first_weekday + i) % 7;
        axui_draw_text_center(surface,
                              gui_rect(app->rect.x + 16 + i * cell_w,
                                       app->rect.y + 72, cell_w, 18),
                              weekdays[label_index],
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_TERTIARY), 1));
    }

    i32 count = axcal_build_cells(app);
    for (i32 i = 0; i < count; i++) {
        gui_rect_t cell = app->cells[i];
        i32 month_delta = 0;
        i32 day = axcal_cell_day(app, i, &month_delta);
        bool_t outside = (bool_t)(month_delta != 0);
        bool_t today = (bool_t)(!outside && day == app->today_day);
        bool_t selected = (bool_t)(!outside && day == app->selected_day);
        u32 background = axau_colour(palette, AXAU_COLOUR_SURFACE_ALT);
        if (selected) {
            background = axui_colour_opacity(
                axau_colour(palette, AXAU_COLOUR_ACCENT), 20);
        } else if (today) {
            background = axui_colour_opacity(
                axau_colour(palette, AXAU_COLOUR_ACCENT), 10);
        }
        gui_fill_round_rect(surface, cell, 4, background);
        if (app->hover_day == day && !outside) {
            axui_round_rect_border(surface, cell, 4, 1,
                                   axau_colour(palette, AXAU_COLOUR_STROKE));
        }
        char number[8];
        ax_snprintf(number, sizeof(number), "%d", day);
        axui_draw_text(surface, cell.x + 6, cell.y + 4, number,
                       axui_style(outside
                                      ? axau_colour(palette, AXAU_COLOUR_TEXT_DISABLED)
                                      : axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        if (outside) continue;
        i32 events = axcal_events_on(app, day);
        i32 shown = 0;
        for (i32 e = 0; e < app->event_count && shown < 2; e++) {
            const axcal_event_t *event = &app->events[e];
            if (event->day != day) continue;
            gui_rect_t chip = gui_rect(cell.x + 3, cell.y + 20 + shown * 14,
                                       cell.w - 6, 12);
            gui_fill_round_rect(surface, chip, 2,
                                axui_colour_opacity(
                                    axcal_kind_colour(event->kind, palette), 40));
            axui_draw_text_clip(surface, chip, chip.x + 3, chip.y + 1,
                                event->title,
                                axui_style(axau_colour(palette,
                                                       AXAU_COLOUR_TEXT), 1));
            shown++;
        }
        if (events > 2) {
            char more[12];
            ax_snprintf(more, sizeof(more), "+%d", events - 2);
            axui_draw_text(surface, cell.x + 4, cell.y + 50, more,
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT_TERTIARY), 1));
        }
    }

    if (app->agenda_open) {
        i32 indices[AXCA_MAX_EVENTS];
        i32 agenda_count = axcal_agenda_order(app, indices);
        gui_rect_t panel = gui_rect(app->rect.x + app->rect.w - 240,
                                    app->rect.y + 92, 224, app->rect.h - 116);
        gui_fill_round_rect(surface, panel, AXAU_RADIUS_CARD,
                            axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
        axui_draw_text(surface, panel.x + 12, panel.y + 10, "Agenda",
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
        i32 y = panel.y + 34;
        for (i32 i = 0; i < agenda_count; i++) {
            if (y + 48 > panel.y + panel.h) break;
            const axcal_event_t *event = &app->events[indices[i]];
            gui_fill_round_rect(surface, gui_rect(panel.x + 12, y, 3, 40), 1,
                                axcal_kind_colour(event->kind, palette));
            char time[16];
            ax_snprintf(time, sizeof(time), "%02d:%02d", event->minute_of_day / 60,
                        event->minute_of_day % 60);
            axui_draw_text(surface, panel.x + 22, y, time,
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT_SECONDARY), 1));
            axui_draw_text_clip(surface,
                                gui_rect(panel.x + 70, y, panel.w - 84, 16),
                                panel.x + 70, y, event->title,
                                axui_style(axau_colour(palette,
                                                       AXAU_COLOUR_TEXT), 1));
            axui_draw_text_clip(surface,
                                gui_rect(panel.x + 22, y + 20, panel.w - 36, 16),
                                panel.x + 22, y + 20, event->place,
                                axui_style(axau_colour(palette,
                                                       AXAU_COLOUR_TEXT_TERTIARY), 1));
            y += 48;
        }
        if (agenda_count == 0) {
            axui_draw_text_center(surface, gui_rect(panel.x, y + 16, panel.w, 20),
                                  "Nothing scheduled",
                                  axui_style(axau_colour(palette,
                                                         AXAU_COLOUR_TEXT_TERTIARY), 1));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

i32 axcal_click(axcal_app_t *app, i32 x, i32 y)
{
    if (!app) return 0;
    gui_rect_t previous = gui_rect(app->rect.x + app->rect.w - 72,
                                   app->rect.y + 12, 26, 26);
    gui_rect_t next = gui_rect(app->rect.x + app->rect.w - 42,
                               app->rect.y + 12, 26, 26);
    if (gui_rect_contains(previous, x, y)) {
        axcal_navigate(app, -1);
        return 1;
    }
    if (gui_rect_contains(next, x, y)) {
        axcal_navigate(app, 1);
        return 2;
    }
    i32 count = axcal_build_cells(app);
    for (i32 i = 0; i < count; i++) {
        if (!gui_rect_contains(app->cells[i], x, y)) continue;
        i32 month_delta = 0;
        i32 day = axcal_cell_day(app, i, &month_delta);
        if (month_delta != 0) {
            axcal_navigate(app, month_delta);
        }
        app->selected_day = day;
        return 10 + day;
    }
    return 0;
}

void axcal_hover(axcal_app_t *app, i32 x, i32 y)
{
    if (!app) return;
    app->hover_day = -1;
    i32 count = app->cell_count > 0 ? app->cell_count : axcal_build_cells(app);
    for (i32 i = 0; i < count; i++) {
        if (!gui_rect_contains(app->cells[i], x, y)) continue;
        i32 month_delta = 0;
        i32 day = axcal_cell_day(app, i, &month_delta);
        if (month_delta == 0) app->hover_day = day;
        return;
    }
}

/* Agenda row lookup: returns the event index under the point, or -1.  The
 * packed order is recomputed so the hit matches what is painted even after an
 * event is appended. */
i32 axcal_agenda_hit(axcal_app_t *app, i32 x, i32 y)
{
    if (!app || !app->agenda_open) return -1;
    gui_rect_t panel = gui_rect(app->rect.x + app->rect.w - 240,
                                app->rect.y + 92, 224, app->rect.h - 116);
    if (!gui_rect_contains(panel, x, y)) return -1;
    i32 indices[AXCA_MAX_EVENTS];
    i32 count = axcal_agenda_order(app, indices);
    i32 row = (y - panel.y - 34) / 48;
    if (row < 0 || row >= count) return -1;
    return indices[row];
}

bool_t axcal_toggle_event(axcal_app_t *app, i32 index)
{
    if (!app || index < 0 || index >= app->event_count) return (bool_t)0;
    app->events[index].done = (bool_t)!app->events[index].done;
    return app->events[index].done;
}

void axcal_dump(const axcal_app_t *app)
{
    if (!app) return;
    printk("calendar: %d/%02d, %d events, selected=%d, agenda=%d\n", app->year,
           app->month, app->event_count, app->selected_day, app->agenda_open);
}

