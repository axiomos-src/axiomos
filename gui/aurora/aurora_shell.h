/*
 * AxiomOS - Aurora shell surfaces.
 *
 * The shell is the furniture around the windows: a bottom bar, a launcher, an
 * action panel and a notification stack.  All four are drawn with the same
 * material and motion primitives as the controls, so nothing in the shell
 * invents its own radius or shadow.
 *
 * The bar is centred rather than left-aligned, because the launcher and the
 * action panel both open from the middle of the screen and a centred bar makes
 * that motion read as one gesture.  Auto-hide keeps a 2 px reveal strip so a
 * hidden bar can still be summoned with the pointer.
 */

#ifndef AXIOM_AURORA_SHELL_H
#define AXIOM_AURORA_SHELL_H

#include <axiom/types.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXAU_BAR_HEIGHT        48
#define AXAU_BAR_ICON          32
#define AXAU_LAUNCHER_WIDTH    560
#define AXAU_LAUNCHER_HEIGHT   560
#define AXAU_ACTION_WIDTH      360
#define AXAU_ACTION_HEIGHT     400
#define AXAU_NOTIFY_MAX        8
#define AXAU_TILE_MAX          6
#define AXAU_RECENT_MAX        6
#define AXAU_LAUNCHER_APPS     24

typedef struct axau_shell_app {
    char   name[32];
    char   glyph[4];
    u32    accent;
    bool_t pinned;
    bool_t running;
    u32    launches;
} axau_shell_app_t;

typedef struct axau_action_tile {
    gui_rect_t rect;
    char       label[20];
    char       glyph[4];
    bool_t     on;
    bool_t     hover;
    i32        anim;
} axau_action_tile_t;

typedef struct axau_shell_notification {
    char       title[48];
    char       body[96];
    char       source[20];
    i32        level;
    u64        timestamp_ns;
    bool_t     unread;
    i32        anim;
} axau_shell_notification_t;

typedef struct axau_shell {
    i32 width;
    i32 height;
    gui_rect_t bar;
    gui_rect_t launcher_button;
    gui_rect_t search_button;
    gui_rect_t action_button;
    gui_rect_t clock_rect;
    gui_rect_t launcher_rect;
    gui_rect_t action_rect;

    bool_t bar_visible;
    bool_t bar_auto_hide;
    i32    bar_reveal;

    bool_t launcher_open;
    i32    launcher_anim;
    char   search[48];
    bool_t search_active;
    i32    selected_result;
    i32    hover_app;
    axau_shell_app_t apps[AXAU_LAUNCHER_APPS];
    i32    app_count;
    char   user_name[24];

    bool_t action_open;
    i32    action_anim;
    axau_action_tile_t tiles[AXAU_TILE_MAX];
    i32    tile_count;
    i32    volume;
    i32    brightness;
    i32    battery;
    bool_t charging;
    bool_t vpn;

    axau_shell_notification_t notifications[AXAU_NOTIFY_MAX];
    i32    notification_count;
    i32    hover_notification;

    u8     hour;
    u8     minute;
    char   date_text[24];
    bool_t night_light;
    i32    action_close_timer;
    i32    frame;
    bool_t initialised;
} axau_shell_t;

void axau_shell_init(axau_shell_t *shell, i32 width, i32 height);
void axau_shell_tick(axau_shell_t *shell, i32 dt_ms);
void axau_shell_paint(gui_surface_t *surface, axau_shell_t *shell);
void axau_shell_paint_overlays(gui_surface_t *surface, axau_shell_t *shell);

i32  axau_shell_pointer_move(axau_shell_t *shell, i32 x, i32 y);
i32  axau_shell_pointer_down(axau_shell_t *shell, i32 x, i32 y, u32 buttons);
i32  axau_shell_pointer_up(axau_shell_t *shell, i32 x, i32 y, u32 buttons);
i32  axau_shell_key(axau_shell_t *shell, char key);

void axau_shell_set_clock(axau_shell_t *shell, u8 hour, u8 minute,
                          const char *date);
void axau_shell_add_app(axau_shell_t *shell, const char *name, const char *glyph,
                        u32 accent, bool_t pinned);
i32  axau_shell_find_app(const axau_shell_t *shell, const char *name);
void axau_shell_launch(axau_shell_t *shell, i32 index);
void axau_shell_notify(axau_shell_t *shell, const char *source,
                       const char *title, const char *body, i32 level);
void axau_shell_dismiss(axau_shell_t *shell, i32 index);
void axau_shell_clear_notifications(axau_shell_t *shell);
i32  axau_shell_unread(const axau_shell_t *shell);

void axau_shell_open_launcher(axau_shell_t *shell);
void axau_shell_close_launcher(axau_shell_t *shell);
void axau_shell_open_actions(axau_shell_t *shell);
void axau_shell_close_actions(axau_shell_t *shell);
bool_t axau_shell_any_overlay(const axau_shell_t *shell);

void axau_shell_dump(const axau_shell_t *shell);
i32  axau_shell_selftest(void);

#endif /* AXIOM_AURORA_SHELL_H */
