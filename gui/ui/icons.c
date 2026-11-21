/*
 * AxiomOS - vector icon set.
 *
 * Icons are drawn from primitives rather than shipped as bitmaps: a 24 px
 * icon stays crisp at any size, and a coloured theme simply passes its own
 * ink.  Every icon is defined inside a normalised 24 by 24 grid and then
 * scaled to the target rect, so the same drawing routine produces toolbar
 * glyphs at 16 px and empty-state glyphs at 64 px.
 *
 * Geometry is expressed in grid units with fractions, converted to pixels
 * once per call.  Nothing here touches the heap.
 */

#include <axiom/lib.h>
#include <gui.h>
#include "ui.h"

/* ------------------------------------------------------------------ */
/* Scaling helpers                                                     */
/* ------------------------------------------------------------------ */

typedef struct icon_grid {
    gui_surface_t *surface;
    i32            x;        /* pixel origin */
    i32            y;
    i32            size;     /* pixel size of the 24 unit square */
    u32            ink;
} icon_grid_t;

/* Convert grid coordinates; the grid origin is the icon's top-left corner. */
static i32 gx(const icon_grid_t *grid, i32 unit)
{
    return grid->x + (unit * grid->size) / 24;
}

static i32 gy(const icon_grid_t *grid, i32 unit)
{
    return grid->y + (unit * grid->size) / 24;
}

/* Stroke width scaled with the icon, never thinner than one pixel. */
static i32 gw(const icon_grid_t *grid, i32 units)
{
    i32 width = (units * grid->size) / 24;
    return width < 1 ? 1 : width;
}

/* A rectangle in grid units, converted to pixels. */
static gui_rect_t grect(const icon_grid_t *grid, i32 x, i32 y, i32 w, i32 h)
{
    i32 x0 = gx(grid, x);
    i32 y0 = gy(grid, y);
    i32 x1 = gx(grid, x + w);
    i32 y1 = gy(grid, y + h);
    return gui_rect(x0, y0, x1 - x0, y1 - y0);
}

static void gline(const icon_grid_t *grid, i32 x0, i32 y0, i32 x1, i32 y1,
                  i32 width_units)
{
    axui_line_aa(grid->surface, gx(grid, x0), gy(grid, y0), gx(grid, x1),
                 gy(grid, y1), grid->ink, (u8)gw(grid, width_units));
}

static void gfill(const icon_grid_t *grid, i32 x, i32 y, i32 w, i32 h)
{
    gui_fill_rect(grid->surface, grect(grid, x, y, w, h), grid->ink);
}

static void gbox(const icon_grid_t *grid, i32 x, i32 y, i32 w, i32 h, i32 radius,
                 i32 border_units)
{
    gui_rect_t rect = grect(grid, x, y, w, h);
    axui_round_rect_border(grid->surface, rect, (radius * grid->size) / 24,
                           gw(grid, border_units), grid->ink);
}

static void gcircle(const icon_grid_t *grid, i32 cx, i32 cy, i32 radius,
                    bool_t outline, i32 width_units)
{
    i32 r = (radius * grid->size) / 24;
    if (outline) {
        axui_circle_outline(grid->surface, gx(grid, cx), gy(grid, cy), r,
                            gw(grid, width_units), grid->ink);
    } else {
        axui_circle(grid->surface, gx(grid, cx), gy(grid, cy), r, grid->ink);
    }
}

/* ------------------------------------------------------------------ */
/* Individual icons                                                    */
/* ------------------------------------------------------------------ */

static void icon_terminal(const icon_grid_t *grid)
{
    gbox(grid, 2, 4, 20, 16, 2, 1);
    gline(grid, 6, 9, 10, 12, 2);
    gline(grid, 10, 12, 6, 15, 2);
    gline(grid, 12, 15, 18, 15, 2);
}

static void icon_folder(const icon_grid_t *grid)
{
    /* Tab on the left, body across the bottom: two shapes rather than an
     * outline so it reads at 16 px. */
    gbox(grid, 2, 6, 8, 4, 1, 1);
    gbox(grid, 2, 8, 20, 12, 2, 1);
    gline(grid, 4, 12, 20, 12, 1);
}

static void icon_folder_open(const icon_grid_t *grid)
{
    gbox(grid, 2, 6, 8, 4, 1, 1);
    gline(grid, 2, 9, 22, 9, 1);
    gline(grid, 3, 20, 8, 11, 1);
    gline(grid, 8, 11, 23, 11, 1);
    gline(grid, 23, 11, 18, 20, 1);
    gline(grid, 18, 20, 3, 20, 1);
}

static void icon_file(const icon_grid_t *grid)
{
    gbox(grid, 4, 2, 16, 20, 2, 1);
    /* Folded corner. */
    gline(grid, 14, 2, 20, 8, 1);
    gline(grid, 14, 2, 14, 8, 1);
    gline(grid, 14, 8, 20, 8, 1);
    gline(grid, 7, 12, 17, 12, 1);
    gline(grid, 7, 16, 17, 16, 1);
}

static void icon_disk(const icon_grid_t *grid)
{
    gbox(grid, 3, 3, 18, 18, 2, 1);
    gcircle(grid, 12, 12, 5, (bool_t)1, 1);
    gcircle(grid, 12, 12, 1, (bool_t)0, 1);
}

static void icon_gear(const icon_grid_t *grid)
{
    /* Eight teeth as short radial strokes plus a ring and a hub.  Cheaper
     * than a real gear polygon and visually equivalent at small sizes. */
    i32 cx = 12;
    i32 cy = 12;
    for (i32 i = 0; i < 8; i++) {
        i32 angle = i * 45;
        i32 x0 = cx + (i32)(7 * 100) / 100;
        (void)x0;
        /* Direction vector for this tooth, quantised to the eight cardinals
         * and diagonals so no trig table is needed. */
        i32 dx = 0;
        i32 dy = 0;
        switch (i & 7) {
        case 0: dx = 1;  dy = 0;  break;
        case 1: dx = 1;  dy = 1;  break;
        case 2: dx = 0;  dy = 1;  break;
        case 3: dx = -1; dy = 1;  break;
        case 4: dx = -1; dy = 0;  break;
        case 5: dx = -1; dy = -1; break;
        case 6: dx = 0;  dy = -1; break;
        default: dx = 1; dy = -1; break;
        }
        (void)angle;
        gline(grid, cx + dx * 6, cy + dy * 6, cx + dx * 10, cy + dy * 10, 2);
    }
    gcircle(grid, cx, cy, 6, (bool_t)1, 1);
    gcircle(grid, cx, cy, 2, (bool_t)1, 1);
}

static void icon_shield(const icon_grid_t *grid)
{
    /* Shield outline: a rounded head and a pointed body. */
    gline(grid, 4, 4, 20, 4, 1);
    gline(grid, 4, 4, 4, 13, 1);
    gline(grid, 20, 4, 20, 13, 1);
    gline(grid, 4, 13, 12, 21, 1);
    gline(grid, 20, 13, 12, 21, 1);
    gline(grid, 12, 8, 12, 14, 2);
    gline(grid, 9, 11, 15, 11, 2);
}

static void icon_chip(const icon_grid_t *grid)
{
    gbox(grid, 6, 6, 12, 12, 1, 1);
    /* Four pins per side. */
    for (i32 i = 0; i < 4; i++) {
        i32 offset = 8 + i * 3;
        gline(grid, offset, 2, offset, 6, 1);
        gline(grid, offset, 18, offset, 22, 1);
        gline(grid, 2, offset, 6, offset, 1);
        gline(grid, 18, offset, 22, offset, 1);
    }
}

static void icon_power(const icon_grid_t *grid)
{
    axui_arc(grid->surface, gx(grid, 12), gy(grid, 12),
             (10 * grid->size) / 24, 300, 240, (u8)gw(grid, 1), grid->ink);
    gline(grid, 12, 3, 12, 11, 2);
}

static void icon_restart(const icon_grid_t *grid)
{
    axui_arc(grid->surface, gx(grid, 12), gy(grid, 12),
             (8 * grid->size) / 24, 20, 300, (u8)gw(grid, 1), grid->ink);
    /* Arrow head at the arc start. */
    gline(grid, 17, 4, 20, 7, 1);
    gline(grid, 20, 7, 17, 9, 1);
}

static void icon_search(const icon_grid_t *grid)
{
    gcircle(grid, 10, 10, 6, (bool_t)1, 1);
    gline(grid, 15, 15, 21, 21, 2);
}

static void icon_network(const icon_grid_t *grid)
{
    /* Three stacked bars, the universal connectivity mark. */
    gline(grid, 4, 8, 20, 8, 2);
    gline(grid, 6, 13, 18, 13, 2);
    gline(grid, 9, 18, 15, 18, 2);
}

static void icon_wifi(const icon_grid_t *grid)
{
    i32 cx = 12;
    i32 cy = 19;
    for (i32 i = 1; i <= 3; i++) {
        i32 radius = 4 + i * 4;
        axui_arc(grid->surface, gx(grid, cx), gy(grid, cy),
                 (radius * grid->size) / 24, 210, 120, (u8)gw(grid, 1), grid->ink);
    }
    gcircle(grid, cx, cy, 1, (bool_t)0, 1);
}

static void icon_battery(const icon_grid_t *grid)
{
    gbox(grid, 3, 8, 16, 9, 1, 1);
    gfill(grid, 19, 10, 2, 5);
    gfill(grid, 5, 10, 8, 5);
}

static void icon_clock(const icon_grid_t *grid)
{
    gcircle(grid, 12, 12, 9, (bool_t)1, 1);
    gline(grid, 12, 6, 12, 12, 1);
    gline(grid, 12, 12, 17, 12, 1);
}

static void icon_calendar(const icon_grid_t *grid)
{
    gbox(grid, 3, 5, 18, 16, 2, 1);
    gline(grid, 3, 10, 21, 10, 1);
    gline(grid, 7, 3, 7, 7, 1);
    gline(grid, 17, 3, 17, 7, 1);
    for (i32 row = 0; row < 2; row++) {
        for (i32 column = 0; column < 4; column++) {
            gfill(grid, 6 + column * 4, 13 + row * 4, 2, 2);
        }
    }
}

static void icon_mail(const icon_grid_t *grid)
{
    gbox(grid, 2, 5, 20, 14, 2, 1);
    gline(grid, 3, 6, 12, 13, 1);
    gline(grid, 21, 6, 12, 13, 1);
}

static void icon_user(const icon_grid_t *grid)
{
    gcircle(grid, 12, 8, 4, (bool_t)1, 1);
    axui_arc(grid->surface, gx(grid, 12), gy(grid, 17),
             (7 * grid->size) / 24, 200, 140, (u8)gw(grid, 1), grid->ink);
}

static void icon_lock(const icon_grid_t *grid)
{
    gbox(grid, 5, 11, 14, 10, 2, 1);
    axui_arc(grid->surface, gx(grid, 12), gy(grid, 11),
             (6 * grid->size) / 24, 180, 180, (u8)gw(grid, 1), grid->ink);
    gcircle(grid, 12, 16, 2, (bool_t)0, 1);
}

static void icon_eye(const icon_grid_t *grid)
{
    axui_arc(grid->surface, gx(grid, 12), gy(grid, 14),
             (10 * grid->size) / 24, 210, 120, (u8)gw(grid, 1), grid->ink);
    gcircle(grid, 12, 12, 3, (bool_t)1, 1);
}

static void icon_grid(const icon_grid_t *grid)
{
    for (i32 row = 0; row < 2; row++) {
        for (i32 column = 0; column < 2; column++) {
            gbox(grid, 3 + column * 10, 3 + row * 10, 8, 8, 1, 1);
        }
    }
}

static void icon_image(const icon_grid_t *grid)
{
    gbox(grid, 2, 4, 20, 16, 1, 1);
    gcircle(grid, 8, 10, 2, (bool_t)0, 1);
    gline(grid, 4, 18, 10, 12, 1);
    gline(grid, 10, 12, 14, 16, 1);
    gline(grid, 14, 16, 18, 11, 1);
    gline(grid, 18, 11, 21, 14, 1);
}

static void icon_music(const icon_grid_t *grid)
{
    gcircle(grid, 7, 18, 3, (bool_t)1, 1);
    gline(grid, 10, 18, 10, 6, 2);
    gline(grid, 10, 6, 19, 4, 1);
    gline(grid, 19, 4, 19, 16, 1);
}

static void icon_video(const icon_grid_t *grid)
{
    gbox(grid, 2, 6, 13, 12, 2, 1);
    gline(grid, 16, 10, 22, 6, 1);
    gline(grid, 16, 14, 22, 18, 1);
    gline(grid, 16, 10, 16, 14, 1);
}

static void icon_code(const icon_grid_t *grid)
{
    gline(grid, 9, 7, 3, 12, 1);
    gline(grid, 3, 12, 9, 17, 1);
    gline(grid, 15, 7, 21, 12, 1);
    gline(grid, 21, 12, 15, 17, 1);
    gline(grid, 13, 5, 11, 19, 1);
}

static void icon_trash(const icon_grid_t *grid)
{
    gbox(grid, 6, 7, 12, 14, 2, 1);
    gline(grid, 3, 7, 21, 7, 1);
    gline(grid, 9, 4, 15, 4, 1);
    gline(grid, 10, 11, 10, 18, 1);
    gline(grid, 14, 11, 14, 18, 1);
}

static void icon_plus(const icon_grid_t *grid)
{
    gline(grid, 12, 4, 12, 20, 2);
    gline(grid, 4, 12, 20, 12, 2);
}

static void icon_close(const icon_grid_t *grid)
{
    gline(grid, 6, 6, 18, 18, 2);
    gline(grid, 18, 6, 6, 18, 2);
}

static void icon_check(const icon_grid_t *grid)
{
    gline(grid, 4, 12, 10, 18, 2);
    gline(grid, 10, 18, 20, 6, 2);
}

static void icon_chevron_left(const icon_grid_t *grid)
{
    gline(grid, 15, 5, 7, 12, 2);
    gline(grid, 7, 12, 15, 19, 2);
}

static void icon_chevron_right(const icon_grid_t *grid)
{
    gline(grid, 9, 5, 17, 12, 2);
    gline(grid, 17, 12, 9, 19, 2);
}

static void icon_chevron_down(const icon_grid_t *grid)
{
    gline(grid, 5, 9, 12, 16, 2);
    gline(grid, 12, 16, 19, 9, 2);
}

static void icon_arrow_up(const icon_grid_t *grid)
{
    gline(grid, 12, 20, 12, 4, 2);
    gline(grid, 6, 10, 12, 4, 2);
    gline(grid, 18, 10, 12, 4, 2);
}

static void icon_arrow_down(const icon_grid_t *grid)
{
    gline(grid, 12, 4, 12, 20, 2);
    gline(grid, 6, 14, 12, 20, 2);
    gline(grid, 18, 14, 12, 20, 2);
}

static void icon_download(const icon_grid_t *grid)
{
    gline(grid, 12, 3, 12, 15, 2);
    gline(grid, 6, 10, 12, 16, 2);
    gline(grid, 18, 10, 12, 16, 2);
    gline(grid, 3, 20, 21, 20, 2);
}

static void icon_refresh(const icon_grid_t *grid)
{
    axui_arc(grid->surface, gx(grid, 12), gy(grid, 12),
             (8 * grid->size) / 24, 30, 300, (u8)gw(grid, 1), grid->ink);
    gcircle(grid, 12, 12, 2, (bool_t)1, 1);
}

static void icon_warning(const icon_grid_t *grid)
{
    gline(grid, 12, 3, 21, 20, 1);
    gline(grid, 21, 20, 3, 20, 1);
    gline(grid, 3, 20, 12, 3, 1);
    gline(grid, 12, 9, 12, 15, 2);
    gcircle(grid, 12, 18, 1, (bool_t)0, 1);
}

static void icon_info(const icon_grid_t *grid)
{
    gcircle(grid, 12, 12, 9, (bool_t)1, 1);
    gline(grid, 12, 6, 12, 8, 2);
    gline(grid, 12, 11, 12, 18, 2);
}

static void icon_star(const icon_grid_t *grid)
{
    /* Five-pointed star approximated by ten strokes from the centre. */
    i32 cx = 12;
    i32 cy = 12;
    const i32 points[10][2] = {
        { 12,  2 }, { 15,  9 }, { 22,  9 }, { 16, 13 }, { 19, 21 },
        { 12, 16 }, {  5, 21 }, {  8, 13 }, {  2,  9 }, {  9,  9 },
    };
    for (i32 i = 0; i < 10; i++) {
        i32 j = (i + 1) % 10;
        gline(grid, points[i][0], points[i][1], points[j][0], points[j][1], 1);
    }
    (void)cx;
    (void)cy;
}

static void icon_play(const icon_grid_t *grid)
{
    axui_fill_triangle(grid->surface, axui_point(gx(grid, 7), gy(grid, 4)),
                       axui_point(gx(grid, 19), gy(grid, 12)),
                       axui_point(gx(grid, 7), gy(grid, 20)), grid->ink);
}

static void icon_pause(const icon_grid_t *grid)
{
    gfill(grid, 7, 5, 4, 14);
    gfill(grid, 14, 5, 4, 14);
}

static void icon_stop(const icon_grid_t *grid)
{
    gfill(grid, 6, 6, 12, 12);
}

static void icon_volume(const icon_grid_t *grid)
{
    axui_fill_triangle(grid->surface, axui_point(gx(grid, 4), gy(grid, 9)),
                       axui_point(gx(grid, 9), gy(grid, 9)),
                       axui_point(gx(grid, 9), gy(grid, 15)), grid->ink);
    gfill(grid, 4, 9, 1, 6);
    axui_arc(grid->surface, gx(grid, 10), gy(grid, 12),
             (5 * grid->size) / 24, 250, 140, (u8)gw(grid, 1), grid->ink);
}

static void icon_camera(const icon_grid_t *grid)
{
    gbox(grid, 2, 7, 20, 13, 2, 1);
    gcircle(grid, 12, 13, 4, (bool_t)1, 1);
    gline(grid, 8, 7, 9, 5, 1);
    gline(grid, 9, 5, 14, 5, 1);
    gline(grid, 14, 5, 15, 7, 1);
}

static void icon_link(const icon_grid_t *grid)
{
    /* Two interlocking loops drawn as arcs with cut ends. */
    axui_arc(grid->surface, gx(grid, 8), gy(grid, 12),
             (4 * grid->size) / 24, 90, 180, (u8)gw(grid, 1), grid->ink);
    axui_arc(grid->surface, gx(grid, 16), gy(grid, 12),
             (4 * grid->size) / 24, 270, 180, (u8)gw(grid, 1), grid->ink);
    gline(grid, 8, 12, 16, 12, 1);
}

static void icon_home(const icon_grid_t *grid)
{
    gline(grid, 3, 12, 12, 4, 1);
    gline(grid, 12, 4, 21, 12, 1);
    gbox(grid, 6, 12, 12, 9, 1, 1);
}

static void icon_monitor(const icon_grid_t *grid)
{
    gbox(grid, 2, 4, 20, 13, 1, 1);
    gline(grid, 9, 20, 15, 20, 1);
    gline(grid, 12, 17, 12, 20, 1);
}

static void icon_keyboard(const icon_grid_t *grid)
{
    gbox(grid, 2, 8, 20, 9, 1, 1);
    for (i32 column = 0; column < 5; column++) {
        gfill(grid, 4 + column * 3, 10, 2, 2);
    }
    gfill(grid, 7, 14, 10, 2);
}

static void icon_cloud(const icon_grid_t *grid)
{
    axui_arc(grid->surface, gx(grid, 8), gy(grid, 12),
             (4 * grid->size) / 24, 160, 200, (u8)gw(grid, 1), grid->ink);
    axui_arc(grid->surface, gx(grid, 13), gy(grid, 11),
             (5 * grid->size) / 24, 180, 180, (u8)gw(grid, 1), grid->ink);
    gline(grid, 8, 16, 18, 16, 1);
    gline(grid, 8, 16, 8, 13, 1);
}

static void icon_filter(const icon_grid_t *grid)
{
    gline(grid, 3, 5, 21, 5, 1);
    gline(grid, 21, 5, 14, 13, 1);
    gline(grid, 14, 13, 10, 13, 1);
    gline(grid, 10, 13, 3, 5, 1);
    gline(grid, 12, 13, 12, 21, 1);
}

static void icon_more(const icon_grid_t *grid)
{
    gcircle(grid, 6, 12, 2, (bool_t)0, 1);
    gcircle(grid, 12, 12, 2, (bool_t)0, 1);
    gcircle(grid, 18, 12, 2, (bool_t)0, 1);
}

/* ------------------------------------------------------------------ */
/* Table                                                               */
/* ------------------------------------------------------------------ */

typedef void (*icon_fn_t)(const icon_grid_t *);

typedef struct icon_entry {
    const char *name;
    icon_fn_t   draw;
} icon_entry_t;

static const icon_entry_t icon_table[] = {
    { "terminal",  icon_terminal },
    { "folder",    icon_folder },
    { "folder-open", icon_folder_open },
    { "file",      icon_file },
    { "disk",      icon_disk },
    { "gear",      icon_gear },
    { "shield",    icon_shield },
    { "chip",      icon_chip },
    { "power",     icon_power },
    { "restart",   icon_restart },
    { "search",    icon_search },
    { "network",   icon_network },
    { "wifi",      icon_wifi },
    { "battery",   icon_battery },
    { "clock",     icon_clock },
    { "calendar",  icon_calendar },
    { "mail",      icon_mail },
    { "user",      icon_user },
    { "lock",      icon_lock },
    { "eye",       icon_eye },
    { "grid",      icon_grid },
    { "image",     icon_image },
    { "music",     icon_music },
    { "video",     icon_video },
    { "code",      icon_code },
    { "trash",     icon_trash },
    { "plus",      icon_plus },
    { "close",     icon_close },
    { "check",     icon_check },
    { "chevron-left",  icon_chevron_left },
    { "chevron-right", icon_chevron_right },
    { "chevron-down",  icon_chevron_down },
    { "arrow-up",   icon_arrow_up },
    { "arrow-down", icon_arrow_down },
    { "download",   icon_download },
    { "refresh",    icon_refresh },
    { "warning",    icon_warning },
    { "info",       icon_info },
    { "star",       icon_star },
    { "play",       icon_play },
    { "pause",      icon_pause },
    { "stop",       icon_stop },
    { "volume",     icon_volume },
    { "camera",     icon_camera },
    { "link",       icon_link },
    { "home",       icon_home },
    { "monitor",    icon_monitor },
    { "keyboard",   icon_keyboard },
    { "cloud",      icon_cloud },
    { "filter",     icon_filter },
    { "more",       icon_more },
};

#define ICON_COUNT ((i32)(sizeof(icon_table) / sizeof(icon_table[0])))

i32 axui_icon_count(void)
{
    return ICON_COUNT;
}

const char *axui_icon_name(i32 index)
{
    if (index < 0 || index >= ICON_COUNT) {
        return 0;
    }
    return icon_table[index].name;
}

i32 axui_icon_find(const char *name)
{
    if (!name) {
        return -1;
    }
    for (i32 i = 0; i < ICON_COUNT; i++) {
        if (ax_strcmp(icon_table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Draw one icon into a square rect.  The caller may pass a rect that is not
 * square: the smaller dimension wins and the icon is centred. */
void axui_icon_draw(gui_surface_t *surface, i32 index, gui_rect_t rect, u32 ink)
{
    if (!surface || index < 0 || index >= ICON_COUNT) {
        return;
    }
    i32 size = axui_min_i32(rect.w, rect.h);
    if (size <= 0) {
        return;
    }
    icon_grid_t grid;
    grid.surface = surface;
    grid.size = size;
    grid.x = rect.x + (rect.w - size) / 2;
    grid.y = rect.y + (rect.h - size) / 2;
    grid.ink = ink;
    icon_table[index].draw(&grid);
}

/* Convenience for named icons, used by the launcher and window chrome. */
void axui_icon_draw_named(gui_surface_t *surface, const char *name, gui_rect_t rect,
                          u32 ink)
{
    i32 index = axui_icon_find(name);
    if (index >= 0) {
        axui_icon_draw(surface, index, rect, ink);
    }
}

/* Draw an icon into a circular badge with a tinted background. */
void axui_icon_badge(gui_surface_t *surface, const char *name, gui_rect_t rect,
                     u32 ink, u32 badge_colour)
{
    axui_round_rect_aa(surface, rect, axui_min_i32(rect.w, rect.h) / 2, badge_colour);
    gui_rect_t inner = axui_rect_inset_px(rect, axui_min_i32(rect.w, rect.h) / 5);
    axui_icon_draw_named(surface, name, inner, ink);
}

/* A grid of every icon, as the icon browser shows in the about page. */
void axui_icon_dump(gui_surface_t *surface, gui_rect_t rect, i32 per_row, u32 ink)
{
    if (!surface || per_row <= 0) {
        return;
    }
    i32 cell = rect.w / per_row;
    if (cell <= 0) {
        return;
    }
    for (i32 i = 0; i < ICON_COUNT; i++) {
        i32 column = i % per_row;
        i32 row = i / per_row;
        gui_rect_t at = gui_rect(rect.x + column * cell, rect.y + row * cell,
                                 cell, cell);
        if (at.y + cell > rect.y + rect.h) {
            break;
        }
        gui_rect_t inner = axui_rect_inset_px(at, cell / 5);
        axui_icon_draw(surface, i, inner, ink);
    }
}
