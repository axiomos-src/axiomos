/*
 * AxiomOS - vector icon vocabulary.
 *
 * Icons are drawn from primitives at paint time instead of sampled from a bitmap
 * atlas.  A bitmap atlas would be smaller on disk but would need one sheet per
 * scale and would look soft at 125% and 150%; geometry stays sharp and the theme
 * can recolour it by passing a different colour.  The cost is more instructions
 * per icon, which is acceptable because icons are small and the compositor
 * caches the layer a toolbar lives in.
 *
 * The vocabulary is intentionally small.  Every glyph here has a meaning used by
 * at least two surfaces; speculative icons are how icon sets become inconsistent.
 */

#include <axiom/types.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

typedef enum axic_id {
    AXIC_FOLDER = 0,
    AXIC_DOCUMENT,
    AXIC_TERMINAL,
    AXIC_GEAR,
    AXIC_SEARCH,
    AXIC_HOME,
    AXIC_GLOBE,
    AXIC_BATTERY,
    AXIC_SIGNAL,
    AXIC_SPEAKER,
    AXIC_SUN,
    AXIC_MOON,
    AXIC_CLOCK,
    AXIC_CALENDAR,
    AXIC_PHOTO,
    AXIC_MUSIC,
    AXIC_MAIL,
    AXIC_BAG,
    AXIC_PEOPLE,
    AXIC_POWER,
    AXIC_CLOSE,
    AXIC_CHECK,
    AXIC_CHEVRON_RIGHT,
    AXIC_CHEVRON_DOWN,
    AXIC_LOCK,
    AXIC_SHIELD,
    AXIC_CPU,
    AXIC_DISK,
    AXIC_PRINTER,
    AXIC_CAMERA,
    AXIC_PIN,
    AXIC_STAR,
    AXIC_HEART,
    AXIC_SHARE,
    AXIC_DOWNLOAD,
    AXIC_TRASH,
    AXIC_REFRESH,
    AXIC_MENU,
    AXIC_GRID,
    AXIC_LIST,
    AXIC_BELL,
    AXIC_CLIPBOARD,
    AXIC_LINK,
    AXIC_WARNING,
    AXIC_INFO,
    AXIC_USER,
    AXIC_PLAY,
    AXIC_PAUSE,
    AXIC_STOP,
    AXIC_COUNT
} axic_id_t;

/* Icon names map onto the command bar's glyph strings, which are one character
 * each so they fit in a control's leading slot. */
static i32 icon_from_token(const char *token)
{
    if (!token || !token[0]) return -1;
    switch (token[0]) {
    case '/': return AXIC_FOLDER;
    case '.': return AXIC_DOCUMENT;
    case '>': return AXIC_TERMINAL;
    case '*': return AXIC_GEAR;
    case '?': return AXIC_SEARCH;
    case 'h': return AXIC_HOME;
    case 'n': return AXIC_GLOBE;
    case 'b': return AXIC_BATTERY;
    case 'w': return AXIC_SIGNAL;
    case 's': return AXIC_SPEAKER;
    case 'L': return AXIC_SUN;
    case 'D': return AXIC_MOON;
    case 't': return AXIC_CLOCK;
    case 'c': return AXIC_CALENDAR;
    case '#': return AXIC_PHOTO;
    case 'm': return AXIC_MUSIC;
    case 'e': return AXIC_MAIL;
    case 'B': return AXIC_BAG;
    case 'p': return AXIC_PEOPLE;
    case 'P': return AXIC_POWER;
    case 'x': return AXIC_CLOSE;
    case 'v': return AXIC_CHECK;
    case 'r': return AXIC_REFRESH;
    case '!': return AXIC_WARNING;
    case '=': return AXIC_MENU;
    case 'g': return AXIC_GRID;
    case 'l': return AXIC_LIST;
    default: break;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Primitive helpers                                                   */
/* ------------------------------------------------------------------ */

/* A rounded outline is the backbone of most icons: folder, document, camera and
 * printer all begin with one.  Keeping it in one helper means stroke weight and
 * corner radius stay consistent across the set. */
static void icon_outline(gui_surface_t *surface, i32 x, i32 y, i32 w, i32 h,
                         i32 radius, i32 thickness, u32 colour)
{
    axui_round_rect_border(surface, gui_rect(x, y, w, h), radius, thickness,
                           colour);
}

static void icon_dot(gui_surface_t *surface, i32 cx, i32 cy, i32 radius,
                     u32 colour)
{
    axui_circle(surface, cx, cy, radius, colour);
}

static void icon_line(gui_surface_t *surface, i32 x0, i32 y0, i32 x1, i32 y1,
                      u32 colour, i32 thickness)
{
    axui_line_aa(surface, x0, y0, x1, y1, colour, thickness);
}

/* ------------------------------------------------------------------ */
/* Icon bodies                                                         */
/* ------------------------------------------------------------------ */

static void icon_folder(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                        u32 colour)
{
    i32 w = size;
    i32 h = size * 3 / 4;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2 + 2;
    /* Tab first, then the body: the tab is what distinguishes a folder from a
     * plain rectangle at 16 pixels. */
    gui_fill_round_rect(surface, gui_rect(x, y, w / 2, h / 3), 2,
                        axui_colour_opacity(colour, 200));
    gui_fill_round_rect(surface, gui_rect(x, y + h / 5, w, h - h / 5), 3,
                        axui_colour_opacity(colour, 200));
}

static void icon_document(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                          u32 colour)
{
    i32 w = size * 3 / 4;
    i32 h = size;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    icon_outline(surface, x, y, w, h, 2, 1, colour);
    for (i32 i = 0; i < 4; i++) {
        i32 line_y = y + 5 + i * 4;
        if (line_y + 4 > y + h) break;
        icon_line(surface, x + 3, line_y, x + w - 3, line_y, colour, 1);
    }
    /* Folded corner: two short strokes instead of a filled triangle, so the
     * interior stays empty and the document reads as paper. */
    icon_line(surface, x + w - 5, y + 1, x + w - 5, y + 5, colour, 1);
    icon_line(surface, x + w - 5, y + 5, x + w - 1, y + 5, colour, 1);
}

static void icon_terminal(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                          u32 colour)
{
    i32 w = size;
    i32 h = size * 4 / 5;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    icon_outline(surface, x, y, w, h, 2, 1, colour);
    icon_line(surface, x + 3, y + 4, x + 6, y + h / 2, colour, 1);
    icon_line(surface, x + 6, y + h / 2, x + 3, y + h - 4, colour, 1);
    icon_line(surface, x + 8, y + h - 4, x + w - 3, y + h - 4, colour, 1);
}

static void icon_gear(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    i32 radius = size / 2;
    axui_circle_outline(surface, cx, cy, radius - 2, colour, 2);
    for (i32 i = 0; i < 8; i++) {
        /* Teeth are placed on a circle with an eight-way table rather than
         * trigonometry: the table is exact and avoids a floating-point dependency
         * in a freestanding build. */
        static const i32 dx[8] = {0, 6, 8, 6, 0, -6, -8, -6};
        static const i32 dy[8] = {-8, -6, 0, 6, 8, 6, 0, -6};
        i32 x0 = cx + (dx[i] * (radius - 2)) / 8;
        i32 y0 = cy + (dy[i] * (radius - 2)) / 8;
        i32 x1 = cx + (dx[i] * radius) / 8;
        i32 y1 = cy + (dy[i] * radius) / 8;
        icon_line(surface, x0, y0, x1, y1, colour, 2);
    }
}

static void icon_search(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                        u32 colour)
{
    i32 radius = size / 2 - 2;
    axui_circle_outline(surface, cx - 2, cy - 2, radius, colour, 2);
    icon_line(surface, cx + radius / 2 + 1, cy + radius / 2 + 1, cx + size / 2,
              cy + size / 2, colour, 2);
}

static void icon_home(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    i32 w = size;
    i32 h = size * 4 / 5;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2 + 3;
    axui_fill_triangle(surface,
                       axui_point(cx, y - 4), axui_point(x, y + h / 3),
                       axui_point(x + w, y + h / 3), colour);
    gui_fill_rect(surface, gui_rect(x + 2, y + h / 3, w - 4, h - h / 3 + 2),
                  axui_colour_opacity(colour, 180));
    gui_fill_rect(surface, gui_rect(cx - w / 8, y + h - h / 4, w / 4, h / 4),
                  axui_colour_opacity(colour, 90));
}

static void icon_globe(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    i32 radius = size / 2 - 1;
    axui_circle_outline(surface, cx, cy, radius, colour, 1);
    icon_line(surface, cx - radius, cy, cx + radius, cy, colour, 1);
    gui_vline(surface, cx, cy - radius, radius * 2,
              axui_colour_opacity(colour, 160));
}

static void icon_battery(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                         u32 colour)
{
    i32 w = size - 3;
    i32 h = size / 2 + 2;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    icon_outline(surface, x, y, w, h, 2, 1, colour);
    gui_fill_rect(surface, gui_rect(x + w, y + h / 4, 2, h / 2), colour);
    gui_fill_rect(surface, gui_rect(x + 2, y + 2, (w - 4) * 2 / 3, h - 4),
                  axui_colour_opacity(colour, 200));
}

static void icon_signal(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                        u32 colour)
{
    i32 base_y = cy + size / 2 - 1;
    for (i32 i = 0; i < 4; i++) {
        i32 height = 3 + i * (size / 5);
        gui_fill_rect(surface,
                      gui_rect(cx - size / 2 + i * (size / 5), base_y - height,
                               size / 8, height),
                      axui_colour_opacity(colour, i < 3 ? 200 : 90));
    }
}

static void icon_speaker(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                         u32 colour)
{
    i32 x = cx - size / 2;
    axui_fill_triangle(surface, axui_point(x, cy - 3),
                       axui_point(x + size / 3, cy - size / 2),
                       axui_point(x + size / 3, cy + size / 2), colour);
    gui_fill_rect(surface, gui_rect(x, cy - 3, size / 4, 6), colour);
    axui_arc(surface, x + size / 3, cy, size / 3, -45, 90, 1,
             axui_colour_opacity(colour, 180));
    axui_arc(surface, x + size / 3, cy, size / 2, -50, 100, 1,
             axui_colour_opacity(colour, 120));
}

static void icon_sun(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                     u32 colour)
{
    axui_circle(surface, cx, cy, size / 5, colour);
    for (i32 i = 0; i < 8; i++) {
        static const i32 dx[8] = {0, 5, 7, 5, 0, -5, -7, -5};
        static const i32 dy[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
        i32 inner = size / 4;
        i32 outer = size / 2 - 1;
        icon_line(surface, cx + (dx[i] * inner) / 7, cy + (dy[i] * inner) / 7,
                  cx + (dx[i] * outer) / 7, cy + (dy[i] * outer) / 7, colour, 1);
    }
}

static void icon_moon(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    /* Crescent: a filled disc with an offset disc erased in the surface colour
     * would need a background colour, which the caller does not own.  Two arcs
     * describe the outline instead. */
    axui_arc(surface, cx, cy, size / 2, 40, 280, 2, colour);
    axui_arc(surface, cx + size / 5, cy - size / 8, size / 3, 110, 150, 2, colour);
}

static void icon_clock(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    i32 radius = size / 2 - 1;
    axui_circle_outline(surface, cx, cy, radius, colour, 1);
    icon_line(surface, cx, cy, cx, cy - radius + 2, colour, 2);
    icon_line(surface, cx, cy, cx + radius / 2, cy, colour, 2);
}

static void icon_calendar(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                          u32 colour)
{
    i32 w = size;
    i32 h = size * 4 / 5;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    icon_outline(surface, x, y, w, h, 2, 1, colour);
    gui_fill_rect(surface, gui_rect(x + 1, y + 1, w - 2, h / 4), colour);
    for (i32 row = 0; row < 2; row++) {
        for (i32 col = 0; col < 3; col++) {
            icon_dot(surface, x + 3 + col * (w / 4), y + h / 2 + row * (h / 4),
                     1, axui_colour_opacity(colour, 160));
        }
    }
}

static void icon_photo(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    i32 w = size;
    i32 h = size * 3 / 4;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    icon_outline(surface, x, y, w, h, 2, 1, colour);
    icon_dot(surface, x + w / 4, y + h / 3, size / 8,
             axui_colour_opacity(colour, 180));
    axui_fill_triangle(surface, axui_point(x + 2, y + h - 2),
                       axui_point(x + w / 2, y + h / 3),
                       axui_point(x + w - 2, y + h - 2),
                       axui_colour_opacity(colour, 150));
}

static void icon_music(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    icon_dot(surface, cx - size / 4, cy + size / 3, size / 8, colour);
    icon_dot(surface, cx + size / 4, cy + size / 4, size / 8, colour);
    icon_line(surface, cx - size / 4 + size / 8, cy + size / 3,
              cx - size / 4 + size / 8, cy - size / 3, colour, 1);
    icon_line(surface, cx + size / 4 + size / 8, cy + size / 4,
              cx + size / 4 + size / 8, cy - size / 3, colour, 1);
    icon_line(surface, cx - size / 4 + size / 8, cy - size / 3,
              cx + size / 4 + size / 8, cy - size / 3 - 1, colour, 2);
}

static void icon_mail(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    i32 w = size;
    i32 h = size * 2 / 3;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    icon_outline(surface, x, y, w, h, 2, 1, colour);
    icon_line(surface, x + 2, y + 2, cx, cy + 2, colour, 1);
    icon_line(surface, cx, cy + 2, x + w - 2, y + 2, colour, 1);
}

static void icon_bag(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                     u32 colour)
{
    i32 w = size * 3 / 4;
    i32 h = size;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2 + 2;
    icon_outline(surface, x, y, w, h - 2, 3, 1, colour);
    axui_arc(surface, cx, y, w / 4, 190, 160, 1, colour);
}

static void icon_people(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                        u32 colour)
{
    icon_dot(surface, cx - size / 5, cy - size / 6, size / 6, colour);
    icon_dot(surface, cx + size / 5, cy - size / 8, size / 7,
             axui_colour_opacity(colour, 140));
    axui_arc(surface, cx - size / 5, cy + size / 2, size / 3, 200, 140, 1, colour);
    axui_arc(surface, cx + size / 5, cy + size / 2, size / 4, 200, 140, 1,
             axui_colour_opacity(colour, 140));
}

static void icon_power(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    i32 radius = size / 2 - 1;
    axui_arc(surface, cx, cy, radius, 300, 300, 2, colour);
    icon_line(surface, cx, cy - radius - 1, cx, cy - radius / 4, colour, 2);
}

static void icon_close(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    i32 arm = size / 3;
    icon_line(surface, cx - arm, cy - arm, cx + arm, cy + arm, colour, 2);
    icon_line(surface, cx + arm, cy - arm, cx - arm, cy + arm, colour, 2);
}

static void icon_check(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    icon_line(surface, cx - size / 3, cy, cx - size / 12, cy + size / 4,
              colour, 2);
    icon_line(surface, cx - size / 12, cy + size / 4, cx + size / 3,
              cy - size / 4, colour, 2);
}

static void icon_chevron(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                         u32 colour, bool_t down)
{
    i32 arm = size / 4;
    if (down) {
        icon_line(surface, cx - arm, cy - arm / 2, cx, cy + arm / 2, colour, 2);
        icon_line(surface, cx, cy + arm / 2, cx + arm, cy - arm / 2, colour, 2);
    } else {
        icon_line(surface, cx - arm / 2, cy - arm, cx + arm / 2, cy, colour, 2);
        icon_line(surface, cx + arm / 2, cy, cx - arm / 2, cy + arm, colour, 2);
    }
}

static void icon_lock(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    i32 w = size * 2 / 3;
    i32 h = size / 2;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2 + 3;
    gui_fill_round_rect(surface, gui_rect(x, y, w, h), 2, colour);
    axui_arc(surface, cx, y, w / 3, 190, 160, 2, colour);
    icon_dot(surface, cx, y + h / 2, 1, axui_colour_opacity(colour, 60));
}

static void icon_shield(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                        u32 colour)
{
    i32 w = size;
    i32 h = size * 5 / 6;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    axui_fill_triangle(surface, axui_point(x, y), axui_point(x + w, y),
                       axui_point(cx, y + h), colour);
    icon_line(surface, cx - w / 4, y + h / 2, cx, y + h / 2 + h / 6, colour, 2);
}

static void icon_cpu(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                     u32 colour)
{
    i32 w = size * 3 / 4;
    i32 h = w;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    icon_outline(surface, x, y, w, h, 2, 1, colour);
    gui_fill_rect(surface, gui_rect(x + w / 4, y + h / 4, w / 2, h / 2),
                  axui_colour_opacity(colour, 160));
    for (i32 i = 0; i < 3; i++) {
        gui_vline(surface, x + (w * (i + 1)) / 4, y - 2, 2, colour);
        gui_vline(surface, x + (w * (i + 1)) / 4, y + h, 2, colour);
    }
}

static void icon_disk(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    i32 w = size;
    i32 h = size / 2;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    gui_fill_round_rect(surface, gui_rect(x, y, w, h), 3,
                        axui_colour_opacity(colour, 190));
    gui_fill_rect(surface, gui_rect(x, y + h / 3, w, 1),
                  axui_colour_opacity(colour, 80));
    icon_dot(surface, x + w - 4, y + h / 2, 1, colour);
}

static void icon_printer(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                         u32 colour)
{
    i32 w = size;
    i32 h = size / 3;
    i32 x = cx - w / 2;
    i32 y = cy;
    gui_fill_round_rect(surface, gui_rect(x, y, w, h), 2, colour);
    gui_fill_rect(surface, gui_rect(x + w / 5, y - h / 2, w * 3 / 5, h / 2),
                  axui_colour_opacity(colour, 170));
    gui_fill_rect(surface, gui_rect(x + w / 5, y + h, w * 3 / 5, h / 2),
                  axui_colour_opacity(colour, 140));
}

static void icon_camera(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                        u32 colour)
{
    i32 w = size;
    i32 h = size * 3 / 4;
    i32 x = cx - w / 2;
    i32 y = cy - h / 2;
    icon_outline(surface, x, y, w, h, 2, 1, colour);
    axui_circle_outline(surface, cx, cy, size / 4, colour, 2);
    gui_fill_rect(surface, gui_rect(cx - w / 6, y - 2, w / 3, 3), colour);
}

static void icon_pin(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                     u32 colour)
{
    /* A pin is a stem plus a point: the point is what makes it readable at
     * sixteen pixels, where a teardrop shape would read as a dot. */
    i32 top = cy - size / 2;
    gui_fill_round_rect(surface, gui_rect(cx - 1, top, 2, size - 3), 1, colour);
    gui_fill_round_rect(surface, gui_rect(cx - 3, top, 6, 4), 2, colour);
    axui_fill_triangle(surface, axui_point(cx - 3, cy + 1),
                       axui_point(cx + 3, cy + 1), axui_point(cx, cy + 4),
                       colour);
}

static void icon_star(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    /* Five-point star from an alternating radius polygon; the points table keeps
     * the shape symmetric without trigonometry. */
    static const i32 sx[10] = {0, 2, 7, 3, 4, 0, -4, -3, -7, -2};
    static const i32 sy[10] = {-7, -2, -2, 2, 7, 3, 7, 2, -2, -2};
    for (i32 i = 0; i < 10; i++) {
        i32 j = (i + 1) % 10;
        icon_line(surface, cx + (sx[i] * size) / 16, cy + (sy[i] * size) / 16,
                  cx + (sx[j] * size) / 16, cy + (sy[j] * size) / 16, colour, 1);
    }
}

static void icon_heart(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    axui_arc(surface, cx - size / 5, cy - size / 6, size / 4, 200, 140, 2, colour);
    axui_arc(surface, cx + size / 5, cy - size / 6, size / 4, 200, 140, 2, colour);
    icon_line(surface, cx - size / 2 + size / 20, cy - size / 6 + size / 4,
              cx, cy + size / 3, colour, 2);
    icon_line(surface, cx + size / 2 - size / 20, cy - size / 6 + size / 4,
              cx, cy + size / 3, colour, 2);
}

static void icon_share(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    icon_dot(surface, cx - size / 3, cy, 2, colour);
    icon_dot(surface, cx + size / 3, cy - size / 4, 2, colour);
    icon_dot(surface, cx + size / 3, cy + size / 4, 2, colour);
    icon_line(surface, cx - size / 3 + 2, cy, cx + size / 3 - 2, cy - size / 4,
              colour, 1);
    icon_line(surface, cx - size / 3 + 2, cy, cx + size / 3 - 2, cy + size / 4,
              colour, 1);
}

static void icon_download(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                          u32 colour)
{
    icon_line(surface, cx, cy - size / 2, cx, cy + size / 6, colour, 2);
    icon_line(surface, cx - size / 4, cy - size / 8, cx, cy + size / 6,
              colour, 2);
    icon_line(surface, cx + size / 4, cy - size / 8, cx, cy + size / 6,
              colour, 2);
    icon_line(surface, cx - size / 3, cy + size / 3, cx + size / 3,
              cy + size / 3, colour, 2);
}

static void icon_trash(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    i32 w = size * 2 / 3;
    i32 x = cx - w / 2;
    gui_fill_round_rect(surface, gui_rect(x, cy - size / 2, w, 3), 1, colour);
    icon_outline(surface, x + 2, cy - size / 3, w - 4, size * 2 / 3, 2, 1,
                 colour);
    gui_vline(surface, cx - w / 8, cy - size / 6, size / 4,
              axui_colour_opacity(colour, 150));
    gui_vline(surface, cx + w / 8, cy - size / 6, size / 4,
              axui_colour_opacity(colour, 150));
}

static void icon_refresh(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                         u32 colour)
{
    axui_arc(surface, cx, cy, size / 2 - 2, 30, 280, 2, colour);
    icon_line(surface, cx + size / 4, cy - size / 2 + 2, cx + size / 2 - 1,
              cy - size / 4, colour, 2);
}

static void icon_menu(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    for (i32 i = 0; i < 3; i++) {
        icon_line(surface, cx - size / 2, cy - size / 4 + i * (size / 4),
                  cx + size / 2, cy - size / 4 + i * (size / 4), colour, 2);
    }
}

static void icon_grid(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    i32 cell = size / 3;
    for (i32 row = 0; row < 2; row++) {
        for (i32 col = 0; col < 2; col++) {
            gui_fill_round_rect(surface,
                                gui_rect(cx - size / 2 + col * (cell + 2),
                                         cy - size / 2 + row * (cell + 2), cell,
                                         cell), 1,
                                axui_colour_opacity(colour, 190));
        }
    }
}

static void icon_list(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    for (i32 i = 0; i < 3; i++) {
        i32 y = cy - size / 3 + i * (size / 3);
        icon_dot(surface, cx - size / 3, y, 1, colour);
        icon_line(surface, cx - size / 6, y, cx + size / 2, y, colour, 1);
    }
}

static void icon_bell(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    axui_arc(surface, cx, cy + 1, size / 3, 200, 140, 2, colour);
    icon_line(surface, cx - size / 2 + 1, cy + size / 6, cx + size / 2 - 1,
              cy + size / 6, colour, 2);
    icon_dot(surface, cx, cy + size / 4, 1, colour);
}

static void icon_clipboard(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                           u32 colour)
{
    icon_outline(surface, cx - size / 3, cy - size / 3, size * 2 / 3,
                 size * 3 / 4, 2, 1, colour);
    gui_fill_round_rect(surface,
                        gui_rect(cx - size / 6, cy - size / 2 + 1, size / 3, 3),
                        1, colour);
}

static void icon_link(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    /* Two rounded loops offset along a diagonal, the conventional link mark. */
    icon_outline(surface, cx - size / 2, cy - size / 4, size / 2, size / 4, 3,
                 1, colour);
    icon_outline(surface, cx, cy, size / 2, size / 4, 3, 1, colour);
}

static void icon_warning(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                         u32 colour)
{
    axui_fill_triangle(surface, axui_point(cx, cy - size / 2),
                       axui_point(cx - size / 2, cy + size / 2),
                       axui_point(cx + size / 2, cy + size / 2), colour);
    gui_fill_rect(surface, gui_rect(cx - 1, cy - size / 6, 2, size / 3),
                  0x00FFFFFFu);
    icon_dot(surface, cx, cy + size / 4, 1, 0x00FFFFFFu);
}

static void icon_info(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    axui_circle_outline(surface, cx, cy, size / 2 - 1, colour, 1);
    icon_dot(surface, cx, cy - size / 4, 1, colour);
    gui_fill_rect(surface, gui_rect(cx - 1, cy - size / 8, 2, size / 2), colour);
}

static void icon_user(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    icon_dot(surface, cx, cy - size / 6, size / 5, colour);
    axui_arc(surface, cx, cy + size / 2, size / 3, 200, 140, 1, colour);
}

static void icon_play(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    axui_fill_triangle(surface, axui_point(cx - size / 3, cy - size / 3),
                       axui_point(cx + size / 3, cy), axui_point(cx - size / 3,
                                                                  cy + size / 3),
                       colour);
}

static void icon_pause(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                       u32 colour)
{
    gui_fill_rect(surface, gui_rect(cx - size / 4, cy - size / 3, 3, size * 2 / 3),
                  colour);
    gui_fill_rect(surface, gui_rect(cx + size / 12, cy - size / 3, 3,
                                    size * 2 / 3), colour);
}

static void icon_stop(gui_surface_t *surface, i32 cx, i32 cy, i32 size,
                      u32 colour)
{
    gui_fill_round_rect(surface, gui_rect(cx - size / 4, cy - size / 4,
                                          size / 2, size / 2), 2, colour);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* The public entry point takes a token string so callers can keep storing a
 * single character in their models, and an unknown token falls back to a
 * neutral square rather than drawing nothing. */
void axau_icon_paint(gui_surface_t *surface, const char *token, i32 cx, i32 cy,
                     i32 size, u32 colour)
{
    if (!surface) return;
    if (size < 8) size = 8;
    i32 id = icon_from_token(token);
    switch (id) {
    case AXIC_FOLDER: icon_folder(surface, cx, cy, size, colour); break;
    case AXIC_DOCUMENT: icon_document(surface, cx, cy, size, colour); break;
    case AXIC_TERMINAL: icon_terminal(surface, cx, cy, size, colour); break;
    case AXIC_GEAR: icon_gear(surface, cx, cy, size, colour); break;
    case AXIC_SEARCH: icon_search(surface, cx, cy, size, colour); break;
    case AXIC_HOME: icon_home(surface, cx, cy, size, colour); break;
    case AXIC_GLOBE: icon_globe(surface, cx, cy, size, colour); break;
    case AXIC_BATTERY: icon_battery(surface, cx, cy, size, colour); break;
    case AXIC_SIGNAL: icon_signal(surface, cx, cy, size, colour); break;
    case AXIC_SPEAKER: icon_speaker(surface, cx, cy, size, colour); break;
    case AXIC_SUN: icon_sun(surface, cx, cy, size, colour); break;
    case AXIC_MOON: icon_moon(surface, cx, cy, size, colour); break;
    case AXIC_CLOCK: icon_clock(surface, cx, cy, size, colour); break;
    case AXIC_CALENDAR: icon_calendar(surface, cx, cy, size, colour); break;
    case AXIC_PHOTO: icon_photo(surface, cx, cy, size, colour); break;
    case AXIC_MUSIC: icon_music(surface, cx, cy, size, colour); break;
    case AXIC_MAIL: icon_mail(surface, cx, cy, size, colour); break;
    case AXIC_BAG: icon_bag(surface, cx, cy, size, colour); break;
    case AXIC_PEOPLE: icon_people(surface, cx, cy, size, colour); break;
    case AXIC_POWER: icon_power(surface, cx, cy, size, colour); break;
    case AXIC_CLOSE: icon_close(surface, cx, cy, size, colour); break;
    case AXIC_CHECK: icon_check(surface, cx, cy, size, colour); break;
    case AXIC_CHEVRON_RIGHT: icon_chevron(surface, cx, cy, size, colour, 0); break;
    case AXIC_CHEVRON_DOWN: icon_chevron(surface, cx, cy, size, colour, 1); break;
    case AXIC_LOCK: icon_lock(surface, cx, cy, size, colour); break;
    case AXIC_SHIELD: icon_shield(surface, cx, cy, size, colour); break;
    case AXIC_CPU: icon_cpu(surface, cx, cy, size, colour); break;
    case AXIC_DISK: icon_disk(surface, cx, cy, size, colour); break;
    case AXIC_PRINTER: icon_printer(surface, cx, cy, size, colour); break;
    case AXIC_CAMERA: icon_camera(surface, cx, cy, size, colour); break;
    case AXIC_PIN: icon_pin(surface, cx, cy, size, colour); break;
    case AXIC_STAR: icon_star(surface, cx, cy, size, colour); break;
    case AXIC_HEART: icon_heart(surface, cx, cy, size, colour); break;
    case AXIC_SHARE: icon_share(surface, cx, cy, size, colour); break;
    case AXIC_DOWNLOAD: icon_download(surface, cx, cy, size, colour); break;
    case AXIC_TRASH: icon_trash(surface, cx, cy, size, colour); break;
    case AXIC_REFRESH: icon_refresh(surface, cx, cy, size, colour); break;
    case AXIC_MENU: icon_menu(surface, cx, cy, size, colour); break;
    case AXIC_GRID: icon_grid(surface, cx, cy, size, colour); break;
    case AXIC_LIST: icon_list(surface, cx, cy, size, colour); break;
    case AXIC_BELL: icon_bell(surface, cx, cy, size, colour); break;
    case AXIC_CLIPBOARD: icon_clipboard(surface, cx, cy, size, colour); break;
    case AXIC_LINK: icon_link(surface, cx, cy, size, colour); break;
    case AXIC_WARNING: icon_warning(surface, cx, cy, size, colour); break;
    case AXIC_INFO: icon_info(surface, cx, cy, size, colour); break;
    case AXIC_USER: icon_user(surface, cx, cy, size, colour); break;
    case AXIC_PLAY: icon_play(surface, cx, cy, size, colour); break;
    case AXIC_PAUSE: icon_pause(surface, cx, cy, size, colour); break;
    case AXIC_STOP: icon_stop(surface, cx, cy, size, colour); break;
    default:
        gui_fill_round_rect(surface,
                            gui_rect(cx - size / 4, cy - size / 4, size / 2,
                                     size / 2), 2,
                            axui_colour_opacity(colour, 170));
        break;
    }
}

/* Draw an icon and its label together, with the label baseline aligned to the
 * optical centre of the icon rather than to the box centre. */
void axau_icon_label(gui_surface_t *surface, gui_rect_t rect, const char *token,
                     const char *label, u32 colour)
{
    if (!surface) return;
    i32 icon_size = rect.h - 8;
    if (icon_size > 20) icon_size = 20;
    axau_icon_paint(surface, token, rect.x + icon_size / 2 + 4,
                    rect.y + rect.h / 2, icon_size, colour);
    axui_draw_text(surface, rect.x + icon_size + 10, rect.y + rect.h / 2 - 8,
                   label, axui_style(colour, 1));
}

