/*
 * AxiomOS - Settings page.
 *
 * The settings window is built from the widget layer rather than bespoke
 * drawing code: a tab strip selects one of four pages, and each page is a
 * column of cards holding the controls that belong to it.  This file owns the
 * layout and the event routing; the controls themselves live in
 * gui/ui/widget_*.c.
 *
 * Pages:
 *   Appearance  - theme, accent, density, animation speed
 *   Display     - resolution, scale, night light, wallpaper stars
 *   Network     - adapter list, DHCP toggle, proxy field
 *   Security    - HVCI state, driver signing, telemetry
 *
 * The page keeps every control in one struct so a redraw is a walk over
 * members, and so the event handler can be a simple region test.
 */

#include <axiom/lib.h>
#include <gui.h>
#include "ui.h"
#include "widgets.h"
#include "settings_view.h"

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

void settings_init(settings_view_t *view, gui_rect_t bounds)
{
    if (!view) {
        return;
    }
    ax_memset(view, 0, sizeof(*view));
    view->bounds = bounds;
    view->page = 0;

    /* Content column: 560 px wide, centred, with 28 px side padding. */
    i32 width = bounds.w - 56;
    if (width > 640) {
        width = 640;
    }
    i32 left = bounds.x + (bounds.w - width) / 2;
    gui_rect_t content = gui_rect(left, bounds.y + 56, width, bounds.h - 96);

    /* Tab strip across the top of the content column. */
    gui_rect_t tabs_rect = gui_rect(left, bounds.y + 12, width, 34);
    axw_tabs_init(&view->tabs, tabs_rect, width / 4);
    axw_tabs_add(&view->tabs, "Appearance");
    axw_tabs_add(&view->tabs, "Display");
    axw_tabs_add(&view->tabs, "Network");
    axw_tabs_add(&view->tabs, "Security");

    /* Page 0: appearance. */
    gui_rect_t card0 = gui_rect(content.x, content.y, content.w, 200);
    view->card_appearance = card0;
    axw_list_init(&view->theme_list,
                  gui_rect(card0.x + 20, card0.y + 40, 260, 148), 26);
    for (i32 i = 0; i < axui_theme_count(); i++) {
        axw_list_add(&view->theme_list, axui_theme_name(i), 0, 0);
    }
    view->theme_list.selected = 0;

    gui_rect_t switches = gui_rect(card0.x + 300, card0.y + 40, 300, 148);
    axw_switch_init(&view->reduce_motion,
                    gui_rect(switches.x, switches.y, 300, 26), "Reduce motion",
                    (bool_t)0);
    axw_switch_init(&view->rounded_corners,
                    gui_rect(switches.x, switches.y + 34, 300, 26),
                    "Rounded corners", (bool_t)1);
    axw_switch_init(&view->show_details,
                    gui_rect(switches.x, switches.y + 68, 300, 26),
                    "Dense lists", (bool_t)0);
    axw_slider_init(&view->animation_speed,
                    gui_rect(switches.x, switches.y + 106, 260, 20), 0, 100, 60);
    view->animation_speed.show_value = (bool_t)1;

    /* Page 1: display. */
    gui_rect_t card1 = gui_rect(content.x, content.y + 212, content.w, 180);
    view->card_display = card1;
    axw_list_init(&view->resolution_list,
                  gui_rect(card1.x + 20, card1.y + 40, 280, 128), 26);
    axw_list_add(&view->resolution_list, "1024 x 768", "native", 0);
    axw_list_add(&view->resolution_list, "1280 x 800", "letterbox", 0);
    axw_list_add(&view->resolution_list, "800 x 600", "scaled", 0);
    view->resolution_list.selected = 0;

    axw_slider_init(&view->ui_scale, gui_rect(card1.x + 320, card1.y + 44, 240, 20),
                    100, 200, 100);
    view->ui_scale.step = 25;
    view->ui_scale.show_value = (bool_t)1;
    axw_switch_init(&view->night_light,
                    gui_rect(card1.x + 320, card1.y + 84, 240, 26), "Night light",
                    (bool_t)0);
    axw_switch_init(&view->wallpaper_stars,
                    gui_rect(card1.x + 320, card1.y + 118, 240, 26),
                    "Star field", (bool_t)1);

    /* Page 2: network. */
    gui_rect_t card2 = gui_rect(content.x, content.y + 212, content.w, 220);
    view->card_network = card2;
    axw_list_init(&view->adapter_list,
                  gui_rect(card2.x + 20, card2.y + 40, content.w - 40, 96), 26);
    axw_list_add(&view->adapter_list, "e1000  link up  1000 Mb/s",
                 "00:0c:29:1a:2b:3c", 0);
    axw_list_add(&view->adapter_list, "loopback", "127.0.0.1", 0);
    view->adapter_list.selected = 0;
    view->adapter_list.show_detail = (bool_t)1;

    axw_switch_init(&view->dhcp, gui_rect(card2.x + 20, card2.y + 148, 260, 26),
                    "DHCP", (bool_t)1);
    view->proxy_value = "http://proxy.local:3128";
    axw_text_field_init(&view->proxy_field,
                        gui_rect(card2.x + 20, card2.y + 180, content.w - 40, 28),
                        "proxy URL");
    axw_text_field_set(&view->proxy_field, view->proxy_value);

    /* Page 3: security. */
    gui_rect_t card3 = gui_rect(content.x, content.y + 212, content.w, 220);
    view->card_security = card3;
    axw_switch_init(&view->hvci,
                    gui_rect(card3.x + 20, card3.y + 44, 260, 26),
                    "HVCI (hypervisor code integrity)", (bool_t)1);
    axw_switch_init(&view->driver_signing,
                    gui_rect(card3.x + 20, card3.y + 78, 260, 26),
                    "Require signed drivers", (bool_t)1);
    axw_switch_init(&view->telemetry,
                    gui_rect(card3.x + 20, card3.y + 112, 260, 26),
                    "Send diagnostics", (bool_t)0);

    axw_radio_group_init(&view->security_level,
                         gui_rect(card3.x + 300, card3.y + 44, 260, 96), 3,
                         (bool_t)0, 8);
    axw_radio_group_label(&view->security_level, 0, "Enforce");
    axw_radio_group_label(&view->security_level, 1, "Audit only");
    axw_radio_group_label(&view->security_level, 2, "Off");

    /* Footer buttons. */
    axw_button_init(&view->apply, gui_rect(bounds.x + bounds.w - 200, bounds.y + bounds.h - 44,
                                           84, 28), "Apply");
    view->apply.primary = (bool_t)1;
    axw_button_init(&view->reset, gui_rect(bounds.x + bounds.w - 108,
                                           bounds.y + bounds.h - 44, 84, 28),
                    "Reset");

    axw_focus_init(&view->focus);
    axw_focus_add(&view->focus, &view->theme_list);
    axw_focus_add(&view->focus, &view->reduce_motion);
    axw_focus_add(&view->focus, &view->rounded_corners);
    axw_focus_add(&view->focus, &view->animation_speed);
    axw_focus_add(&view->focus, &view->resolution_list);
    axw_focus_add(&view->focus, &view->ui_scale);
    axw_focus_add(&view->focus, &view->adapter_list);
    axw_focus_add(&view->focus, &view->proxy_field);
    axw_focus_add(&view->focus, &view->hvci);
    axw_focus_add(&view->focus, &view->security_level);
    axw_focus_add(&view->focus, &view->apply);
    axw_focus_add(&view->focus, &view->reset);
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

/* A section header above each card. */
static void card_title(gui_surface_t *surface, gui_rect_t card, const char *title,
                       const char *subtitle)
{
    const gui_theme_t *theme = axui_theme_current();
    axui_text_style_t style = axui_style(theme->text, 1);
    style.bold = (bool_t)1;
    axui_draw_text(surface, card.x + 20, card.y + 18, title, style);
    if (subtitle) {
        axui_text_style_t sub = axui_style(theme->text_dim, 1);
        axui_draw_text(surface, card.x + 20, card.y + 34, subtitle, sub);
    }
    (void)theme;
}

void settings_paint(gui_surface_t *surface, settings_view_t *view)
{
    if (!surface || !view) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();

    gui_fill_rect(surface, view->bounds, theme->window_bg);
    axw_tabs_paint(surface, &view->tabs);

    /* Only the selected page's cards are painted; the others keep their
     * geometry but are skipped, which is cheap and keeps layout stable. */
    switch (view->page) {
    case 0:
        axw_card_paint(surface, view->card_appearance, 10);
        card_title(surface, view->card_appearance, "Appearance",
                   "theme, motion and list density");
        axw_list_paint(surface, &view->theme_list);
        axw_switch_paint(surface, &view->reduce_motion);
        axw_switch_paint(surface, &view->rounded_corners);
        axw_switch_paint(surface, &view->show_details);
        axw_slider_paint(surface, &view->animation_speed);
        break;
    case 1:
        axw_card_paint(surface, view->card_display, 10);
        card_title(surface, view->card_display, "Display",
                   "resolution and scaling");
        axw_list_paint(surface, &view->resolution_list);
        axw_slider_paint(surface, &view->ui_scale);
        axw_switch_paint(surface, &view->night_light);
        axw_switch_paint(surface, &view->wallpaper_stars);
        break;
    case 2:
        axw_card_paint(surface, view->card_network, 10);
        card_title(surface, view->card_network, "Network",
                   "adapters and addressing");
        axw_list_paint(surface, &view->adapter_list);
        axw_switch_paint(surface, &view->dhcp);
        axw_text_field_paint(surface, &view->proxy_field);
        break;
    case 3:
        axw_card_paint(surface, view->card_security, 10);
        card_title(surface, view->card_security, "Security",
                   "integrity, signing and diagnostics");
        axw_switch_paint(surface, &view->hvci);
        axw_switch_paint(surface, &view->driver_signing);
        axw_switch_paint(surface, &view->telemetry);
        axw_radio_group_paint(surface, &view->security_level);
        break;
    default:
        break;
    }

    /* A footer hint on the left, buttons on the right. */
    axui_text_style_t hint = axui_style(theme->text_dim, 1);
    axui_draw_text(surface, view->bounds.x + 28, view->bounds.y + view->bounds.h - 30,
                   "changes apply immediately to this session", hint);

    axw_button_paint(surface, &view->apply);
    axw_button_paint(surface, &view->reset);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

/* Return value: 1 when a setting the caller must act on changed (theme,
 * resolution, HVCI), 0 otherwise.  The caller inspects the specific field. */
i32 settings_event(settings_view_t *view, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!view) {
        return 0;
    }
    u32 events = axw_tabs_event(&view->tabs, x, y, buttons, pressed);
    if (events & AXW_CHANGED) {
        view->page = view->tabs.selected;
        return 1;
    }

    switch (view->page) {
    case 0:
        if (axw_list_event(&view->theme_list, x, y, buttons, pressed) & AXW_SELECT) {
            view->theme_index = view->theme_list.selected;
            return 1;
        }
        if (axw_switch_event(&view->reduce_motion, x, y, buttons, pressed) & AXW_TOGGLE) {
            return 1;
        }
        axw_switch_event(&view->rounded_corners, x, y, buttons, pressed);
        axw_switch_event(&view->show_details, x, y, buttons, pressed);
        if (axw_slider_event(&view->animation_speed, x, y, buttons, pressed) &
            AXW_CHANGED) {
            return 1;
        }
        break;
    case 1:
        if (axw_list_event(&view->resolution_list, x, y, buttons, pressed) &
            AXW_SELECT) {
            view->resolution_index = view->resolution_list.selected;
            return 1;
        }
        if (axw_slider_event(&view->ui_scale, x, y, buttons, pressed) & AXW_CHANGED) {
            return 1;
        }
        axw_switch_event(&view->night_light, x, y, buttons, pressed);
        axw_switch_event(&view->wallpaper_stars, x, y, buttons, pressed);
        break;
    case 2:
        axw_list_event(&view->adapter_list, x, y, buttons, pressed);
        if (axw_switch_event(&view->dhcp, x, y, buttons, pressed) & AXW_TOGGLE) {
            return 1;
        }
        if (axw_text_field_event(&view->proxy_field, x, y, buttons, pressed) &
            AXW_FOCUS) {
            return 0;
        }
        break;
    case 3:
        if (axw_switch_event(&view->hvci, x, y, buttons, pressed) & AXW_TOGGLE) {
            return 1;
        }
        axw_switch_event(&view->driver_signing, x, y, buttons, pressed);
        axw_switch_event(&view->telemetry, x, y, buttons, pressed);
        if (axw_radio_group_event(&view->security_level, x, y, buttons, pressed) &
            AXW_CHANGED) {
            view->security_policy = view->security_level.selected;
            return 1;
        }
        break;
    default:
        break;
    }

    if (axw_button_event(&view->apply, x, y, buttons, pressed) & AXW_CLICK) {
        return 1;
    }
    if (axw_button_event(&view->reset, x, y, buttons, pressed) & AXW_CLICK) {
        settings_init(view, view->bounds);
        return 1;
    }
    return 0;
}

/* Keyboard: Tab cycles focus, the focused control receives the key.  Only a
 * few controls accept keys today, so the switch is short. */
u32 settings_key(settings_view_t *view, char key, u8 scancode, bool_t shift)
{
    if (!view) {
        return 0;
    }
    if (axw_is_tab_key(key, scancode)) {
        /* Drop focus flags on the previous target before moving on. */
        void *next;
        if (axw_is_shift_tab(key, scancode, shift)) {
            next = axw_focus_prev(&view->focus);
        } else {
            next = axw_focus_next(&view->focus);
        }
        if (next == &view->theme_list) {
            view->theme_list.focused = (bool_t)1;
        } else {
            view->theme_list.focused = (bool_t)0;
        }
        if (next == &view->resolution_list) {
            view->resolution_list.focused = (bool_t)1;
        } else {
            view->resolution_list.focused = (bool_t)0;
        }
        if (next == &view->adapter_list) {
            view->adapter_list.focused = (bool_t)1;
        } else {
            view->adapter_list.focused = (bool_t)0;
        }
        if (next == &view->proxy_field) {
            view->proxy_field.focused = (bool_t)1;
        } else {
            view->proxy_field.focused = (bool_t)0;
        }
        if (next == &view->apply) {
            view->apply.focused = (bool_t)1;
        } else {
            view->apply.focused = (bool_t)0;
        }
        if (next == &view->reset) {
            view->reset.focused = (bool_t)1;
        } else {
            view->reset.focused = (bool_t)0;
        }
        return 1;
    }

    if (view->proxy_field.focused) {
        u32 events = axw_text_field_key(&view->proxy_field, key, scancode);
        if (events & AXW_CHANGED) {
            view->proxy_value = axw_text_field_get(&view->proxy_field);
            return 1;
        }
        return 0;
    }
    if (view->theme_list.focused) {
        axw_list_key(&view->theme_list, key, scancode);
        return 1;
    }
    if (view->resolution_list.focused) {
        axw_list_key(&view->resolution_list, key, scancode);
        return 1;
    }
    if (view->adapter_list.focused) {
        axw_list_key(&view->adapter_list, key, scancode);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Summaries                                                           */
/* ------------------------------------------------------------------ */

/* A one-line description of the current settings, shown in the title bar's
 * subtitle and in `about`. */
void settings_summary(const settings_view_t *view, char *buffer, usize cap)
{
    if (!view || !buffer || cap < 8) {
        return;
    }
    ax_snprintf(buffer, cap, "%s | %d x %d @ %d%% | hvci %s",
                axui_theme_name(view->theme_index),
                view->resolution_index == 0 ? 1024
                : view->resolution_index == 1 ? 1280
                                              : 800,
                view->resolution_index == 0 ? 768
                : view->resolution_index == 1 ? 800
                                              : 600,
                view->ui_scale.value,
                view->hvci.on ? "on" : "off");
}
