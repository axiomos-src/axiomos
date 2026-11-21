/*
 * AxiomOS Aurora - weather panel.
 *
 * A compact forecast surface: current conditions, an hour strip, a five day
 * outlook and an air quality rail.  The data model is deliberately simple and
 * offline: a provider interface feeds the panel, and the bundled sample provider
 * synthesises plausible values so the shell can be exercised without a network
 * stack.  All drawing is integer only, matching the rest of Aurora.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>
#include "gui.h"
#include "aurora.h"

#define AXAW_DAYS             5
#define AXAW_HOURS            12
#define AXAW_CONDITIONS       9
#define AXAW_MAX_LOCATIONS    6
#define AXAW_CARD_RADIUS      AXAU_RADIUS_CARD
#define AXAW_STRIP_HEIGHT     64
#define AXAW_ROW_HEIGHT       40
#define AXAW_ICON_RADIUS      20

#define AXAW_CLEAR            0
#define AXAW_PARTLY           1
#define AXAW_CLOUDY           2
#define AXAW_OVERCAST         3
#define AXAW_RAIN             4
#define AXAW_STORM            5
#define AXAW_SNOW             6
#define AXAW_FOG              7
#define AXAW_WIND             8

#define AXAW_VIEW_SUMMARY     0
#define AXAW_VIEW_HOURLY      1
#define AXAW_VIEW_DAILY       2
#define AXAW_VIEW_DETAILS     3

typedef struct axaw_hour
{
    u32  hour;
    u32  temperature;
    u32  precipitation;
    u32  humidity;
    u32  wind;
    u8   condition;
    bool_t daylight;
} axaw_hour_t;

typedef struct axaw_day
{
    u32  weekday;
    u32  high;
    u32  low;
    u32  precipitation;
    u32  sunrise;
    u32  sunset;
    u8   condition;
    u16  label;
} axaw_day_t;

typedef struct axaw_location
{
    u16  name;
    u32  latitude;
    u32  longitude;
    u32  elevation;
    u32  timezone;
    bool_t pinned;
} axaw_location_t;

typedef struct axaw_model
{
    axaw_location_t locations[AXAW_MAX_LOCATIONS];
    axaw_hour_t hours[AXAW_HOURS];
    axaw_day_t days[AXAW_DAYS];
    u32  location_count;
    u32  selected;
    u32  temperature;
    u32  feels_like;
    u32  humidity;
    u32  pressure;
    u32  wind_speed;
    u32  wind_direction;
    u32  visibility;
    u32  uv_index;
    u32  air_quality;
    u32  updated_minutes;
    u8   condition;
    u8   view;
    bool_t celsius;
    bool_t stale;
} axaw_model_t;

typedef struct axaw_layout
{
    gui_rect_t card;
    gui_rect_t headline;
    gui_rect_t hour_strip;
    gui_rect_t day_rows;
    gui_rect_t air_rail;
    gui_rect_t footer;
    gui_rect_t location_chip;
} axaw_layout_t;

static axaw_model_t axaw_model;

static void axaw_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axaw_condition_name(u8 condition)
{
    switch (condition)
    {
    case AXAW_CLEAR:    return 'c';
    case AXAW_PARTLY:   return 'p';
    case AXAW_CLOUDY:   return 'y';
    case AXAW_OVERCAST: return 'o';
    case AXAW_RAIN:     return 'r';
    case AXAW_STORM:    return 's';
    case AXAW_SNOW:     return 'n';
    case AXAW_FOG:      return 'f';
    case AXAW_WIND:     return 'w';
    default:            return '?';
    }
}

static const char *axaw_condition_text(u8 condition)
{
    switch (condition)
    {
    case AXAW_CLEAR:    return "Clear";
    case AXAW_PARTLY:   return "Partly cloudy";
    case AXAW_CLOUDY:   return "Cloudy";
    case AXAW_OVERCAST: return "Overcast";
    case AXAW_RAIN:     return "Rain";
    case AXAW_STORM:    return "Thunderstorm";
    case AXAW_SNOW:     return "Snow";
    case AXAW_FOG:      return "Fog";
    case AXAW_WIND:     return "Windy";
    default:            return "Unknown";
    }
}

static u32 axaw_warm(u32 base, u32 delta)
{
    return base > delta * 4 ? base - delta * 4 : 4;
}

static u32 axaw_tint_for(u8 condition, bool_t daylight)
{
    switch (condition)
    {
    case AXAW_CLEAR:
        return daylight ? GUI_RGB(96, 170, 255) : GUI_RGB(44, 62, 110);
    case AXAW_PARTLY:
        return daylight ? GUI_RGB(120, 168, 226) : GUI_RGB(56, 70, 104);
    case AXAW_CLOUDY:
        return GUI_RGB(132, 146, 164);
    case AXAW_OVERCAST:
        return GUI_RGB(104, 112, 126);
    case AXAW_RAIN:
        return GUI_RGB(78, 104, 138);
    case AXAW_STORM:
        return GUI_RGB(62, 68, 96);
    case AXAW_SNOW:
        return GUI_RGB(168, 190, 214);
    case AXAW_FOG:
        return GUI_RGB(140, 148, 152);
    case AXAW_WIND:
        return GUI_RGB(118, 138, 128);
    default:
        return GUI_RGB(120, 120, 120);
    }
}

static void axaw_icon(gui_surface_t *surface, gui_rect_t box, u8 condition, u32 colour)
{
    u32 cx = box.x + box.w / 2;
    u32 cy = box.y + box.h / 2;
    u32 radius = AXAW_ICON_RADIUS;
    switch (condition)
    {
    case AXAW_CLEAR:
        gui_fill_round_rect(surface, gui_rect(cx - radius, cy - radius, radius * 2, radius * 2), radius, colour);
        for (u32 arm = 0; arm < 8; arm++)
        {
            u32 dx = (arm == 0 || arm == 4) ? 1 : (arm == 2 || arm == 6 ? 0 : 1);
            u32 dy = (arm == 2 || arm == 6) ? 1 : (arm == 0 || arm == 4 ? 0 : 1);
            u32 length = radius + 8;
            for (u32 step = radius + 2; step < length; step++)
            {
                gui_fill_rect(surface, gui_rect(cx + dx * step - 1, cy + dy * step - 1, 3, 3), colour);
                if (arm == 0 || arm == 4)
                    gui_fill_rect(surface, gui_rect(cx - dx * step - 1, cy + dy * step - 1, 3, 3), colour);
                if (arm == 2 || arm == 6)
                    gui_fill_rect(surface, gui_rect(cx + dx * step - 1, cy - dy * step - 1, 3, 3), colour);
            }
        }
        break;
    case AXAW_PARTLY:
        gui_fill_round_rect(surface, gui_rect(cx - radius, cy - radius - 4, radius * 2, radius * 2), radius, colour);
        gui_fill_round_rect(surface, gui_rect(cx - radius - 8, cy - 2, radius * 2 + 10, 18), 9, GUI_RGB(230, 232, 236));
        break;
    case AXAW_CLOUDY:
    case AXAW_OVERCAST:
        gui_fill_round_rect(surface, gui_rect(cx - radius - 7, cy - 8, radius * 2 + 6, 20), 10, colour);
        gui_fill_round_rect(surface, gui_rect(cx - radius + 3, cy - 16, radius, radius), 10, colour);
        break;
    case AXAW_RAIN:
    case AXAW_STORM:
        gui_fill_round_rect(surface, gui_rect(cx - radius - 7, cy - 12, radius * 2 + 6, 20), 10, colour);
        for (u32 drop = 0; drop < 4; drop++)
            gui_fill_rect(surface, gui_rect(cx - 14 + drop * 9, cy + 10, 2, 10), GUI_RGB(150, 190, 230));
        if (condition == AXAW_STORM)
            gui_fill_rect(surface, gui_rect(cx - 2, cy + 8, 4, 14), GUI_RGB(244, 220, 120));
        break;
    case AXAW_SNOW:
        gui_fill_round_rect(surface, gui_rect(cx - radius - 7, cy - 12, radius * 2 + 6, 20), 10, colour);
        for (u32 flake = 0; flake < 5; flake++)
            gui_fill_rect(surface, gui_rect(cx - 14 + flake * 7, cy + 10 + (flake & 1) * 4, 3, 3), GUI_RGB(236, 244, 252));
        break;
    case AXAW_FOG:
        for (u32 line = 0; line < 4; line++)
            gui_fill_rect(surface, gui_rect(cx - 18, cy - 10 + line * 6, 36 - (line & 1) * 8, 3), colour);
        break;
    case AXAW_WIND:
        for (u32 line = 0; line < 3; line++)
            gui_fill_rect(surface, gui_rect(cx - 18 + line * 2, cy - 8 + line * 7, 30 - line * 4, 2), colour);
        break;
    default:
        gui_fill_rect(surface, gui_rect(cx - 4, cy - 4, 8, 8), colour);
        break;
    }
}

static void axaw_text_number(gui_surface_t *surface, u32 x, u32 y, u32 value, u32 colour, u32 scale)
{
    char text[8];
    u32 digits = 1;
    u32 temp = value;
    while (temp >= 10)
    {
        temp /= 10;
        digits++;
    }
    for (u32 i = 0; i < digits; i++)
    {
        text[digits - 1 - i] = (char)('0' + (value % 10));
        value /= 10;
    }
    text[digits] = 0;
    gui_text(surface, x, y, text, colour, scale);
}

static void axaw_text_signed(gui_surface_t *surface, u32 x, u32 y, i32 value, u32 colour, u32 scale)
{
    char text[12];
    u32 length = 0;
    u32 magnitude = value < 0 ? (u32)(-value) : (u32)value;
    if (value < 0)
        text[length++] = '-';
    u32 digits = 1;
    u32 temp = magnitude;
    while (temp >= 10)
    {
        temp /= 10;
        digits++;
    }
    for (u32 i = 0; i < digits; i++)
    {
        text[length + digits - 1 - i] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    }
    length += digits;
    text[length] = 0;
    gui_text(surface, x, y, text, colour, scale);
}

/* ------------------------------------------------------------- providers */

static void axaw_sample_provider(u32 seed)
{
    u32 base = 12 + (seed % 9);
    for (u32 i = 0; i < AXAW_HOURS; i++)
    {
        axaw_hour_t *hour = &axaw_model.hours[i];
        hour->hour = (8 + i) % 24;
        hour->temperature = base + ((i * 3 + seed) % 7);
        hour->precipitation = (i * 17 + seed) % 60;
        hour->humidity = 40 + ((i * 11 + seed) % 45);
        hour->wind = 3 + ((i * 5) % 12);
        hour->daylight = hour->hour >= 6 && hour->hour < 19;
        if (hour->precipitation > 40)
            hour->condition = AXAW_RAIN;
        else if (hour->precipitation > 25)
            hour->condition = AXAW_CLOUDY;
        else
            hour->condition = hour->daylight ? AXAW_PARTLY : AXAW_CLEAR;
    }
    for (u32 i = 0; i < AXAW_DAYS; i++)
    {
        axaw_day_t *day = &axaw_model.days[i];
        day->weekday = (i + 3) % 7;
        day->high = base + 4 + ((i * 4 + seed) % 6);
        day->low = axaw_warm(day->high, 6 + (i % 3));
        day->precipitation = (i * 23 + seed) % 70;
        day->sunrise = 6 * 60 + (i * 2) % 20;
        day->sunset = 18 * 60 + 20 - (i * 3) % 25;
        day->condition = day->precipitation > 45 ? AXAW_RAIN : (day->precipitation > 20 ? AXAW_CLOUDY : AXAW_CLEAR);
    }
    axaw_model.temperature = axaw_model.hours[0].temperature;
    axaw_model.feels_like = axaw_model.temperature - 1;
    axaw_model.humidity = axaw_model.hours[0].humidity;
    axaw_model.pressure = 1008 + (seed % 18);
    axaw_model.wind_speed = axaw_model.hours[0].wind;
    axaw_model.wind_direction = (seed * 37) % 360;
    axaw_model.visibility = 9 + (seed % 5);
    axaw_model.uv_index = 1 + (seed % 6);
    axaw_model.air_quality = 30 + (seed % 50);
    axaw_model.condition = axaw_model.hours[0].condition;
    axaw_model.updated_minutes = (seed % 20);
    axaw_model.stale = false;
}

void axaw_load_sample(u32 seed)
{
    axaw_sample_provider(seed);
}

bool_t axaw_add_location(u16 name, u32 latitude, u32 longitude, u32 timezone)
{
    if (axaw_model.location_count >= AXAW_MAX_LOCATIONS)
        return false;
    axaw_location_t *location = &axaw_model.locations[axaw_model.location_count];
    location->name = name;
    location->latitude = latitude;
    location->longitude = longitude;
    location->elevation = 0;
    location->timezone = timezone;
    location->pinned = false;
    axaw_model.location_count++;
    return true;
}

bool_t axaw_select(u32 index)
{
    if (index >= axaw_model.location_count)
        return false;
    axaw_model.selected = index;
    axaw_sample_provider(index * 13 + 5);
    return true;
}

/* ------------------------------------------------------------------ unit */

u32 axaw_display_temperature(u32 celsius)
{
    if (axaw_model.celsius)
        return celsius;
    return (celsius * 9) / 5 + 32;
}

void axaw_set_unit(bool_t celsius)
{
    axaw_model.celsius = celsius;
}

bool_t axaw_unit_celsius(void)
{
    return axaw_model.celsius;
}

void axaw_set_view(u32 view)
{
    if (view > AXAW_VIEW_DETAILS)
        return;
    axaw_model.view = (u8)view;
}

u32 axaw_view(void)
{
    return axaw_model.view;
}

/* --------------------------------------------------------------- painting */

void axaw_layout_for(gui_rect_t bounds, axaw_layout_t *layout)
{
    if (layout == NULL_PTR)
        return;
    layout->card = bounds;
    layout->headline = gui_rect(bounds.x + 20, bounds.y + 16, bounds.w - 40, 72);
    layout->location_chip = gui_rect(bounds.x + bounds.w - 132, bounds.y + 20, 112, 28);
    layout->hour_strip = gui_rect(bounds.x + 20, layout->headline.y + layout->headline.h + 12,
                                  bounds.w - 40, AXAW_STRIP_HEIGHT);
    layout->day_rows = gui_rect(bounds.x + 20, layout->hour_strip.y + layout->hour_strip.h + 12,
                                bounds.w - 40, AXAW_DAYS * AXAW_ROW_HEIGHT);
    layout->air_rail = gui_rect(bounds.x + 20, layout->day_rows.y + layout->day_rows.h + 8,
                                bounds.w - 40, 28);
    layout->footer = gui_rect(bounds.x + 20, bounds.y + bounds.h - 28,
                              bounds.w - 40, 16);
}

void axaw_paint(gui_surface_t *surface, gui_rect_t bounds)
{
    axaw_layout_t layout;
    u32 text_colour = GUI_RGB(238, 240, 244);
    u32 muted = GUI_RGB(168, 176, 188);
    u32 accent = GUI_RGB(112, 178, 246);
    u32 surface_fill = GUI_RGB(28, 32, 40);
    axaw_layout_for(bounds, &layout);
    gui_fill_round_rect(surface, bounds, AXAW_CARD_RADIUS, surface_fill);
    gui_rect_outline(surface, bounds, GUI_RGB(52, 58, 70));
    /* Headline: icon, temperature, condition text. */
    gui_rect_t icon_box = gui_rect(layout.headline.x, layout.headline.y, 64, 64);
    axaw_icon(surface, icon_box, axaw_model.condition, axaw_tint_for(axaw_model.condition, true));
    axaw_text_number(surface, layout.headline.x + 76, layout.headline.y + 4,
                     axaw_display_temperature(axaw_model.temperature), text_colour, 4);
    u32 unit_x = layout.headline.x + 76 + 4 * 8 * 2 + 4;
    gui_text(surface, unit_x, layout.headline.y + 22, axaw_model.celsius ? "C" : "F", muted, 2);
    gui_text(surface, layout.headline.x + 80, layout.headline.y + 52,
             axaw_condition_text(axaw_model.condition), muted, 1);
    gui_text(surface, layout.headline.x + 200, layout.headline.y + 52, "Feels", muted, 1);
    axaw_text_number(surface, layout.headline.x + 240, layout.headline.y + 52,
                     axaw_display_temperature(axaw_model.feels_like), text_colour, 1);
    /* Location chip. */
    gui_fill_round_rect(surface, layout.location_chip, 14, GUI_RGB(42, 48, 60));
    gui_text(surface, layout.location_chip.x + 12, layout.location_chip.y + 10, "Local", muted, 1);
    /* Hour strip. */
    gui_fill_round_rect(surface, layout.hour_strip, AXAW_CARD_RADIUS, GUI_RGB(36, 42, 52));
    u32 slot = layout.hour_strip.w / 6;
    for (u32 i = 0; i < 6 && i < AXAW_HOURS; i++)
    {
        axaw_hour_t *hour = &axaw_model.hours[i];
        gui_rect_t box = gui_rect(layout.hour_strip.x + slot * i + 6, layout.hour_strip.y + 6,
                                  slot - 12, AXAW_STRIP_HEIGHT - 12);
        gui_fill_round_rect(surface, box, 6, GUI_RGB(44, 50, 62));
        axaw_text_number(surface, box.x + 4, box.y + 6, hour->hour, muted, 1);
        axaw_icon(surface, gui_rect(box.x + box.w - 28, box.y + 18, 24, 24), hour->condition,
                  axaw_tint_for(hour->condition, hour->daylight));
        axaw_text_number(surface, box.x + 4, box.y + box.h - 16,
                         axaw_display_temperature(hour->temperature), text_colour, 1);
    }
    /* Day rows. */
    for (u32 i = 0; i < AXAW_DAYS; i++)
    {
        axaw_day_t *day = &axaw_model.days[i];
        gui_rect_t row = gui_rect(layout.day_rows.x, layout.day_rows.y + i * AXAW_ROW_HEIGHT,
                                  layout.day_rows.w, AXAW_ROW_HEIGHT - 4);
        gui_fill_round_rect(surface, row, 6, GUI_RGB(34, 39, 48));
        axaw_text_number(surface, row.x + 10, row.y + 12, day->weekday, muted, 1);
        axaw_icon(surface, gui_rect(row.x + 40, row.y + 8, 24, 24), day->condition,
                  axaw_tint_for(day->condition, true));
        axaw_text_number(surface, row.x + 80, row.y + 12, axaw_display_temperature(day->low), muted, 1);
        axaw_text_number(surface, row.x + 130, row.y + 12, axaw_display_temperature(day->high), text_colour, 1);
        gui_hline(surface, row.x + 180, row.x + row.w - 20, row.y + AXAW_ROW_HEIGHT / 2, GUI_RGB(48, 54, 66));
        u32 bar_x = row.x + 200;
        u32 bar_width = row.w > 280 ? row.w - 230 : 40;
        gui_fill_round_rect(surface, gui_rect(bar_x, row.y + 14, bar_width, 8), 4, GUI_RGB(46, 52, 64));
        u32 fill = (day->precipitation * bar_width) / 100;
        if (fill > 0)
            gui_fill_round_rect(surface, gui_rect(bar_x, row.y + 14, fill, 8), 4, accent);
    }
    /* Air quality rail. */
    gui_fill_round_rect(surface, layout.air_rail, 10, GUI_RGB(34, 39, 48));
    gui_text(surface, layout.air_rail.x + 12, layout.air_rail.y + 8, "Air", muted, 1);
    u32 rail_width = layout.air_rail.w - 120;
    gui_fill_round_rect(surface, gui_rect(layout.air_rail.x + 56, layout.air_rail.y + 10, rail_width, 8), 4,
                        GUI_RGB(46, 52, 64));
    u32 level = axaw_model.air_quality;
    if (level > 100)
        level = 100;
    gui_fill_round_rect(surface, gui_rect(layout.air_rail.x + 56, layout.air_rail.y + 10,
                                          (level * rail_width) / 100, 8), 4,
                        level < 40 ? GUI_RGB(120, 200, 140) : (level < 70 ? GUI_RGB(226, 190, 110) : GUI_RGB(226, 120, 110)));
    axaw_text_number(surface, layout.air_rail.x + 56 + rail_width + 12, layout.air_rail.y + 8, level, muted, 1);
    /* Footer. */
    gui_text(surface, layout.footer.x, layout.footer.y, "Updated", muted, 1);
    axaw_text_number(surface, layout.footer.x + 64, layout.footer.y, axaw_model.updated_minutes, muted, 1);
    gui_text(surface, layout.footer.x + 80, layout.footer.y, "min ago", muted, 1);
    if (axaw_model.stale)
        gui_text(surface, layout.footer.x + layout.footer.w - 64, layout.footer.y, "Stale", GUI_RGB(230, 170, 110), 1);
}

void axaw_paint_hourly(gui_surface_t *surface, gui_rect_t bounds)
{
    gui_fill_round_rect(surface, bounds, AXAW_CARD_RADIUS, GUI_RGB(28, 32, 40));
    gui_rect_outline(surface, bounds, GUI_RGB(52, 58, 70));
    u32 row_height = AXAW_HOURS > 0 ? (bounds.h - 32) / AXAW_HOURS : 16;
    if (row_height < 14)
        row_height = 14;
    for (u32 i = 0; i < AXAW_HOURS; i++)
    {
        axaw_hour_t *hour = &axaw_model.hours[i];
        u32 y = bounds.y + 16 + i * row_height;
        if (y + row_height > bounds.y + bounds.h)
            break;
        axaw_text_number(surface, bounds.x + 12, y, hour->hour, GUI_RGB(200, 206, 216), 1);
        axaw_icon(surface, gui_rect(bounds.x + 40, y - 2, 16, 16), hour->condition,
                  axaw_tint_for(hour->condition, hour->daylight));
        axaw_text_number(surface, bounds.x + 72, y, axaw_display_temperature(hour->temperature),
                         GUI_RGB(238, 240, 244), 1);
        axaw_text_number(surface, bounds.x + 112, y, hour->precipitation, GUI_RGB(150, 190, 230), 1);
        gui_text(surface, bounds.x + 128, y, "%", GUI_RGB(150, 190, 230), 1);
        axaw_text_number(surface, bounds.x + 160, y, hour->wind, GUI_RGB(168, 176, 188), 1);
    }
}

/* --------------------------------------------------------------- details */

u32 axaw_detail_count(void)
{
    return 7;
}

bool_t axaw_detail(u32 index, const char **label, u32 *value, const char **unit)
{
    if (label == NULL_PTR || value == NULL_PTR || unit == NULL_PTR)
        return false;
    switch (index)
    {
    case 0: *label = "Humidity"; *value = axaw_model.humidity; *unit = "%"; return true;
    case 1: *label = "Pressure"; *value = axaw_model.pressure; *unit = "hPa"; return true;
    case 2: *label = "Wind"; *value = axaw_model.wind_speed; *unit = "km/h"; return true;
    case 3: *label = "Direction"; *value = axaw_model.wind_direction; *unit = "deg"; return true;
    case 4: *label = "Visibility"; *value = axaw_model.visibility; *unit = "km"; return true;
    case 5: *label = "UV index"; *value = axaw_model.uv_index; *unit = ""; return true;
    case 6: *label = "Air"; *value = axaw_model.air_quality; *unit = "aqi"; return true;
    default: return false;
    }
}

u32 axaw_condition_code(void)
{
    return axaw_condition_name(axaw_model.condition);
}

/* -------------------------------------------------------------- selftest */

bool_t axaw_selftest(void)
{
    axaw_zero(&axaw_model, (u32)sizeof(axaw_model));
    if (!axaw_add_location(0x100, 30, 120, 8))
        return false;
    if (axaw_add_location(0x200, 40, 116, 8))
    {
        /* a second location is allowed */
    }
    if (axaw_model.location_count != 2)
        return false;
    if (!axaw_select(0))
        return false;
    axaw_load_sample(3);
    if (axaw_model.temperature == 0)
        return false;
    if (axaw_model.hours[0].hour != 8)
        return false;
    if (axaw_model.days[0].high <= axaw_model.days[0].low)
        return false;
    axaw_set_unit(true);
    if (!axaw_unit_celsius())
        return false;
    if (axaw_display_temperature(20) != 20)
        return false;
    axaw_set_unit(false);
    if (axaw_display_temperature(20) != 68)
        return false;
    axaw_set_unit(true);
    axaw_set_view(2);
    if (axaw_view() != 2)
        return false;
    axaw_set_view(9);
    if (axaw_view() != 2)
        return false;
    const char *label = NULL_PTR;
    const char *unit = NULL_PTR;
    u32 value = 0;
    if (!axaw_detail(0, &label, &value, &unit))
        return false;
    if (value != axaw_model.humidity)
        return false;
    if (axaw_detail(7, &label, &value, &unit))
        return false;
    if (axaw_select(5))
        return false;
    axaw_layout_t layout;
    axaw_layout_for(gui_rect(10, 10, 420, 640), &layout);
    if (layout.card.w != 420)
        return false;
    if (layout.hour_strip.y <= layout.headline.y)
        return false;
    if (axaw_condition_code() == 0)
        return false;
    return true;
}

