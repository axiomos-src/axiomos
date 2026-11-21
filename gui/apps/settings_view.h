/*
 * AxiomOS - Settings page interface.
 */

#ifndef AXIOM_APP_SETTINGS_VIEW_H
#define AXIOM_APP_SETTINGS_VIEW_H

#include <axiom/types.h>
#include <gui.h>
#include "ui.h"
#include "widgets.h"

typedef struct settings_view {
    gui_rect_t     bounds;
    i32            page;

    axw_tabs_t     tabs;
    axw_focus_t    focus;

    /* Appearance */
    gui_rect_t     card_appearance;
    axw_list_t     theme_list;
    i32            theme_index;
    axw_switch_t   reduce_motion;
    axw_switch_t   rounded_corners;
    axw_switch_t   show_details;
    axw_slider_t   animation_speed;

    /* Display */
    gui_rect_t     card_display;
    axw_list_t     resolution_list;
    i32            resolution_index;
    axw_slider_t   ui_scale;
    axw_switch_t   night_light;
    axw_switch_t   wallpaper_stars;

    /* Network */
    gui_rect_t     card_network;
    axw_list_t     adapter_list;
    axw_switch_t   dhcp;
    axw_text_field_t proxy_field;
    const char    *proxy_value;

    /* Security */
    gui_rect_t     card_security;
    axw_switch_t   hvci;
    axw_switch_t   driver_signing;
    axw_switch_t   telemetry;
    axw_radio_group_t security_level;
    i32            security_policy;

    /* Footer */
    axw_button_t   apply;
    axw_button_t   reset;
} settings_view_t;

void settings_init(settings_view_t *view, gui_rect_t bounds);
void settings_paint(gui_surface_t *surface, settings_view_t *view);
i32  settings_event(settings_view_t *view, i32 x, i32 y, u8 buttons, bool_t pressed);
u32  settings_key(settings_view_t *view, char key, u8 scancode, bool_t shift);
void settings_summary(const settings_view_t *view, char *buffer, usize cap);

#endif /* AXIOM_APP_SETTINGS_VIEW_H */
