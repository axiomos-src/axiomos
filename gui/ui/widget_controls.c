/* AxiomOS UI - basic controls.
 *
 * Button, icon button, checkbox, radio group, switch, slider, progress, spinner
 * and the decorative surfaces (panel, card, badge, chip, divider, tooltip,
 * toast).  Every control paints from its state block and returns an event
 * bitmask from its event function; nothing here allocates.
 *
 * The visual language: 1 px hairline borders, 6 px radius on small controls,
 * a 190 alpha accent fill for primary buttons, and hover/press tints derived
 * from the control's own fill so custom themes stay coherent.
 */

#include <axiom/lib.h>
#include "widgets.h"

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

bool_t axw_hit(gui_rect_t rect, i32 x, i32 y)
{
    return (bool_t)(x >= rect.x && y >= rect.y && x < rect.x + rect.w &&
                    y < rect.y + rect.h);
}

i32 axw_hit_row(gui_rect_t rect, i32 y, i32 row_height, i32 scroll)
{
    if (row_height <= 0) {
        return -1;
    }
    i32 local = y - rect.y + scroll;
    if (local < 0) {
        return -1;
    }
    return local / row_height;
}

i32 axw_hit_column(gui_rect_t rect, i32 x, const i32 *widths, i32 count)
{
    if (!widths) {
        return -1;
    }
    i32 pen = rect.x;
    for (i32 i = 0; i < count; i++) {
        if (x >= pen && x < pen + widths[i]) {
            return i;
        }
        pen += widths[i];
    }
    return -1;
}

/* A control's effective fill: the theme surface, lifted for hover, pressed
 * when held, and flattened when disabled. */
static u32 control_fill(const axw_button_t *button)
{
    const gui_theme_t *theme = axui_theme_current();
    if (!button->enabled) {
        return axui_disabled_fill();
    }
    if (button->primary) {
        if (button->pressed) {
            return axui_darken(theme->accent, 12);
        }
        if (button->hover) {
            return axui_lighten(theme->accent, 8);
        }
        return theme->accent;
    }
    u32 base = theme->window_bg_alt;
    if (button->pressed) {
        return axui_press_tint(base);
    }
    if (button->hover) {
        return axui_hover_tint(base);
    }
    return base;
}

static u32 control_text(const axw_button_t *button)
{
    const gui_theme_t *theme = axui_theme_current();
    if (!button->enabled) {
        return axui_disabled_text();
    }
    if (button->primary) {
        return axui_accent_text();
    }
    if (button->danger) {
        return theme->error;
    }
    return theme->text;
}

/* Paint a focus ring just outside a rect.  Drawn as four segments rather than
 * a rounded outline so it reads on both sharp and rounded controls. */
static void focus_ring(gui_surface_t *surface, gui_rect_t rect, i32 radius)
{
    u32 colour = axui_focus_ring();
    axui_round_rect_border(surface, gui_rect(rect.x - 2, rect.y - 2, rect.w + 4,
                                             rect.h + 4), radius + 2, 2, colour);
}

/* ------------------------------------------------------------------ */
/* Button                                                              */
/* ------------------------------------------------------------------ */

void axw_button_init(axw_button_t *button, gui_rect_t rect, const char *label)
{
    ax_memset(button, 0, sizeof(*button));
    button->rect = rect;
    button->label = label;
    button->enabled = (bool_t)1;
    button->radius = axui_metrics()->radius_medium;
}

void axw_button_set_primary(axw_button_t *button, bool_t primary)
{
    button->primary = primary;
}

void axw_button_set_enabled(axw_button_t *button, bool_t enabled)
{
    button->enabled = enabled;
}

void axw_button_paint(gui_surface_t *surface, const axw_button_t *button)
{
    if (!surface || !button->label) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    u32 fill = control_fill(button);
    gui_rect_t rect = button->rect;

    if (!button->ghost) {
        axui_round_rect_aa(surface, rect, button->radius, fill);
        bool_t hairline = !button->primary;
        if (hairline) {
            axui_round_rect_border(surface, rect, button->radius, 1, theme->border);
        }
        /* A one-pixel top highlight gives the button a slight lift on dark
         * themes, the same trick the title bar uses. */
        if (axui_is_dark(fill) && !button->pressed) {
            gui_rect_t highlight = gui_rect(rect.x + button->radius, rect.y,
                                            rect.w - button->radius * 2, 1);
            gui_fill_rect(surface, highlight, axui_lighten(fill, 12));
        }
    }

    axui_text_style_t style = axui_style(control_text(button), 1);
    style.bold = button->primary;

    i32 text_y = axui_text_baseline(rect, 1);
    if (button->glyph) {
        /* Glyph on the left, label centred in the remaining space. */
        i32 glyph_x = rect.x + 12;
        axui_draw_text(surface, glyph_x, text_y, button->glyph, style);
        i32 label_x = glyph_x + 16;
        i32 avail = rect.x + rect.w - label_x - 12;
        i32 width = axui_measure_text(button->label, 1);
        i32 centre = label_x + (avail - width) / 2;
        axui_draw_text_clip(surface, rect, centre, text_y, button->label, style);
    } else {
        axui_draw_text_center(surface, rect, button->label, style);
    }

    if (button->focused) {
        focus_ring(surface, rect, button->radius);
    }
}

u32 axw_button_event(axw_button_t *button, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!button || !button->enabled) {
        return AXW_NONE;
    }
    bool_t inside = axw_hit(button->rect, x, y);
    bool_t was_hover = button->hover;
    button->hover = inside;

    u32 events = AXW_NONE;
    if (inside != was_hover) {
        events |= inside ? AXW_HOVER : 0;
    }

    if (pressed && inside) {
        button->pressed = (bool_t)1;
        events |= AXW_PRESS;
    } else if (!pressed) {
        if (button->pressed) {
            button->pressed = (bool_t)0;
            events |= AXW_RELEASE;
            if (inside) {
                events |= AXW_CLICK;
            }
        }
    }
    (void)buttons;
    return events;
}

/* ------------------------------------------------------------------ */
/* Icon button                                                         */
/* ------------------------------------------------------------------ */

void axw_icon_button_init(axw_icon_button_t *button, gui_rect_t rect,
                          const char *glyph)
{
    ax_memset(button, 0, sizeof(*button));
    button->rect = rect;
    button->glyph = glyph;
    button->glyph_colour = axui_theme_current()->text_dim;
}

void axw_icon_button_paint(gui_surface_t *surface, const axw_icon_button_t *button)
{
    if (!surface || !button->glyph) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    u32 fill = button->hover ? axui_hover_tint(theme->window_bg)
                             : theme->window_bg;
    if (button->pressed) {
        fill = axui_press_tint(fill);
    }
    axui_round_rect_aa(surface, button->rect, 4, fill);

    u32 colour = button->glyph_colour;
    if (button->close_style && button->hover) {
        colour = theme->error;
        axui_round_rect_aa(surface, button->rect, 4,
                           axui_colour_alpha(theme->error, 40));
    }
    axui_text_style_t style = axui_style(colour, 1);
    axui_draw_text_center(surface, button->rect, button->glyph, style);
}

u32 axw_icon_button_event(axw_icon_button_t *button, i32 x, i32 y, u8 buttons,
                          bool_t pressed)
{
    if (!button) {
        return AXW_NONE;
    }
    bool_t inside = axw_hit(button->rect, x, y);
    button->hover = inside;
    u32 events = AXW_NONE;
    if (pressed && inside) {
        button->pressed = (bool_t)1;
        events |= AXW_PRESS;
    } else if (!pressed && button->pressed) {
        button->pressed = (bool_t)0;
        events |= AXW_RELEASE;
        if (inside) {
            events |= AXW_CLICK;
        }
    }
    (void)buttons;
    return events;
}

/* ------------------------------------------------------------------ */
/* Checkbox                                                            */
/* ------------------------------------------------------------------ */

void axw_checkbox_init(axw_checkbox_t *box, gui_rect_t rect, const char *label,
                       bool_t checked)
{
    ax_memset(box, 0, sizeof(*box));
    box->rect = rect;
    box->label = label;
    box->checked = checked;
    box->enabled = (bool_t)1;
}

void axw_checkbox_paint(gui_surface_t *surface, const axw_checkbox_t *box)
{
    if (!surface) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    i32 size = axui_min_i32(16, box->rect.h);
    gui_rect_t mark = gui_rect(box->rect.x, box->rect.y + (box->rect.h - size) / 2,
                               size, size);

    u32 fill = box->enabled ? theme->window_bg : axui_disabled_fill();
    if (box->checked || box->indeterminate) {
        fill = box->enabled ? theme->accent : axui_disabled_text();
    } else if (box->hover) {
        fill = axui_hover_tint(fill);
    }
    axui_round_rect_aa(surface, mark, 3, fill);
    if (!box->checked && !box->indeterminate) {
        axui_round_rect_border(surface, mark, 3, 1, theme->border);
    }

    if (box->checked) {
        /* Tick: two anti-aliased strokes. */
        u32 ink = axui_accent_text();
        axui_line_aa(surface, mark.x + 3, mark.y + size / 2,
                     mark.x + size / 2 - 1, mark.y + size - 4, ink, 2);
        axui_line_aa(surface, mark.x + size / 2 - 1, mark.y + size - 4,
                     mark.x + size - 4, mark.y + 3, ink, 2);
    } else if (box->indeterminate) {
        u32 ink = axui_accent_text();
        gui_fill_rect(surface, gui_rect(mark.x + 4, mark.y + size / 2 - 1, size - 8, 2),
                      ink);
    }

    if (box->label) {
        axui_text_style_t style = axui_style(box->enabled ? theme->text
                                                          : axui_disabled_text(), 1);
        i32 text_x = mark.x + size + 8;
        axui_draw_text_clip(surface,
                            gui_rect(text_x, box->rect.y,
                                     box->rect.x + box->rect.w - text_x, box->rect.h),
                            text_x, axui_text_baseline(box->rect, 1), box->label,
                            style);
    }

    if (box->focused) {
        focus_ring(surface, mark, 3);
    }
}

u32 axw_checkbox_event(axw_checkbox_t *box, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!box || !box->enabled) {
        return AXW_NONE;
    }
    bool_t inside = axw_hit(box->rect, x, y);
    box->hover = inside;
    if (pressed && inside) {
        box->pressed = (bool_t)1;
        return AXW_PRESS;
    }
    if (!pressed && box->pressed) {
        box->pressed = (bool_t)0;
        if (inside) {
            box->checked = (bool_t)!box->checked;
            box->indeterminate = (bool_t)0;
            return AXW_CLICK | AXW_TOGGLE | AXW_CHANGED;
        }
    }
    (void)buttons;
    return AXW_NONE;
}

/* ------------------------------------------------------------------ */
/* Radio                                                               */
/* ------------------------------------------------------------------ */

void axw_radio_init(axw_radio_t *radio, gui_rect_t rect, const char *label,
                    bool_t selected)
{
    ax_memset(radio, 0, sizeof(*radio));
    radio->rect = rect;
    radio->label = label;
    radio->selected = selected;
    radio->enabled = (bool_t)1;
}

void axw_radio_paint(gui_surface_t *surface, const axw_radio_t *radio)
{
    if (!surface) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    i32 size = axui_min_i32(16, radio->rect.h);
    i32 cx = radio->rect.x + size / 2;
    i32 cy = radio->rect.y + radio->rect.h / 2;

    u32 ring = radio->hover ? theme->accent : theme->border;
    if (!radio->enabled) {
        ring = axui_disabled_text();
    }
    axui_circle(surface, cx, cy, size / 2, radio->enabled ? theme->window_bg
                                                          : axui_disabled_fill());
    axui_circle_outline(surface, cx, cy, size / 2, 1, ring);

    if (radio->selected) {
        axui_circle(surface, cx, cy, size / 2 - 4,
                    radio->enabled ? theme->accent : axui_disabled_text());
    }

    if (radio->label) {
        axui_text_style_t style = axui_style(radio->enabled ? theme->text
                                                            : axui_disabled_text(), 1);
        axui_draw_text_clip(surface,
                            gui_rect(cx + size, radio->rect.y,
                                     radio->rect.x + radio->rect.w - cx - size,
                                     radio->rect.h),
                            cx + size + 6, axui_text_baseline(radio->rect, 1),
                            radio->label, style);
    }
    if (radio->focused) {
        axui_circle_outline(surface, cx, cy, size / 2 + 2, 1, theme->accent);
    }
}

u32 axw_radio_event(axw_radio_t *radio, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!radio || !radio->enabled) {
        return AXW_NONE;
    }
    bool_t inside = axw_hit(radio->rect, x, y);
    radio->hover = inside;
    if (pressed && inside) {
        return AXW_PRESS;
    }
    if (!pressed && inside) {
        return AXW_CLICK | AXW_TOGGLE | AXW_CHANGED;
    }
    (void)buttons;
    return AXW_NONE;
}

void axw_radio_group_init(axw_radio_group_t *group, gui_rect_t rect, i32 count,
                          bool_t horizontal, i32 gap)
{
    ax_memset(group, 0, sizeof(*group));
    if (count > 8) {
        count = 8;
    }
    group->count = count;
    group->horizontal = horizontal;
    group->gap = gap;
    group->selected = 0;

    for (i32 i = 0; i < count; i++) {
        gui_rect_t item;
        if (horizontal) {
            i32 width = (rect.w - gap * (count - 1)) / count;
            item = gui_rect(rect.x + i * (width + gap), rect.y, width, rect.h);
        } else {
            i32 height = (rect.h - gap * (count - 1)) / count;
            item = gui_rect(rect.x, rect.y + i * (height + gap), rect.w, height);
        }
        axw_radio_init(&group->items[i], item, "", (bool_t)(i == 0));
    }
}

void axw_radio_group_label(axw_radio_group_t *group, i32 index, const char *label)
{
    if (!group || index < 0 || index >= group->count) {
        return;
    }
    group->items[index].label = label;
}

void axw_radio_group_paint(gui_surface_t *surface, const axw_radio_group_t *group)
{
    for (i32 i = 0; i < group->count; i++) {
        axw_radio_paint(surface, &group->items[i]);
    }
}

u32 axw_radio_group_event(axw_radio_group_t *group, i32 x, i32 y, u8 buttons,
                          bool_t pressed)
{
    if (!group) {
        return AXW_NONE;
    }
    for (i32 i = 0; i < group->count; i++) {
        u32 events = axw_radio_event(&group->items[i], x, y, buttons, pressed);
        if (events & (AXW_TOGGLE | AXW_CHANGED)) {
            for (i32 j = 0; j < group->count; j++) {
                group->items[j].selected = (bool_t)(j == i);
            }
            group->selected = i;
            return AXW_CLICK | AXW_TOGGLE | AXW_CHANGED | (u32)(i << 16);
        }
        if (events) {
            return events;
        }
    }
    return AXW_NONE;
}

/* ------------------------------------------------------------------ */
/* Switch                                                              */
/* ------------------------------------------------------------------ */

void axw_switch_init(axw_switch_t *toggle, gui_rect_t rect, const char *label,
                     bool_t on)
{
    ax_memset(toggle, 0, sizeof(*toggle));
    toggle->rect = rect;
    toggle->label = label;
    toggle->on = on;
    toggle->enabled = (bool_t)1;
    toggle->anim = on ? 100 : 0;
}

void axw_switch_paint(gui_surface_t *surface, const axw_switch_t *toggle)
{
    if (!surface) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    i32 track_w = 36;
    i32 track_h = 18;
    if (track_w > toggle->rect.w) {
        track_w = toggle->rect.w;
    }
    gui_rect_t track = gui_rect(toggle->rect.x,
                                toggle->rect.y + (toggle->rect.h - track_h) / 2,
                                track_w, track_h);

    /* The track interpolates from the border colour to the accent as the
     * switch animates, so a mid-toggle reads as a real in-between value. */
    u32 off = toggle->enabled ? theme->window_bg_alt : axui_disabled_fill();
    u32 on = toggle->enabled ? theme->accent : axui_disabled_text();
    u32 fill = axui_lerp_colour(off, on, toggle->anim);
    axui_round_rect_aa(surface, track, track_h / 2, fill);
    axui_round_rect_border(surface, track, track_h / 2, 1,
                           toggle->hover ? theme->accent : theme->border);

    /* Knob: 14 px, positioned along the track. */
    i32 knob = 14;
    i32 travel = track_w - knob - 4;
    i32 x = track.x + 2 + (travel * toggle->anim) / 100;
    axui_circle(surface, x + knob / 2, track.y + track_h / 2, knob / 2,
                toggle->enabled ? theme->text : axui_disabled_text());

    if (toggle->label) {
        axui_text_style_t style = axui_style(toggle->enabled ? theme->text
                                                             : axui_disabled_text(), 1);
        i32 text_x = track.x + track_w + 10;
        axui_draw_text_clip(surface,
                            gui_rect(text_x, toggle->rect.y,
                                     toggle->rect.x + toggle->rect.w - text_x,
                                     toggle->rect.h),
                            text_x, axui_text_baseline(toggle->rect, 1), toggle->label,
                            style);
    }
    if (toggle->focused) {
        axui_round_rect_border(surface, gui_rect(track.x - 2, track.y - 2,
                                                 track.w + 4, track.h + 4),
                               track_h / 2 + 2, 1, theme->accent);
    }
}

u32 axw_switch_event(axw_switch_t *toggle, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!toggle || !toggle->enabled) {
        return AXW_NONE;
    }
    bool_t inside = axw_hit(toggle->rect, x, y);
    toggle->hover = inside;
    if (pressed && inside) {
        return AXW_PRESS;
    }
    if (!pressed && inside) {
        toggle->on = (bool_t)!toggle->on;
        return AXW_CLICK | AXW_TOGGLE | AXW_CHANGED;
    }
    (void)buttons;
    return AXW_NONE;
}

void axw_switch_animate(axw_switch_t *toggle, i32 step)
{
    if (!toggle) {
        return;
    }
    i32 target = toggle->on ? 100: 0;
    toggle->anim = axui_approach(toggle->anim, target, step);
}

/* ------------------------------------------------------------------ */
/* Slider                                                              */
/* ------------------------------------------------------------------ */

void axw_slider_init(axw_slider_t *slider, gui_rect_t rect, i32 minimum,
                     i32 maximum, i32 value)
{
    ax_memset(slider, 0, sizeof(*slider));
    slider->rect = rect;
    slider->minimum = minimum;
    slider->maximum = maximum > minimum ? maximum : minimum + 1;
    slider->step = 1;
    slider->value = axui_clamp_i32(value, minimum, slider->maximum);
    slider->enabled = (bool_t)1;
}

void axw_slider_set_value(axw_slider_t *slider, i32 value)
{
    if (!slider) {
        return;
    }
    slider->value = axui_clamp_i32(value, slider->minimum, slider->maximum);
}

void axw_slider_paint(gui_surface_t *surface, const axw_slider_t *slider)
{
    if (!surface) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    i32 track_h = 4;
    i32 cy = slider->rect.y + slider->rect.h / 2;
    gui_rect_t track = gui_rect(slider->rect.x, cy - track_h / 2,
                                slider->rect.w, track_h);

    u32 track_colour = slider->enabled ? axui_tint(theme->window_bg_alt,
                                                  theme->border, 40)
                                       : axui_disabled_fill();
    axui_round_rect_aa(surface, track, track_h / 2, track_colour);

    i32 span = slider->maximum - slider->minimum;
    i32 position = span > 0 ? ((slider->value - slider->minimum) * track.w) / span : 0;
    if (position > track.w) {
        position = track.w;
    }

    gui_rect_t filled = gui_rect(track.x, track.y, position, track.h);
    if (filled.w > 0) {
        axui_round_rect_aa(surface, filled, track_h / 2,
                           slider->enabled ? theme->accent : axui_disabled_text());
    }

    i32 knob_r = slider->hover || slider->dragging ? 8 : 7;
    i32 knob_x = track.x + position;
    axui_circle(surface, knob_x, cy, knob_r,
                slider->enabled ? theme->text : axui_disabled_text());
    axui_circle_outline(surface, knob_x, cy, knob_r, 1, theme->window_bg);

    if (slider->show_value) {
        char text[16];
        ax_itoa(slider->value, text);
        axui_text_style_t style = axui_style(theme->text_dim, 1);
        axui_draw_text(surface, slider->rect.x + slider->rect.w + 8,
                       axui_text_baseline(slider->rect, 1), text, style);
    }
    if (slider->focused) {
        axui_circle_outline(surface, knob_x, cy, knob_r + 2, 1, theme->accent);
    }
}

u32 axw_slider_event(axw_slider_t *slider, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!slider || !slider->enabled) {
        return AXW_NONE;
    }
    gui_rect_t hit = gui_rect(slider->rect.x, slider->rect.y - 6,
                              slider->rect.w, slider->rect.h + 12);
    bool_t inside = axw_hit(hit, x, y);
    slider->hover = inside;

    if (pressed && inside) {
        slider->dragging = (bool_t)1;
    }
    if (!pressed) {
        if (slider->dragging) {
            slider->dragging = (bool_t)0;
            return AXW_RELEASE;
        }
        return AXW_NONE;
    }

    if (!slider->dragging) {
        (void)buttons;
        return AXW_NONE;
    }

    /* Update the value from the pointer position, snapped to the step. */
    i32 old = slider->value;
    i32 relative = x - slider->rect.x;
    if (relative < 0) {
        relative = 0;
    }
    if (relative > slider->rect.w) {
        relative = slider->rect.w;
    }
    i32 span = slider->maximum - slider->minimum;
    i32 value = slider->minimum + (relative * span) / (slider->rect.w > 0 ? slider->rect.w : 1);
    if (slider->step > 1) {
        value = slider->minimum + ((value - slider->minimum) / slider->step) * slider->step;
    }
    slider->value = axui_clamp_i32(value, slider->minimum, slider->maximum);
    return slider->value != old ? (AXW_CHANGED | AXW_SCROLL) : AXW_NONE;
}

/* ------------------------------------------------------------------ */
/* Progress                                                            */
/* ------------------------------------------------------------------ */

void axw_progress_init(axw_progress_t *bar, gui_rect_t rect, i32 percent)
{
    ax_memset(bar, 0, sizeof(*bar));
    bar->rect = rect;
    bar->percent = axui_clamp_i32(percent, 0, 100);
    bar->displayed = bar->percent;
    bar->track = axui_theme_current()->window_bg_alt;
    bar->fill = axui_theme_current()->accent;
}

void axw_progress_set(axw_progress_t *bar, i32 percent)
{
    if (!bar) {
        return;
    }
    bar->percent = axui_clamp_i32(percent, 0, 100);
}

void axw_progress_paint(gui_surface_t *surface, const axw_progress_t *bar)
{
    if (!surface) {
        return;
    }
    i32 radius = axui_min_i32(bar->rect.h / 2, 6);
    axui_round_rect_aa(surface, bar->rect, radius, bar->track);

    i32 width = (bar->rect.w * bar->displayed) / 100;
    if (width > 0) {
        gui_rect_t filled = gui_rect(bar->rect.x, bar->rect.y, width, bar->rect.h);
        axui_round_rect_aa(surface, filled, radius, bar->fill);
    }

    if (bar->striped && width > 8) {
        /* Diagonal stripes drawn as short segments, offset by the phase. */
        i32 offset = bar->phase % 12;
        for (i32 x = offset - 12; x < width; x += 12) {
            i32 x0 = axui_max_i32(bar->rect.x, bar->rect.x + x);
            i32 x1 = axui_min_i32(bar->rect.x + width, bar->rect.x + x + 6);
            if (x1 <= x0) {
                continue;
            }
            axui_line_aa(surface, x0, bar->rect.y + bar->rect.h - 1, x1,
                         bar->rect.y, axui_lighten(bar->fill, 18), 2);
        }
    }

    if (bar->indeterminate) {
        /* A moving three-quarter arc inside the track. */
        i32 cx = bar->rect.x + bar->rect.w / 2;
        i32 cy = bar->rect.y + bar->rect.h / 2;
        axui_arc(surface, cx, cy, bar->rect.h / 2 - 2, bar->phase * 6, 120, 2,
                 bar->fill);
    }
}

void axw_progress_animate(axw_progress_t *bar, i32 step)
{
    if (!bar) {
        return;
    }
    bar->displayed = axui_approach(bar->displayed, bar->percent, step);
    bar->phase = (bar->phase + step) % 360;
}

/* ------------------------------------------------------------------ */
/* Spinner                                                             */
/* ------------------------------------------------------------------ */

void axw_spinner_init(axw_spinner_t *spinner, i32 cx, i32 cy, i32 radius)
{
    ax_memset(spinner, 0, sizeof(*spinner));
    spinner->cx = cx;
    spinner->cy = cy;
    spinner->radius = radius;
    spinner->colour = axui_theme_current()->accent;
    spinner->running = (bool_t)1;
}

void axw_spinner_paint(gui_surface_t *surface, const axw_spinner_t *spinner)
{
    if (!surface) {
        return;
    }
    axui_arc(surface, spinner->cx, spinner->cy, spinner->radius, spinner->angle, 270,
             2, spinner->colour);
}

void axw_spinner_advance(axw_spinner_t *spinner, i32 degrees)
{
    if (!spinner || !spinner->running) {
        return;
    }
    spinner->angle = (spinner->angle + degrees) % 360;
}

/* ------------------------------------------------------------------ */
/* Panel, card, divider                                                */
/* ------------------------------------------------------------------ */

void axw_panel_paint(gui_surface_t *surface, gui_rect_t rect, i32 radius)
{
    const gui_theme_t *theme = axui_theme_current();
    axui_round_rect_aa(surface, rect, radius, theme->window_bg_alt);
    axui_round_rect_border(surface, rect, radius, 1, theme->border);
}

void axw_card_paint(gui_surface_t *surface, gui_rect_t rect, i32 radius)
{
    const gui_theme_t *theme = axui_theme_current();
    /* A card sits one step above a panel and carries a shadow, which is what
     * separates it from the background on both dark and light themes. */
    gui_shadow(surface, rect, 6, (u8)axui_shadow_strength());
    axui_round_rect_aa(surface, rect, radius, theme->window_bg_alt);
    axui_round_rect_border(surface, rect, radius, 1, theme->border);
    gui_fill_rect(surface, gui_rect(rect.x + radius, rect.y, rect.w - radius * 2, 1),
                  axui_lighten(theme->window_bg_alt, 8));
}

void axw_divider_paint(gui_surface_t *surface, gui_rect_t rect)
{
    const gui_theme_t *theme = axui_theme_current();
    if (rect.w > rect.h) {
        gui_fill_rect(surface, gui_rect(rect.x, rect.y + rect.h / 2, rect.w, 1),
                      axui_hairline(theme->window_bg));
    } else {
        gui_fill_rect(surface, gui_rect(rect.x + rect.w / 2, rect.y, 1, rect.h),
                      axui_hairline(theme->window_bg));
    }
}

/* ------------------------------------------------------------------ */
/* Badge, chip                                                         */
/* ------------------------------------------------------------------ */

void axw_badge_paint(gui_surface_t *surface, gui_rect_t rect, const char *text,
                     u32 fill, u32 text_colour)
{
    if (!surface || !text) {
        return;
    }
    axui_round_rect_aa(surface, rect, rect.h / 2, fill);
    axui_text_style_t style = axui_style(text_colour, 1);
    axui_draw_text_center(surface, rect, text, style);
}

void axw_chip_paint(gui_surface_t *surface, gui_rect_t rect, const char *text,
                    const char *glyph)
{
    if (!surface) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    axui_round_rect_aa(surface, rect, rect.h / 2,
                       axui_tint(theme->window_bg_alt, theme->accent, 12));
    axui_round_rect_border(surface, rect, rect.h / 2, 1,
                           axui_colour_alpha(theme->accent, 120));

    i32 pen = rect.x + 8;
    axui_text_style_t style = axui_style(theme->text, 1);
    if (glyph) {
        axui_draw_text(surface, pen, axui_text_baseline(rect, 1), glyph,
                       axui_style(theme->accent, 1));
        pen += 14;
    }
    if (text) {
        axui_draw_text_clip(surface,
                            gui_rect(pen, rect.y, rect.x + rect.w - pen, rect.h),
                            pen, axui_text_baseline(rect, 1), text, style);
    }
}

/* ------------------------------------------------------------------ */
/* Section header and empty state                                      */
/* ------------------------------------------------------------------ */

void axw_section_header_paint(gui_surface_t *surface, gui_rect_t rect,
                              const char *title, const char *subtitle)
{
    if (!surface) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    axui_text_style_t title_style = axui_style(theme->text, 1);
    title_style.bold = (bool_t)1;
    axui_draw_text(surface, rect.x, rect.y, title, title_style);
    if (subtitle) {
        axui_text_style_t sub = axui_style(theme->text_dim, 1);
        axui_draw_text(surface, rect.x, rect.y + 18, subtitle, sub);
    }
    gui_fill_rect(surface, gui_rect(rect.x, rect.y + rect.h - 1, rect.w, 1),
                  axui_hairline(theme->window_bg));
}

void axw_empty_state_paint(gui_surface_t *surface, gui_rect_t rect,
                           const char *glyph, const char *title,
                           const char *detail)
{
    if (!surface) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    i32 cy = rect.y + rect.h / 2 - 30;

    if (glyph) {
        axui_text_style_t style = axui_style(theme->text_dim, 2);
        axui_draw_text_center(surface, gui_rect(rect.x, cy, rect.w, 32), glyph,
                              style);
        cy += 40;
    }
    if (title) {
        axui_text_style_t style = axui_style(theme->text, 1);
        axui_draw_text_center(surface, gui_rect(rect.x, cy, rect.w, 18), title,
                              style);
        cy += 20;
    }
    if (detail) {
        axui_text_style_t style = axui_style(theme->text_dim, 1);
        axui_draw_text_center(surface, gui_rect(rect.x, cy, rect.w, 18), detail,
                              style);
    }
}

/* ------------------------------------------------------------------ */
/* Tooltip and toast                                                   */
/* ------------------------------------------------------------------ */

gui_rect_t axw_tooltip_measure(gui_rect_t anchor, const char *text)
{
    i32 width = axui_measure_text(text, 1) + 16;
    i32 height = 22;
    i32 x = anchor.x + (anchor.w - width) / 2;
    i32 y = anchor.y - height - 6;
    if (y < 0) {
        y = anchor.y + anchor.h + 6;
    }
    return gui_rect(x, y, width, height);
}

void axw_tooltip_paint(gui_surface_t *surface, gui_rect_t anchor, const char *text)
{
    if (!surface || !text) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    gui_rect_t rect = axw_tooltip_measure(anchor, text);
    axui_round_rect_aa(surface, rect, 4, axui_surface_overlay());
    axui_round_rect_border(surface, rect, 4, 1, theme->border);
    axui_text_style_t style = axui_style(theme->text, 1);
    axui_draw_text_center(surface, rect, text, style);
}

void axw_toast_init(axw_toast_t *toast, gui_rect_t rect, const char *text, i32 level)
{
    ax_memset(toast, 0, sizeof(*toast));
    toast->rect = rect;
    toast->text = text;
    toast->level = level;
    toast->life = 180;      /* about three seconds at 60 Hz */
    toast->total = toast->life;
}

void axw_toast_paint(gui_surface_t *surface, const axw_toast_t *toast)
{
    if (!surface || !toast->text) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    u32 accent = toast->level == 1 ? theme->success
                 : toast->level == 2 ? theme->warning
                 : toast->level == 3 ? theme->error
                                     : theme->accent;

    /* Fade in over the first 15 frames and out over the last 15. */
    i32 elapsed = toast->total - toast->life;
    i32 alpha = 255;
    if (elapsed < 15) {
        alpha = (elapsed * 255) / 15;
    } else if (toast->life < 15) {
        alpha = (toast->life * 255) / 15;
    }
    if (alpha < 0) {
        alpha = 0;
    }

    gui_rect_t rect = toast->rect;
    if (elapsed < 15) {
        /* Slide down into place while fading in. */
        rect.y -= (15 - elapsed) * 3;
    }

    axui_round_rect_aa(surface, rect, 8, axui_colour_alpha(theme->window_bg_alt,
                                                           (u8)alpha));
    axui_round_rect_border(surface, rect, 8, 1,
                           axui_colour_alpha(accent, (u8)alpha));
    gui_fill_rect(surface, gui_rect(rect.x + 12, rect.y + 8, 3, rect.h - 16),
                  axui_colour_alpha(accent, (u8)alpha));

    axui_text_style_t style = axui_style(axui_colour_alpha(theme->text, (u8)alpha), 1);
    axui_draw_text_clip(surface, rect, rect.x + 22, axui_text_baseline(rect, 1),
                        toast->text, style);
}

bool_t axw_toast_tick(axw_toast_t *toast)
{
    if (!toast) {
        return (bool_t)0;
    }
    if (toast->life > 0) {
        toast->life--;
    }
    return (bool_t)(toast->life > 0);
}

/* ------------------------------------------------------------------ */
/* Scrollbar                                                           */
/* ------------------------------------------------------------------ */

void axw_scrollbar_init(axw_scrollbar_t *bar, gui_rect_t rect, bool_t vertical)
{
    ax_memset(bar, 0, sizeof(*bar));
    bar->rect = rect;
    bar->vertical = vertical;
}

void axw_scrollbar_config(axw_scrollbar_t *bar, i32 total, i32 visible, i32 offset)
{
    if (!bar) {
        return;
    }
    bar->total = total > 0 ? total : 0;
    bar->visible = visible > 0 ? visible : 0;
    bar->offset = offset;
}

/* Geometry of the thumb, shared by paint and by the drag maths. */
static gui_rect_t scrollbar_thumb(const axw_scrollbar_t *bar)
{
    i32 length = bar->vertical ? bar->rect.h : bar->rect.w;
    i32 thickness = bar->vertical ? bar->rect.w : bar->rect.h;
    if (bar->total <= 0 || bar->visible >= bar->total) {
        return gui_rect(bar->rect.x, bar->rect.y, bar->rect.w, bar->rect.h);
    }
    i32 thumb_len = (length * bar->visible) / bar->total;
    i32 minimum = axui_metrics()->scrollbar_min_thumb;
    if (thumb_len < minimum) {
        thumb_len = minimum;
    }
    if (thumb_len > length) {
        thumb_len = length;
    }
    i32 max_offset = bar->total - bar->visible;
    i32 position = max_offset > 0 ? (bar->offset * (length - thumb_len)) / max_offset
                                 : 0;
    if (position < 0) {
        position = 0;
    }
    if (position > length - thumb_len) {
        position = length - thumb_len;
    }
    if (bar->vertical) {
        return gui_rect(bar->rect.x, bar->rect.y + position, thickness, thumb_len);
    }
    return gui_rect(bar->rect.x + position, bar->rect.y, thumb_len, thickness);
}

void axw_scrollbar_paint(gui_surface_t *surface, const axw_scrollbar_t *bar)
{
    if (!surface) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    gui_fill_rect(surface, bar->rect, axui_tint(theme->window_bg,
                                               theme->border, 30));

    gui_rect_t thumb = scrollbar_thumb(bar);
    u32 colour = bar->dragging ? theme->accent
                               : bar->hover ? axui_lighten(theme->border, 20)
                                            : theme->border;
    if (bar->vertical) {
        axui_round_rect_aa(surface, gui_rect(thumb.x + 2, thumb.y, thumb.w - 4,
                                             thumb.h), 3, colour);
    } else {
        axui_round_rect_aa(surface, gui_rect(thumb.x, thumb.y + 2, thumb.w,
                                             thumb.h - 4), 3, colour);
    }
}

u32 axw_scrollbar_event(axw_scrollbar_t *bar, i32 x, i32 y, u8 buttons, bool_t pressed)
{
    if (!bar) {
        return AXW_NONE;
    }
    bool_t inside = axw_hit(bar->rect, x, y);
    bar->hover = inside;

    gui_rect_t thumb = scrollbar_thumb(bar);
    if (pressed && axw_hit(thumb, x, y)) {
        bar->dragging = (bool_t)1;
        bar->drag_origin = bar->vertical ? y : x;
        bar->drag_offset = bar->offset;
        return AXW_PRESS;
    }
    if (pressed && inside) {
        /* A click on the track pages by one window toward the pointer. */
        i32 delta = bar->visible;
        i32 old = bar->offset;
        if (bar->vertical) {
            bar->offset += y < thumb.y ? -delta : delta;
        } else {
            bar->offset += x < thumb.x ? -delta : delta;
        }
        i32 max_offset = bar->total - bar->visible;
        bar->offset = axui_clamp_i32(bar->offset, 0, max_offset > 0 ? max_offset : 0);
        return bar->offset != old ? (AXW_SCROLL | AXW_CHANGED) : AXW_NONE;
    }
    if (!pressed) {
        if (bar->dragging) {
            bar->dragging = (bool_t)0;
            return AXW_RELEASE;
        }
        return AXW_NONE;
    }
    if (!bar->dragging) {
        (void)buttons;
        return AXW_NONE;
    }

    i32 length = bar->vertical ? bar->rect.h : bar->rect.w;
    i32 thumb_len = bar->vertical ? thumb.h : thumb.w;
    i32 travel = length - thumb_len;
    if (travel <= 0) {
        return AXW_NONE;
    }
    i32 moved = (bar->vertical ? y : x) - bar->drag_origin;
    i32 max_offset = bar->total - bar->visible;
    i32 old = bar->offset;
    bar->offset = bar->drag_offset + (moved * max_offset) / travel;
    bar->offset = axui_clamp_i32(bar->offset, 0, max_offset);
    return bar->offset != old ? (AXW_SCROLL | AXW_CHANGED) : AXW_NONE;
}
