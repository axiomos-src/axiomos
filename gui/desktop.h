/*
 * AxiomOS - desktop shell interface.
 */

#ifndef AXIOM_GUI_DESKTOP_H
#define AXIOM_GUI_DESKTOP_H

#include <axiom/types.h>
#include <gui.h>

#define DESK_ICON_MAX 24
#define DESK_NOTIFY_MAX 5

typedef struct desk_icon {
    const char *label;
    const char *glyph;
    i32         id;
    gui_rect_t  rect;
    bool_t      hover;
    bool_t      pinned;
} desk_icon_t;

typedef struct desktop {
    gui_rect_t  bounds;
    desk_icon_t icons[DESK_ICON_MAX];
    i32         icon_count;
    bool_t      taskbar_on_top;
    bool_t      dock_visible;
    bool_t      show_seconds;
    char        clock_text[16];
    char        date_text[32];
    i32         hover_icon;
    i32         pressed_icon;
    i32         battery_percent;
    bool_t      battery_charging;
    bool_t      network_up;
    bool_t      muted;
    i32         volume;
    u32         frame;
} desktop_t;

void       desktop_init(desktop_t *desktop, gui_rect_t bounds);
gui_rect_t desktop_taskbar_rect(const desktop_t *desktop);
gui_rect_t desktop_workspace_rect(const desktop_t *desktop);
gui_rect_t desktop_dock_rect(const desktop_t *desktop);
i32        desktop_add_icon(desktop_t *desktop, const char *label, const char *glyph,
                            i32 id, bool_t pinned);
void       desktop_layout_icons(desktop_t *desktop, i32 columns);
void       desktop_paint_wallpaper(gui_surface_t *surface, const desktop_t *desktop,
                                   bool_t stars);
void       desktop_paint_taskbar(gui_surface_t *surface, const desktop_t *desktop,
                                 const char *active_title);
void       desktop_paint_dock(gui_surface_t *surface, const desktop_t *desktop,
                              const char *const *labels);
i32        desktop_hit(const desktop_t *desktop, i32 x, i32 y);
i32        desktop_event(desktop_t *desktop, i32 x, i32 y, u8 buttons, bool_t pressed);
void       desktop_set_clock(desktop_t *desktop, const char *time_text,
                             const char *date_text);
void       desktop_set_battery(desktop_t *desktop, i32 percent, bool_t charging);
void       desktop_set_network(desktop_t *desktop, bool_t up);
void       desktop_set_volume(desktop_t *desktop, i32 volume, bool_t muted);
void       desktop_tick(desktop_t *desktop);

typedef struct desk_notification {
    char   title[48];
    char   body[128];
    i32    life;
    i32    total;
    i32    level;
    bool_t used;
} desk_notification_t;

typedef struct desk_notifications {
    desk_notification_t items[DESK_NOTIFY_MAX];
    i32                 count;
} desk_notifications_t;

void desktop_notify_init(desk_notifications_t *center);
void desktop_notify_push(desk_notifications_t *center, const char *title,
                         const char *body, i32 level);
void desktop_notify_tick(desk_notifications_t *center, i32 frames);
void desktop_notify_paint(gui_surface_t *surface, const desk_notifications_t *center,
                          gui_rect_t corner);

#endif /* AXIOM_GUI_DESKTOP_H */
