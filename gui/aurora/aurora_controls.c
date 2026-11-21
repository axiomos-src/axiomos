/*
 * AxiomOS - Aurora control library.
 *
 * Each control follows the same three-part contract:
 *
 *   paint()  reads state, never mutates it;
 *   hit()    answers whether a point belongs to the control and updates only
 *            hover/focus, never the value;
 *   action() changes the value and returns whether it did.
 *
 * Keeping mutation out of paint is what makes the shell testable: a frame can
 * be rendered at any point in a transition without side effects, which matters
 * for screenshots and for the boot-time surface check.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */

static void button_colours(const axau_button_t *button,
                           const axau_palette_t *palette,
                           u32 *fill, u32 *border, u32 *text)
{
    if (!button->enabled) {
        *fill = axau_colour(palette, AXAU_COLOUR_CONTROL_DISABLED);
        *border = axau_colour(palette, AXAU_COLOUR_STROKE);
        *text = axau_colour(palette, AXAU_COLOUR_TEXT_DISABLED);
        return;
    }
    if (button->accent) {
        *fill = axau_colour(palette, AXAU_COLOUR_ACCENT);
        if (button->pressed) {
            *fill = axau_colour(palette, AXAU_COLOUR_ACCENT_PRESSED);
        } else if (button->hover) {
            *fill = axau_colour(palette, AXAU_COLOUR_ACCENT_HOVER);
        }
        *border = axui_colour_alpha(GUI_RGB(0x00, 0x00, 0x00), 40);
        *text = axau_colour(palette, AXAU_COLOUR_ACCENT_TEXT);
        return;
    }
    if (button->subtle) {
        *fill = 0;
        *border = 0;
        *text = axau_colour(palette, AXAU_COLOUR_TEXT);
        if (button->pressed) {
            *fill = axau_colour(palette, AXAU_COLOUR_CONTROL_PRESSED);
        } else if (button->hover) {
            *fill = axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER);
        }
        return;
    }
    *fill = axau_colour(palette, AXAU_COLOUR_CONTROL);
    if (button->pressed) {
        *fill = axau_colour(palette, AXAU_COLOUR_CONTROL_PRESSED);
    } else if (button->hover) {
        *fill = axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER);
    }
    *border = axau_colour(palette, AXAU_COLOUR_STROKE);
    *text = axau_colour(palette, AXAU_COLOUR_TEXT);
}

void axau_button_paint(gui_surface_t *surface, axau_button_t *button,
                       const axau_palette_t *palette,
                       const axau_metrics_t *metrics)
{
    if (!surface || !button) return;
    i32 radius = metrics ? metrics->radius_control : AXAU_RADIUS_CONTROL;
    u32 fill = 0;
    u32 border = 0;
    u32 text = 0;
    button_colours(button, palette, &fill, &border, &text);

    /* Press animation shrinks the fill by one pixel on each side, which reads
     * as a physical key without moving the label. */
    gui_rect_t body = button->rect;
    if (button->pressed) {
        body = gui_rect(body.x + 1, body.y + 1, body.w - 2, body.h - 2);
    }
    if (fill) {
        axui_round_rect_aa(surface, body, radius, fill);
    }
    if (border) {
        axui_round_rect_border(surface, body, radius, 1, border);
    }
    if (!button->enabled) {
        /* Disabled buttons keep their label, dimmed, but lose any accent. */
    }

    i32 text_scale = 1;
    i32 label_width = 0;
    if (button->label[0]) {
        label_width = axui_measure_text(button->label, (u8)text_scale);
    }
    i32 icon_width = button->has_icon ? AXAU_ICON_SIZE + 8 : 0;
    i32 content_width = label_width + icon_width;
    i32 start_x = body.x + (body.w - content_width) / 2;
    i32 centre_y = axau_text_center_y(body, (u8)text_scale);
    if (button->has_icon) {
        axau_icon_glyph(surface, start_x + AXAU_ICON_SIZE / 2,
                        body.y + body.h / 2, AXAU_ICON_SIZE, button->glyph, text);
        start_x += AXAU_ICON_SIZE + 8;
    }
    if (button->label[0]) {
        axui_draw_text(surface, start_x, centre_y, button->label,
                       axui_style(text, (u8)text_scale));
    }
    if (button->focused) {
        axau_paint_focus(surface, body, radius, palette);
    }
}

bool_t axau_button_hit(axau_button_t *button, i32 x, i32 y)
{
    if (!button) return (bool_t)0;
    bool_t inside = gui_rect_contains(button->rect, x, y);
    button->hover = (bool_t)(inside && button->enabled);
    return inside;
}

bool_t axau_button_down(axau_button_t *button, i32 x, i32 y)
{
    if (!button || !button->enabled) {
        return (bool_t)0;
    }
    if (!gui_rect_contains(button->rect, x, y)) {
        return (bool_t)0;
    }
    button->pressed = (bool_t)1;
    button->focused = (bool_t)1;
    return (bool_t)1;
}

bool_t axau_button_up(axau_button_t *button, i32 x, i32 y)
{
    if (!button) return (bool_t)0;
    bool_t was_pressed = button->pressed;
    button->pressed = (bool_t)0;
    if (!was_pressed) {
        return (bool_t)0;
    }
    return (bool_t)gui_rect_contains(button->rect, x, y);
}

/* ------------------------------------------------------------------ */
/* Checkbox                                                            */
/* ------------------------------------------------------------------ */

void axau_checkbox_paint(gui_surface_t *surface, axau_checkbox_t *box,
                         const axau_palette_t *palette,
                         const axau_metrics_t *metrics)
{
    if (!surface || !box) return;
    i32 size = 20;
    if (metrics && metrics->scale_percent >= 150) {
        size = 24;
    }
    i32 radius = 3;
    gui_rect_t mark = gui_rect(box->rect.x, box->rect.y + (box->rect.h - size) / 2,
                               size, size);
    u32 accent = axau_colour(palette, AXAU_COLOUR_ACCENT);
    if (box->checked || box->indeterminate) {
        u32 fill = box->enabled ? accent : axau_colour(palette, AXAU_COLOUR_CONTROL_DISABLED);
        axui_round_rect_aa(surface, mark, radius, fill);
    } else {
        u32 fill = axau_colour(palette, box->hover ? AXAU_COLOUR_CONTROL_HOVER
                                                    : AXAU_COLOUR_CONTROL);
        axui_round_rect_aa(surface, mark, radius, fill);
        axui_round_rect_border(surface, mark, radius, 1,
                               axau_colour(palette, AXAU_COLOUR_STROKE_STRONG));
    }
    u32 tick = box->enabled ? axau_colour(palette, AXAU_COLOUR_ACCENT_TEXT)
                            : axau_colour(palette, AXAU_COLOUR_TEXT_DISABLED);
    if (box->indeterminate) {
        gui_fill_rect(surface, gui_rect(mark.x + 5, mark.y + size / 2 - 1,
                                        size - 10, 2), tick);
    } else if (box->checked) {
        /* A check mark drawn as two line segments; the short one is 60 percent
         * of the long one so the tick looks handwritten rather than geometric. */
        i32 x0 = mark.x + size / 5;
        i32 y0 = mark.y + size / 2;
        i32 x1 = mark.x + size * 2 / 5;
        i32 y1 = mark.y + size - size / 4;
        i32 x2 = mark.x + size - size / 5;
        i32 y2 = mark.y + size / 4;
        axui_line_aa(surface, x0, y0, x1, y1, tick, 2);
        axui_line_aa(surface, x1, y1, x2, y2, tick, 2);
    }
    if (box->label[0]) {
        u32 text = box->enabled ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                : axau_colour(palette, AXAU_COLOUR_TEXT_DISABLED);
        axui_draw_text(surface, mark.x + size + 12,
                       axau_text_center_y(box->rect, 1), box->label,
                       axui_style(text, 1));
    }
    if (box->focused) {
        axau_paint_focus(surface, mark, radius, palette);
    }
}

bool_t axau_checkbox_hit(axau_checkbox_t *box, i32 x, i32 y)
{
    if (!box) return (bool_t)0;
    bool_t inside = gui_rect_contains(box->rect, x, y);
    box->hover = (bool_t)(inside && box->enabled);
    return inside;
}

void axau_checkbox_toggle(axau_checkbox_t *box, i32 x, i32 y)
{
    if (!box || !box->enabled) return;
    if (!gui_rect_contains(box->rect, x, y)) return;
    box->checked = (bool_t)!box->checked;
    box->indeterminate = (bool_t)0;
}

/* ------------------------------------------------------------------ */
/* Radio                                                               */
/* ------------------------------------------------------------------ */

void axau_radio_paint(gui_surface_t *surface, axau_radio_t *radio,
                      const axau_palette_t *palette,
                      const axau_metrics_t *metrics)
{
    if (!surface || !radio) return;
    i32 size = 20;
    if (metrics && metrics->scale_percent >= 150) {
        size = 24;
    }
    i32 radius = size / 2;
    i32 cx = radio->rect.x + radius;
    i32 cy = radio->rect.y + radio->rect.h / 2;
    u32 stroke = axau_colour(palette, AXAU_COLOUR_STROKE_STRONG);
    u32 accent = axau_colour(palette, AXAU_COLOUR_ACCENT);
    if (radio->selected) {
        axui_circle(surface, cx, cy, radius, accent);
        axui_circle(surface, cx, cy, radius - 3,
                    axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
        axui_circle(surface, cx, cy, radius - 6, accent);
    } else {
        u32 fill = axau_colour(palette, radio->hover ? AXAU_COLOUR_CONTROL_HOVER
                                                      : AXAU_COLOUR_CONTROL);
        axui_circle(surface, cx, cy, radius, fill);
        axui_circle_outline(surface, cx, cy, radius, 1, stroke);
    }
    if (radio->label[0]) {
        u32 text = radio->enabled ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                  : axau_colour(palette, AXAU_COLOUR_TEXT_DISABLED);
        axui_draw_text(surface, radio->rect.x + size + 12,
                       axau_text_center_y(radio->rect, 1), radio->label,
                       axui_style(text, 1));
    }
    if (radio->focused) {
        axui_circle_outline(surface, cx, cy, radius + 2, 2,
                            axau_colour(palette, AXAU_COLOUR_FOCUS));
    }
}

bool_t axau_radio_hit(axau_radio_t *radio, i32 x, i32 y)
{
    if (!radio) return (bool_t)0;
    bool_t inside = gui_rect_contains(radio->rect, x, y);
    radio->hover = (bool_t)(inside && radio->enabled);
    if (inside && radio->enabled && gui_rect_contains(radio->rect, x, y)) {
        radio->selected = (bool_t)1;
        radio->focused = (bool_t)1;
    }
    return inside;
}

/* ------------------------------------------------------------------ */
/* Toggle switch                                                       */
/* ------------------------------------------------------------------ */

void axau_toggle_paint(gui_surface_t *surface, axau_toggle_t *toggle,
                       const axau_palette_t *palette,
                       const axau_metrics_t *metrics)
{
    if (!surface || !toggle) return;
    i32 width = 40;
    i32 height = 20;
    if (metrics && metrics->scale_percent >= 150) {
        width = 48;
        height = 24;
    }
    i32 thumb = height - 4;
    gui_rect_t track = gui_rect(toggle->rect.x, toggle->rect.y +
                                (toggle->rect.h - height) / 2, width, height);
    u32 accent = axau_colour(palette, AXAU_COLOUR_ACCENT);
    u32 off = axau_colour(palette, AXAU_COLOUR_CONTROL);
    u32 fill = toggle->on ? accent : off;
    if (!toggle->enabled) {
        fill = axau_colour(palette, AXAU_COLOUR_CONTROL_DISABLED);
    }
    axui_round_rect_aa(surface, track, height / 2, fill);
    axui_round_rect_border(surface, track, height / 2, 1,
                           axau_colour(palette, AXAU_COLOUR_STROKE));

    /* Thumb position interpolates with thumb_anim, 0 = off, 100 = on. */
    i32 anim = toggle->thumb_anim;
    if (anim < 0) anim = 0;
    if (anim > 100) anim = 100;
    i32 travel = width - thumb - 4;
    i32 thumb_x = track.x + 2 + (travel * anim) / 100;
    i32 thumb_y = track.y + 2;
    u32 thumb_colour = toggle->enabled ? GUI_RGB(0xFF, 0xFF, 0xFF)
                                       : axau_colour(palette, AXAU_COLOUR_TEXT_DISABLED);
    axui_circle(surface, thumb_x + thumb / 2, thumb_y + thumb / 2, thumb / 2,
                thumb_colour);
    if (toggle->on) {
        /* Inline state label, shown only when the caller supplied one. */
        const char *label = toggle->on_label[0] ? toggle->on_label : 0;
        if (label) {
            axui_draw_text(surface, track.x + width + 12,
                           axau_text_center_y(toggle->rect, 1), label,
                           axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 1));
        }
    } else if (toggle->off_label[0]) {
        axui_draw_text(surface, track.x + width + 12,
                       axau_text_center_y(toggle->rect, 1), toggle->off_label,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
    if (toggle->focused) {
        axui_round_rect_border(surface,
                               gui_rect(track.x - 2, track.y - 2, track.w + 4,
                                        track.h + 4),
                               height / 2 + 2, 2,
                               axau_colour(palette, AXAU_COLOUR_FOCUS));
    }
}

bool_t axau_toggle_hit(axau_toggle_t *toggle, i32 x, i32 y)
{
    if (!toggle) return (bool_t)0;
    gui_rect_t area = gui_rect(toggle->rect.x, toggle->rect.y, 40, toggle->rect.h);
    bool_t inside = gui_rect_contains(area, x, y);
    toggle->hover = (bool_t)(inside && toggle->enabled);
    return inside;
}

bool_t axau_toggle_click(axau_toggle_t *toggle, i32 x, i32 y)
{
    if (!toggle || !toggle->enabled) return (bool_t)0;
    if (!axau_toggle_hit(toggle, x, y)) return (bool_t)0;
    toggle->on = (bool_t)!toggle->on;
    toggle->focused = (bool_t)1;
    toggle->thumb_anim = toggle->on ? 100 : 0;
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Slider                                                              */
/* ------------------------------------------------------------------ */

static i32 slider_track_left(const axau_slider_t *slider)
{
    return slider->rect.x + 8;
}

static i32 slider_track_width(const axau_slider_t *slider)
{
    return slider->rect.w - 16;
}

i32 axau_slider_from_x(const axau_slider_t *slider, i32 x)
{
    if (!slider) return 0;
    i32 range = slider->maximum - slider->minimum;
    if (range <= 0) return slider->minimum;
    i32 track = slider_track_width(slider);
    i32 offset = x - slider_track_left(slider);
    if (offset < 0) offset = 0;
    if (offset > track) offset = track;
    i32 value = slider->minimum + (offset * range) / (track ? track : 1);
    if (slider->step > 0) {
        i32 steps = (value - slider->minimum) / slider->step;
        value = slider->minimum + steps * slider->step;
    }
    if (value < slider->minimum) value = slider->minimum;
    if (value > slider->maximum) value = slider->maximum;
    return value;
}

void axau_slider_paint(gui_surface_t *surface, axau_slider_t *slider,
                       const axau_palette_t *palette,
                       const axau_metrics_t *metrics)
{
    if (!surface || !slider) return;
    i32 track_y = slider->rect.y + slider->rect.h / 2;
    i32 track_w = slider_track_width(slider);
    i32 range = slider->maximum - slider->minimum;
    i32 fraction = range > 0 ? ((slider->value - slider->minimum) * 1000) / range : 0;
    i32 filled = (track_w * fraction) / 1000;

    u32 track_colour = axau_colour(palette, AXAU_COLOUR_STROKE_STRONG);
    u32 fill_colour = axau_colour(palette, slider->enabled ? AXAU_COLOUR_ACCENT
                                                            : AXAU_COLOUR_CONTROL_DISABLED);
    gui_fill_rect(surface, gui_rect(slider_track_left(slider), track_y - 2,
                                    track_w, 4), track_colour);
    if (filled > 0) {
        gui_fill_rect(surface, gui_rect(slider_track_left(slider), track_y - 2,
                                        filled, 4), fill_colour);
    }
    i32 thumb_x = slider_track_left(slider) + filled;
    i32 thumb_radius = slider->dragging ? 9 : 8;
    if (metrics && metrics->scale_percent >= 150) {
        thumb_radius = slider->dragging ? 11 : 10;
    }
    axui_circle(surface, thumb_x, track_y, thumb_radius,
                axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
    axui_circle_outline(surface, thumb_x, track_y, thumb_radius, 1,
                        axau_colour(palette, AXAU_COLOUR_STROKE_STRONG));
    axui_circle(surface, thumb_x, track_y, thumb_radius - 4,
                slider->enabled ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                                : axau_colour(palette, AXAU_COLOUR_CONTROL_DISABLED));
    if (slider->show_value) {
        char text[16];
        ax_snprintf(text, sizeof(text), "%d", slider->value);
        axui_draw_text_right(surface,
                             gui_rect(slider->rect.x, slider->rect.y - 20,
                                      slider->rect.w, 16),
                             text,
                             axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 1));
    }
    if (slider->focused) {
        axui_circle_outline(surface, thumb_x, track_y, thumb_radius + 2, 2,
                            axau_colour(palette, AXAU_COLOUR_FOCUS));
    }
}

bool_t axau_slider_hit(axau_slider_t *slider, i32 x, i32 y)
{
    if (!slider) return (bool_t)0;
    bool_t inside = gui_rect_contains(slider->rect, x, y);
    slider->hover = (bool_t)(inside && slider->enabled);
    return inside;
}

bool_t axau_slider_drag(axau_slider_t *slider, i32 x, i32 y)
{
    if (!slider || !slider->enabled) return (bool_t)0;
    if (!slider->dragging && !gui_rect_contains(slider->rect, x, y)) {
        return (bool_t)0;
    }
    slider->dragging = (bool_t)1;
    slider->focused = (bool_t)1;
    slider->value = axau_slider_from_x(slider, x);
    (void)y;
    return (bool_t)1;
}

void axau_slider_release(axau_slider_t *slider)
{
    if (!slider) return;
    slider->dragging = (bool_t)0;
}

/* ------------------------------------------------------------------ */
/* Progress                                                            */
/* ------------------------------------------------------------------ */

void axau_progress_tick(axau_progress_t *progress, i32 dt_ms)
{
    if (!progress || progress->paused) return;
    progress->phase = (progress->phase + dt_ms * 3) % 360;
    if (progress->percent < 0) {
        /* Indeterminate: the phase drives a travelling arc in paint. */
    }
}

void axau_progress_paint(gui_surface_t *surface, axau_progress_t *progress,
                         const axau_palette_t *palette)
{
    if (!surface || !progress) return;
    u32 accent = axau_colour(palette, AXAU_COLOUR_ACCENT);
    u32 track = axau_colour(palette, AXAU_COLOUR_STROKE);
    if (progress->ring) {
        i32 radius = (progress->rect.w < progress->rect.h
                          ? progress->rect.w : progress->rect.h) / 2 - 2;
        i32 cx = progress->rect.x + progress->rect.w / 2;
        i32 cy = progress->rect.y + progress->rect.h / 2;
        axui_circle_outline(surface, cx, cy, radius, 3, track);
        if (progress->percent >= 0) {
            i32 sweep = (progress->percent * 360) / 100;
            axui_arc(surface, cx, cy, radius, 0, sweep, 3, accent);
        } else {
            i32 start = progress->phase;
            axui_arc(surface, cx, cy, radius, start, start + 90, 3, accent);
        }
        return;
    }
    i32 bar_height = 4;
    gui_rect_t bar = gui_rect(progress->rect.x, progress->rect.y +
                              (progress->rect.h - bar_height) / 2,
                              progress->rect.w, bar_height);
    axui_round_rect_aa(surface, bar, bar_height / 2, track);
    if (progress->percent >= 0) {
        i32 width = (bar.w * progress->percent) / 100;
        if (width > 0) {
            axui_round_rect_aa(surface, gui_rect(bar.x, bar.y, width, bar.h),
                               bar_height / 2, accent);
        }
    } else {
        /* Travelling segment: 30 percent of the track, bouncing left to right. */
        i32 segment = bar.w * 3 / 10;
        i32 travel = bar.w - segment;
        i32 cycle = progress->phase * 2;
        i32 position = cycle < 360 ? (travel * cycle) / 360
                                   : (travel * (720 - cycle)) / 360;
        axui_round_rect_aa(surface, gui_rect(bar.x + position, bar.y, segment,
                                             bar.h), bar_height / 2, accent);
    }
}

/* ------------------------------------------------------------------ */
/* Chip                                                                */
/* ------------------------------------------------------------------ */

void axau_chip_paint(gui_surface_t *surface, axau_chip_t *chip,
                     const axau_palette_t *palette)
{
    if (!surface || !chip) return;
    u32 fill = chip->selected
                   ? axui_colour_opacity(axau_colour(palette, AXAU_COLOUR_ACCENT), 18)
                   : axau_colour(palette, chip->hover ? AXAU_COLOUR_CONTROL_HOVER
                                                       : AXAU_COLOUR_CONTROL);
    axui_round_rect_aa(surface, chip->rect, chip->rect.h / 2, fill);
    axui_round_rect_border(surface, chip->rect, chip->rect.h / 2, 1,
                           chip->selected ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                                          : axau_colour(palette, AXAU_COLOUR_STROKE));
    u32 text = chip->selected ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                              : axau_colour(palette, AXAU_COLOUR_TEXT);
    axui_draw_text_center(surface, chip->rect, chip->label, axui_style(text, 1));
    if (chip->dismissible) {
        i32 size = 10;
        i32 cx = chip->rect.x + chip->rect.w - 12;
        i32 cy = chip->rect.y + chip->rect.h / 2;
        axui_line_aa(surface, cx - size / 2, cy - size / 2, cx + size / 2,
                     cy + size / 2,
                     axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 1);
        axui_line_aa(surface, cx - size / 2, cy + size / 2, cx + size / 2,
                     cy - size / 2,
                     axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 1);
    }
}

bool_t axau_chip_hit(axau_chip_t *chip, i32 x, i32 y)
{
    if (!chip) return (bool_t)0;
    bool_t inside = gui_rect_contains(chip->rect, x, y);
    chip->hover = inside;
    if (inside) {
        chip->selected = (bool_t)!chip->selected;
    }
    return inside;
}

/* ------------------------------------------------------------------ */
/* Textbox                                                             */
/* ------------------------------------------------------------------ */

void axau_textbox_clear(axau_textbox_t *box)
{
    if (!box) return;
    ax_memset(box->text, 0, sizeof(box->text));
    box->cursor = 0;
    box->scroll = 0;
}

void axau_textbox_paint(gui_surface_t *surface, axau_textbox_t *box,
                        const axau_palette_t *palette,
                        const axau_metrics_t *metrics)
{
    if (!surface || !box) return;
    i32 radius = metrics ? metrics->radius_control : AXAU_RADIUS_CONTROL;
    u32 fill = axau_colour(palette, box->hover ? AXAU_COLOUR_CONTROL_HOVER
                                                : AXAU_COLOUR_CONTROL);
    if (box->focused) {
        fill = axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED);
    }
    axui_round_rect_aa(surface, box->rect, radius, fill);
    u32 border = box->error ? axau_colour(palette, AXAU_COLOUR_ERROR)
                            : (box->focused ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                                            : axau_colour(palette, AXAU_COLOUR_STROKE));
    axui_round_rect_border(surface, box->rect, radius, 1, border);

    i32 padding = 10;
    gui_rect_t inner = gui_rect(box->rect.x + padding, box->rect.y,
                                box->rect.w - padding * 2, box->rect.h);
    u32 text_colour = axau_colour(palette, AXAU_COLOUR_TEXT);
    const char *content = box->text;
    if (box->password && content[0]) {
        /* Password mask: one bullet per character, capped so the clip is not
         * asked to draw past the control. */
        static char mask[128];
        usize length = ax_strlen(content);
        if (length >= sizeof(mask)) length = sizeof(mask) - 1;
        for (usize i = 0; i < length; i++) {
            mask[i] = '*';
        }
        mask[length] = 0;
        content = mask;
    }
    if (content[0]) {
        axui_draw_text_clip(surface, inner, inner.x,
                            axau_text_center_y(inner, 1), content,
                            axui_style(text_colour, 1));
    } else if (box->placeholder[0]) {
        axui_draw_text_clip(surface, inner, inner.x,
                            axau_text_center_y(inner, 1), box->placeholder,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
    if (box->focused) {
        i32 caret_x = inner.x + axui_measure_text(content, 1) - box->scroll;
        if (caret_x < inner.x) caret_x = inner.x;
        gui_fill_rect(surface, gui_rect(caret_x, inner.y + 6, 1, inner.h - 12),
                      axau_colour(palette, AXAU_COLOUR_TEXT));
    }
}

bool_t axau_textbox_hit(axau_textbox_t *box, i32 x, i32 y)
{
    if (!box) return (bool_t)0;
    bool_t inside = gui_rect_contains(box->rect, x, y);
    box->focused = inside;
    return inside;
}

bool_t axau_textbox_key(axau_textbox_t *box, char key)
{
    if (!box || !box->focused) return (bool_t)0;
    usize length = ax_strlen(box->text);
    if (key == 8) {
        if (length > 0) {
            box->text[length - 1] = 0;
            box->cursor = (u32)(length - 1);
        }
        return (bool_t)1;
    }
    if (key < 0x20 || key > 0x7E) {
        return (bool_t)0;
    }
    if (length + 1 >= sizeof(box->text)) {
        return (bool_t)0;
    }
    box->text[length] = key;
    box->text[length + 1] = 0;
    box->cursor = (u32)(length + 1);
    return (bool_t)1;
}
