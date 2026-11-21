/*
 * AxiomOS - widget toolkit.
 *
 * Widgets are stateless renderers plus tiny event helpers; applications keep
 * the state.  Keeping them free of allocation makes them usable from the boot
 * splash before the heap is fully grown.
 */

#include <gui.h>

void gui_widget_button(gui_surface_t *surface, gui_theme_t *theme,
                       gui_button_t *button)
{
    u32 base = button->primary ? theme->accent
               : (button->hover ? theme->taskbar_hover : theme->window_bg_alt);
    u32 text = button->primary ? theme->text_inverse : theme->text;
    if (button->pressed)
        base = button->primary ? theme->accent_dim : theme->selection;

    gui_fill_round_rect(surface, button->rect, 6, base);
    gui_rect_outline(surface,
                     gui_rect(button->rect.x, button->rect.y,
                              button->rect.w, button->rect.h),
                     button->primary ? theme->accent : theme->border);
    i32 text_width = gui_text_width(button->label, 1);
    i32 x = button->rect.x + (button->rect.w - text_width) / 2;
    i32 y = button->rect.y + (button->rect.h - gui_font_height) / 2;
    gui_text(surface, x, y, button->label, text, 1);
}

bool_t gui_widget_button_event(gui_button_t *button, i32 x, i32 y, u8 buttons,
                               bool_t pressed)
{
    bool_t inside = gui_rect_contains(button->rect, x, y);
    button->hover = inside;
    if (inside && pressed && (buttons & 1)) {
        button->pressed = true;
        return false;
    }
    if (!pressed && button->pressed) {
        button->pressed = false;
        return inside;
    }
    return false;
}

void gui_widget_progress(gui_surface_t *surface, gui_rect_t rect, i32 percent,
                         u32 fill, u32 track)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    gui_fill_round_rect(surface, rect, rect.h / 2, track);
    i32 width = (rect.w * percent) / 100;
    if (width > 0)
        gui_fill_round_rect(surface,
                            gui_rect(rect.x, rect.y, width, rect.h),
                            rect.h / 2, fill);
}

void gui_widget_checkbox(gui_surface_t *surface, i32 x, i32 y, bool_t checked,
                         const char *label, u32 text_colour, u32 accent)
{
    gui_rect_t box = gui_rect(x, y, 14, 14);
    gui_fill_round_rect(surface, box, 3,
                        checked ? accent : GUI_RGB(0x1B, 0x23, 0x3B));
    gui_rect_outline(surface, box, GUI_RGB(0x35, 0x40, 0x60));
    if (checked) {
        gui_line(surface, x + 3, y + 7, x + 6, y + 10, GUI_RGB(0x08, 0x10, 0x20));
        gui_line(surface, x + 6, y + 10, x + 11, y + 4, GUI_RGB(0x08, 0x10, 0x20));
    }
    gui_text(surface, x + 22, y - 1, label, text_colour, 1);
}

void gui_widget_slider(gui_surface_t *surface, gui_rect_t rect, i32 value,
                       i32 maximum, u32 accent, u32 track)
{
    if (maximum <= 0)
        maximum = 1;
    if (value < 0)
        value = 0;
    if (value > maximum)
        value = maximum;
    i32 mid = rect.y + rect.h / 2;
    gui_fill_round_rect(surface, gui_rect(rect.x, mid - 2, rect.w, 4), 2, track);
    i32 filled = (rect.w * value) / maximum;
    if (filled > 2)
        gui_fill_round_rect(surface, gui_rect(rect.x, mid - 2, filled, 4), 2, accent);
    i32 knob_x = rect.x + filled - 6;
    gui_fill_round_rect(surface, gui_rect(knob_x, mid - 6, 12, 12), 6,
                        GUI_RGB(0xF2, 0xF6, 0xFF));
}

void gui_widget_scrollbar(gui_surface_t *surface, gui_rect_t rect, i32 offset,
                          i32 total, i32 visible, u32 track, u32 thumb)
{
    gui_fill_round_rect(surface, rect, 3, track);
    if (total <= visible)
        return;
    i32 height = rect.h * visible / total;
    if (height < 18)
        height = 18;
    i32 max_offset = total - visible;
    i32 y = rect.y + (rect.h - height) * offset / (max_offset ? max_offset : 1);
    gui_fill_round_rect(surface, gui_rect(rect.x + 1, y, rect.w - 2, height), 3,
                        thumb);
}

void gui_widget_panel(gui_surface_t *surface, gui_rect_t rect, u32 fill,
                      u32 border, i32 radius)
{
    gui_fill_round_rect(surface, rect, radius, fill);
    gui_rect_outline(surface, rect, border);
}

void gui_widget_badge(gui_surface_t *surface, i32 x, i32 y, const char *text,
                      u32 fill, u32 text_colour)
{
    i32 width = gui_text_width(text, 1) + 12;
    gui_fill_round_rect(surface, gui_rect(x, y, width, 16), 8, fill);
    gui_text(surface, x + 6, y + 0, text, text_colour, 1);
}
