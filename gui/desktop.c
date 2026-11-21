/*
 * AxiomOS - desktop shell composition.
 *
 * The desktop is painted in four layers, back to front:
 *
 *   1. wallpaper   - gradient plus a soft grid, optionally a vignette
 *   2. icons       - a flow of launcher icons on the wallpaper grid
 *   3. taskbar     - top strip: launcher, window buttons, clock, tray
 *   4. dock        - bottom centre strip of pinned applications
 *
 * This module owns that composition and nothing else: it draws into a
 * gui_surface_t and returns hit results, so the window manager can treat the
 * shell as just another surface.  Time formatting is passed in by the caller,
 * which keeps this file free of RTC dependencies.
 */

#include <axiom/lib.h>
#include <gui.h>
#include "ui.h"
#include "widgets.h"

#include "desktop.h"

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

void desktop_init(desktop_t *desktop, gui_rect_t bounds)
{
    if (!desktop) {
        return;
    }
    ax_memset(desktop, 0, sizeof(*desktop));
    desktop->bounds = bounds;
    desktop->taskbar_on_top = (bool_t)1;
    desktop->dock_visible = (bool_t)1;
    desktop->battery_percent = 62;
    desktop->volume = 55;
    desktop->hover_icon = -1;
    desktop->pressed_icon = -1;
}

/* The taskbar occupies the top 32 px; the dock the bottom 64 px. */
gui_rect_t desktop_taskbar_rect(const desktop_t *desktop)
{
    if (!desktop) {
        return gui_rect(0, 0, 0, 0);
    }
    return gui_rect(desktop->bounds.x, desktop->bounds.y, desktop->bounds.w, 32);
}

gui_rect_t desktop_workspace_rect(const desktop_t *desktop)
{
    if (!desktop) {
        return gui_rect(0, 0, 0, 0);
    }
    i32 top = desktop->taskbar_on_top ? 32 : 0;
    i32 bottom = desktop->dock_visible ? 64 : 0;
    return gui_rect(desktop->bounds.x, desktop->bounds.y + top, desktop->bounds.w,
                    desktop->bounds.h - top - bottom);
}

gui_rect_t desktop_dock_rect(const desktop_t *desktop)
{
    if (!desktop || !desktop->dock_visible) {
        return gui_rect(0, 0, 0, 0);
    }
    i32 width = desktop->icon_count * 64 + 24;
    if (width > desktop->bounds.w - 40) {
        width = desktop->bounds.w - 40;
    }
    i32 height = 56;
    return gui_rect(desktop->bounds.x + (desktop->bounds.w - width) / 2,
                    desktop->bounds.y + desktop->bounds.h - height - 6, width,
                    height);
}

i32 desktop_add_icon(desktop_t *desktop, const char *label, const char *glyph, i32 id,
                     bool_t pinned)
{
    if (!desktop || desktop->icon_count >= DESK_ICON_MAX) {
        return -1;
    }
    i32 index = desktop->icon_count++;
    desktop->icons[index].label = label;
    desktop->icons[index].glyph = glyph;
    desktop->icons[index].id = id;
    desktop->icons[index].pinned = pinned;
    return index;
}

/* Lay the launcher icons out on the workspace as a grid of 96 px cells, with
 * the tile itself inset so labels have room. */
void desktop_layout_icons(desktop_t *desktop, i32 columns)
{
    if (!desktop || columns <= 0) {
        return;
    }
    gui_rect_t workspace = desktop_workspace_rect(desktop);
    i32 cell = 96;
    i32 margin = 24;
    for (i32 i = 0; i < desktop->icon_count; i++) {
        i32 column = i % columns;
        i32 row = i / columns;
        desktop->icons[i].rect =
            gui_rect(workspace.x + margin + column * cell,
                     workspace.y + margin + row * cell, cell - 16, cell - 16);
    }
}

/* ------------------------------------------------------------------ */
/* Wallpaper                                                           */
/* ------------------------------------------------------------------ */

/* A deterministic star field, so the wallpaper is identical across frames and
 * repaints do not shimmer. */
static void wallpaper_stars(gui_surface_t *surface, gui_rect_t rect, u32 seed)
{
    if (!surface) {
        return;
    }
    u32 state = seed | 1;
    i32 count = (rect.w * rect.h) / 4000;
    if (count > 600) {
        count = 600;
    }
    for (i32 i = 0; i < count; i++) {
        /* xorshift keeps the pattern stable per seed. */
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        i32 x = rect.x + (i32)(state % (u32)(rect.w > 0 ? rect.w : 1));
        i32 y = rect.y + (i32)((state >> 8) % (u32)(rect.h > 0 ? rect.h : 1));
        u8 alpha = (u8)(40 + (state >> 24) % 120);
        u32 colour = axui_colour_alpha(0xFFFFFFFF, alpha);
        gui_fill_rect(surface, gui_rect(x, y, 1, 1), colour);
        /* The brightest few get a cross, which reads as a large star. */
        if (alpha > 140) {
            gui_fill_rect(surface, gui_rect(x + 1, y, 1, 1), colour);
            gui_fill_rect(surface, gui_rect(x, y + 1, 1, 1), colour);
        }
    }
}

/* Wallpaper: a vertical two-stop gradient, a faint perspective grid and an
 * optional star field.  Everything derives from the active theme so a theme
 * switch repaints the whole desktop coherently. */
void desktop_paint_wallpaper(gui_surface_t *surface, const desktop_t *desktop,
                             bool_t stars)
{
    if (!surface || !desktop) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    gui_rect_t rect = desktop->bounds;

    u32 top = axui_darken(theme->desktop_top, 6);
    u32 bottom = axui_lighten(theme->desktop_bottom, 4);
    axui_gradient_linear(surface, rect, 90, top, bottom);

    /* Two soft accent glows in opposite corners, clipped by the edited
     * gradient's own alpha. */
    axui_gradient_radial(surface, rect.x + rect.w / 4, rect.y + rect.h / 5,
                         rect.w / 3,
                         axui_colour_alpha(theme->accent, 26),
                         axui_colour_alpha(theme->accent, 0));
    axui_gradient_radial(surface, rect.x + (rect.w * 3) / 4,
                         rect.y + (rect.h * 4) / 5, rect.w / 4,
                         axui_colour_alpha(axui_lighten(theme->accent, 30), 22),
                         axui_colour_alpha(axui_lighten(theme->accent, 30), 0));

    if (stars) {
        wallpaper_stars(surface, rect, 0x5A17u + desktop->frame / 240);
    }

    /* A barely-there grid, 64 px cells, only in the lower third so it reads as
     * ground plane rather than wallpaper pattern. */
    gui_rect_t ground = gui_rect(rect.x, rect.y + (rect.h * 2) / 3, rect.w,
                                 rect.h / 3);
    axui_grid_pattern(surface, ground, 64, axui_colour_alpha(theme->desktop_top,
                                                             30));
    axui_vignette(surface, 26);
}

/* ------------------------------------------------------------------ */
/* Taskbar                                                             */
/* ------------------------------------------------------------------ */

/* An indicator pill: the small round status chips on the right of the bar. */
static void tray_pill(gui_surface_t *surface, gui_rect_t rect, const char *glyph,
                      u32 ink)
{
    gui_fill_rect(surface, rect, axui_colour_alpha(ink, 34));
    axui_draw_text_center(surface, rect, glyph, axui_style(ink, 1));
}

/* The taskbar: launcher button on the left, active window title centred,
 * status pills on the right.  Hit ids come back from the event function as
 * negative values so they never collide with launcher icon ids. */
void desktop_paint_taskbar(gui_surface_t *surface, const desktop_t *desktop,
                            const char *active_title)
{
    if (!surface || !desktop) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    gui_rect_t bar = desktop_taskbar_rect(desktop);

    /* Frosted strip: a solid base plus a one-pixel bottom highlight. */
    gui_fill_rect(surface, bar, axui_colour_alpha(theme->title_bg, 235));
    gui_fill_rect(surface, gui_rect(bar.x, bar.y + bar.h - 1, bar.w, 1),
                  axui_colour_alpha(theme->border, 200));

    /* Launcher button. */
    gui_rect_t launcher = gui_rect(bar.x + 6, bar.y + 4, 26, 24);
    if (desktop->hover_icon == -1) {
        gui_fill_rect(surface, launcher, axui_colour_alpha(theme->accent, 40));
    }
    axui_icon_draw_named(surface, "grid",
                         axui_rect_inset_px(launcher, 5), theme->accent);

    /* Title. */
    if (active_title) {
        gui_rect_t title = gui_rect(bar.x + 44, bar.y, bar.w - 220, bar.h);
        axui_text_style_t style = axui_style(theme->text, 1);
        style.bold = (bool_t)1;
        axui_draw_text_clip(surface, title, title.x, axui_text_baseline(title, 1),
                            active_title, style);
    }

    /* Right cluster: network, volume, battery, then the clock. */
    i32 right = bar.x + bar.w - 8;
    char number[8];

    if (desktop->clock_text[0]) {
        i32 clock_w = axui_measure_text(desktop->clock_text, 1) + 8;
        gui_rect_t clock = gui_rect(right - clock_w, bar.y, clock_w, bar.h);
        axui_text_style_t style = axui_style(theme->text, 1);
        style.bold = (bool_t)1;
        axui_draw_text_right(surface, clock, desktop->clock_text, style);
        right -= clock_w + 4;
    }

    i32 battery_w = 34;
    gui_rect_t battery = gui_rect(right - battery_w, bar.y + 6, battery_w, 20);
    gui_fill_rect(surface, battery, axui_colour_alpha(theme->text_dim, 30));
    gui_fill_rect(surface, gui_rect(battery.x + 2, battery.y + 3,
                                    (battery.w - 8) * desktop->battery_percent / 100,
                                    14),
                  desktop->battery_percent < 20 ? theme->error : theme->success);
    ax_itoa(desktop->battery_percent, number);
    (void)number;
    right -= battery_w + 4;

    gui_rect_t volume = gui_rect(right - 24, bar.y + 4, 24, 24);
    tray_pill(surface, volume, "V",
              desktop->muted ? theme->error : theme->text_dim);
    right -= 28;

    gui_rect_t net = gui_rect(right - 24, bar.y + 4, 24, 24);
    tray_pill(surface, net, "N",
              desktop->network_up ? theme->success : theme->text_dim);
    right -= 28;

    /* The separators echo the floating panels in the rest of the shell. */
    gui_fill_rect(surface, gui_rect(right - 4, bar.y + 7, 1, bar.h - 14),
                  axui_colour_alpha(theme->border, 140));
}

/* ------------------------------------------------------------------ */
/* Dock                                                                */
/* ------------------------------------------------------------------ */

void desktop_paint_dock(gui_surface_t *surface, const desktop_t *desktop,
                        const char *const *labels)
{
    if (!surface || !desktop || !desktop->dock_visible) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    gui_rect_t dock = desktop_dock_rect(desktop);

    axui_round_rect_aa(surface, dock, 14, axui_colour_alpha(theme->title_bg, 225));
    axui_round_rect_border(surface, dock, 14, 1, axui_colour_alpha(theme->border, 200));

    i32 step = 64;
    for (i32 i = 0; i < desktop->icon_count; i++) {
        if (!desktop->icons[i].pinned) {
            continue;
        }
        gui_rect_t tile = gui_rect(dock.x + 12 + i * step, dock.y + 8, 48, 40);
        if (tile.x + tile.w > dock.x + dock.w) {
            break;
        }
        bool_t hover = (bool_t)(desktop->hover_icon == i);

        /* The hovered tile lifts by two pixels and gains an accent wash. */
        if (hover) {
            tile.y -= 2;
            gui_fill_rect(surface, gui_rect(tile.x, tile.y + tile.h - 2, tile.w, 2),
                          theme->accent);
            axui_round_rect_aa(surface, gui_rect(tile.x - 2, tile.y - 2, tile.w + 4,
                                                 tile.h + 4), 10,
                               axui_colour_alpha(theme->accent, 30));
        }

        i32 icon_index = axui_icon_find(desktop->icons[i].glyph);
        if (icon_index >= 0) {
            axui_icon_draw(surface, icon_index, gui_rect(tile.x + 8, tile.y,
                                                         tile.w - 16, tile.h - 6),
                           hover ? axui_accent_text() : theme->text);
        }
        if (labels && labels[i]) {
            axui_text_style_t style = axui_style(theme->text_dim, 1);
            axui_draw_text_center(surface, gui_rect(tile.x, tile.y + tile.h,
                                                    tile.w, 12), labels[i], style);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

/* Hit ids: >= 0 means a desktop icon, -1 means the launcher button,
 * -2 the taskbar clock, -3 the tray.  Returns -100 when nothing was hit. */
i32 desktop_hit(const desktop_t *desktop, i32 x, i32 y)
{
    if (!desktop) {
        return -100;
    }
    gui_rect_t bar = desktop_taskbar_rect(desktop);
    if (axw_hit(bar, x, y)) {
        gui_rect_t launcher = gui_rect(bar.x + 6, bar.y + 4, 26, 24);
        if (axw_hit(launcher, x, y)) {
            return -1;
        }
        if (x > bar.x + bar.w - 100) {
            return -2;
        }
        return -3;
    }
    for (i32 i = 0; i < desktop->icon_count; i++) {
        if (axw_hit(desktop->icons[i].rect, x, y)) {
            return i;
        }
    }
    return -100;
}

/* Track hover/press state and report a click on release. */
i32 desktop_event(desktop_t *desktop, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!desktop) {
        return -100;
    }
    i32 hit = desktop_hit(desktop, x, y);
    desktop->hover_icon = hit;
    if (pressed) {
        desktop->pressed_icon = hit;
        return -100;
    }
    if (desktop->pressed_icon == hit && hit != -100) {
        desktop->pressed_icon = -1;
        return hit;
    }
    desktop->pressed_icon = -1;
    (void)buttons;
    return -100;
}

void desktop_set_clock(desktop_t *desktop, const char *time_text, const char *date_text)
{
    if (!desktop) {
        return;
    }
    ax_strlcpy(desktop->clock_text, time_text ? time_text : "", sizeof(desktop->clock_text));
    ax_strlcpy(desktop->date_text, date_text ? date_text : "", sizeof(desktop->date_text));
}

/* Status updates come from the kernel's device polls; the desktop only keeps
 * the numbers it paints. */
void desktop_set_battery(desktop_t *desktop, i32 percent, bool_t charging)
{
    if (!desktop) {
        return;
    }
    desktop->battery_percent = axui_clamp_i32(percent, 0, 100);
    desktop->battery_charging = charging;
}

void desktop_set_network(desktop_t *desktop, bool_t up)
{
    if (desktop) {
        desktop->network_up = up;
    }
}

void desktop_set_volume(desktop_t *desktop, i32 volume, bool_t muted)
{
    if (!desktop) {
        return;
    }
    desktop->volume = axui_clamp_i32(volume, 0, 100);
    desktop->muted = muted;
}

void desktop_tick(desktop_t *desktop)
{
    if (desktop) {
        desktop->frame++;
    }
}

/* ------------------------------------------------------------------ */
/* Notification centre                                                 */
/* ------------------------------------------------------------------ */



void desktop_notify_init(desk_notifications_t *center)
{
    if (center) {
        ax_memset(center, 0, sizeof(*center));
    }
}

void desktop_notify_push(desk_notifications_t *center, const char *title,
                         const char *body, i32 level)
{
    if (!center) {
        return;
    }
    i32 slot = center->count;
    if (slot >= DESK_NOTIFY_MAX) {
        /* Evict the oldest by shifting everything down one slot. */
        for (i32 i = 0; i < DESK_NOTIFY_MAX - 1; i++) {
            center->items[i] = center->items[i + 1];
        }
        slot = DESK_NOTIFY_MAX - 1;
    } else {
        center->count++;
    }
    desk_notification_t *item = &center->items[slot];
    ax_strlcpy(item->title, title ? title : "", sizeof(item->title));
    ax_strlcpy(item->body, body ? body : "", sizeof(item->body));
    item->level = level;
    item->life = 240;
    item->total = item->life;
    item->used = (bool_t)1;
}

/* Age every notification; expired ones are removed by shifting the array. */
void desktop_notify_tick(desk_notifications_t *center, i32 frames)
{
    if (!center) {
        return;
    }
    for (i32 i = 0; i < center->count; i++) {
        if (center->items[i].life > 0) {
            center->items[i].life -= frames;
        }
    }
    /* Compact any that expired. */
    i32 write = 0;
    for (i32 i = 0; i < center->count; i++) {
        if (center->items[i].life > 0) {
            if (write != i) {
                center->items[write] = center->items[i];
            }
            write++;
        }
    }
    center->count = write;
}

void desktop_notify_paint(gui_surface_t *surface, const desk_notifications_t *center,
                          gui_rect_t corner)
{
    if (!surface || !center) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    i32 width = 300;
    i32 height = 64;
    for (i32 i = 0; i < center->count; i++) {
        const desk_notification_t *item = &center->items[i];
        gui_rect_t rect = gui_rect(corner.x - width, corner.y + i * (height + 8),
                                   width, height);
        if (rect.y + height > corner.y + corner.h) {
            break;
        }
        u32 accent = item->level == 2 ? theme->error
                     : item->level == 1 ? theme->warning
                                       : theme->accent;

        axui_round_rect_aa(surface, rect, 8, axui_colour_alpha(theme->title_bg, 240));
        axui_round_rect_border(surface, rect, 8, 1, axui_colour_alpha(theme->border, 180));
        gui_fill_rect(surface, gui_rect(rect.x + 10, rect.y + 10, 4, rect.h - 20),
                      accent);

        axui_text_style_t title_style = axui_style(theme->text, 1);
        title_style.bold = (bool_t)1;
        axui_draw_text_clip(surface, rect, rect.x + 24, rect.y + 12, item->title,
                            title_style);
        axui_text_style_t body_style = axui_style(theme->text_dim, 1);
        axui_draw_text_wrapped(surface,
                               gui_rect(rect.x + 24, rect.y + 30, rect.w - 36,
                                        rect.h - 36),
                               item->body, body_style);

        /* A thin lifetime bar drains as the notification ages. */
        i32 remaining = item->total > 0 ? (rect.w - 20) * item->life / item->total : 0;
        gui_fill_rect(surface, gui_rect(rect.x + 10, rect.y + rect.h - 4, remaining,
                                        2), axui_colour_alpha(accent, 160));
    }
}
