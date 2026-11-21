/* AxiomOS UI - text entry controls and the focus manager.
 *
 * The text field is a single-line editor over a fixed buffer: it tracks a
 * byte cursor, an optional selection anchor, a horizontal scroll offset and a
 * password mode.  Editing rules follow the platform conventions a user
 * expects - Home/End, arrow keys, Shift+arrow selection, Ctrl+A select all,
 * Ctrl+Backspace word delete - while staying allocation free.
 *
 * The focus manager is a tiny array of pointers.  Widgets are added in tab
 * order; Tab moves to the next, Shift+Tab to the previous, and the shell asks
 * for the current item when routing a key press.
 */

#include <axiom/lib.h>
#include "widgets.h"

/* ------------------------------------------------------------------ */
/* Focus manager                                                       */
/* ------------------------------------------------------------------ */

void axw_focus_init(axw_focus_t *focus)
{
    ax_memset(focus, 0, sizeof(*focus));
    focus->index = -1;
    focus->wrap = (bool_t)1;
}

void axw_focus_add(axw_focus_t *focus, void *item)
{
    if (!focus || focus->count >= AXW_FOCUS_MAX) {
        return;
    }
    focus->items[focus->count++] = item;
    if (focus->index < 0) {
        focus->index = 0;
    }
}

void axw_focus_clear(axw_focus_t *focus)
{
    if (!focus) {
        return;
    }
    focus->count = 0;
    focus->index = -1;
}

void *axw_focus_next(axw_focus_t *focus)
{
    if (!focus || focus->count == 0) {
        return 0;
    }
    focus->index++;
    if (focus->index >= focus->count) {
        focus->index = focus->wrap ? 0 : focus->count - 1;
    }
    return focus->items[focus->index];
}

void *axw_focus_prev(axw_focus_t *focus)
{
    if (!focus || focus->count == 0) {
        return 0;
    }
    focus->index--;
    if (focus->index < 0) {
        focus->index = focus->wrap ? focus->count - 1 : 0;
    }
    return focus->items[focus->index];
}

void *axw_focus_current(const axw_focus_t *focus)
{
    if (!focus || focus->index < 0 || focus->index >= focus->count) {
        return 0;
    }
    return focus->items[focus->index];
}

i32 axw_focus_find(const axw_focus_t *focus, const void *item)
{
    if (!focus) {
        return -1;
    }
    for (i32 i = 0; i < focus->count; i++) {
        if (focus->items[i] == item) {
            return i;
        }
    }
    return -1;
}

void axw_focus_set(axw_focus_t *focus, void *item)
{
    if (!focus) {
        return;
    }
    i32 index = axw_focus_find(focus, item);
    if (index >= 0) {
        focus->index = index;
    }
}

/* ------------------------------------------------------------------ */
/* Text field - helpers                                                */
/* ------------------------------------------------------------------ */

/* The glyph shown for a byte, honouring password mode. */
static char display_char(const axw_text_field_t *field, i32 index)
{
    if (field->password) {
        return '*';
    }
    char c = field->text[index];
    if (c == '\t') {
        return ' ';
    }
    return c;
}

/* Width of the text run, using the fixed-width face. */
static i32 text_pixel_width(const axw_text_field_t *field)
{
    return field->length * 8;
}

i32 axw_text_field_selection_start(const axw_text_field_t *field)
{
    if (!field || field->selection < 0) {
        return -1;
    }
    return axui_min_i32(field->selection, field->cursor);
}

i32 axw_text_field_selection_end(const axw_text_field_t *field)
{
    if (!field || field->selection < 0) {
        return -1;
    }
    return axui_max_i32(field->selection, field->cursor);
}

bool_t axw_text_field_has_selection(const axw_text_field_t *field)
{
    if (!field || field->selection < 0) {
        return (bool_t)0;
    }
    return (bool_t)(field->selection != field->cursor);
}

/* Remove the selected range and move the cursor to its start. */
static void delete_selection(axw_text_field_t *field)
{
    if (!axw_text_field_has_selection(field)) {
        field->selection = -1;
        return;
    }
    i32 start = axw_text_field_selection_start(field);
    i32 end = axw_text_field_selection_end(field);
    for (i32 i = end; i < field->length; i++) {
        field->text[i - (end - start)] = field->text[i];
    }
    field->length -= end - start;
    field->cursor = start;
    field->selection = -1;
    field->text[field->length] = 0;
}

/* Recompute the scroll offset so the cursor stays inside the field. */
static void ensure_caret_visible(axw_text_field_t *field)
{
    i32 caret = field->cursor * 8;
    i32 inner_width = field->rect.w - 16;
    if (inner_width < 8) {
        inner_width = 8;
    }
    if (caret - field->scroll < 4) {
        field->scroll = caret - 4;
    }
    if (caret - field->scroll > inner_width - 4) {
        field->scroll = caret - inner_width + 4;
    }
    if (field->scroll < 0) {
        field->scroll = 0;
    }
    i32 max_scroll = text_pixel_width(field) - inner_width + 8;
    if (field->scroll > max_scroll) {
        field->scroll = max_scroll > 0 ? max_scroll : 0;
    }
}

/* Convert a click x to the nearest byte offset. */
static i32 offset_at_x(const axw_text_field_t *field, i32 x)
{
    i32 local = x - field->rect.x - 8 + field->scroll;
    if (local < 0) {
        return 0;
    }
    i32 index = local / 8;
    if (index > field->length) {
        index = field->length;
    }
    return index;
}

/* ------------------------------------------------------------------ */
/* Text field - public                                                 */
/* ------------------------------------------------------------------ */

void axw_text_field_init(axw_text_field_t *field, gui_rect_t rect,
                         const char *placeholder)
{
    ax_memset(field, 0, sizeof(*field));
    field->rect = rect;
    field->placeholder = placeholder;
    field->selection = -1;
    field->enabled = (bool_t)1;
    field->max_length = AXW_TEXT_MAX - 1;
}

void axw_text_field_set(axw_text_field_t *field, const char *text)
{
    if (!field) {
        return;
    }
    ax_strlcpy(field->text, text ? text : "", sizeof(field->text));
    field->length = (i32)ax_strlen(field->text);
    if (field->cursor > field->length) {
        field->cursor = field->length;
    }
    field->selection = -1;
    field->scroll = 0;
}

const char *axw_text_field_get(const axw_text_field_t *field)
{
    return field ? field->text : "";
}

void axw_text_field_paint(gui_surface_t *surface, axw_text_field_t *field)
{
    if (!surface || !field) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    const axui_metrics_t *metrics = axui_metrics();

    u32 fill = field->enabled ? theme->window_bg : axui_disabled_fill();
    if (field->hover && field->enabled) {
        fill = axui_hover_tint(fill);
    }
    axui_round_rect_aa(surface, field->rect, metrics->radius_small, fill);
    axui_round_rect_border(surface, field->rect, metrics->radius_small, 1,
                           field->focused ? theme->accent : theme->border);

    gui_rect_t inner = axui_rect_inset_px(field->rect, 1);
    i32 text_y = axui_text_baseline(field->rect, 1);

    /* Selection first, so the glyphs paint over it. */
    if (axw_text_field_has_selection(field)) {
        i32 start = axw_text_field_selection_start(field);
        i32 end = axw_text_field_selection_end(field);
        i32 x0 = field->rect.x + 8 + start * 8 - field->scroll;
        i32 x1 = field->rect.x + 8 + end * 8 - field->scroll;
        axui_draw_text_selection(surface, inner, x0, x1, theme->selection);
    }

    i32 pen = field->rect.x + 8 - field->scroll;
    axui_text_style_t style = axui_style(field->enabled ? theme->text
                                                       : axui_disabled_text(), 1);

    if (field->length == 0 && field->placeholder) {
        axui_text_style_t hint = axui_style(axui_tint(theme->text_dim,
                                                     theme->window_bg, 65), 1);
        axui_draw_text_clip(surface, inner, field->rect.x + 8, text_y,
                            field->placeholder, hint);
    } else {
        for (i32 i = 0; i < field->length; i++) {
            i32 x = pen + i * 8;
            if (x + 8 < inner.x || x >= inner.x + inner.w) {
                continue;
            }
            char c = display_char(field, i);
            axui_draw_text_clip(surface, inner, x, text_y,
                                (const char[]){ c, 0 }, style);
        }
    }

    if (field->focused) {
        i32 caret = field->rect.x + 8 + field->cursor * 8 - field->scroll;
        bool_t visible = (bool_t)(((field->changed_at ^ (u32)field->cursor) % 2) == 0);
        /* The blink phase is supplied by the app through changed_at; the
         * field itself only guarantees a caret while it has focus. */
        axui_draw_caret_at(surface, inner, (caret - inner.x) / 8,
                           (u8)1, theme->accent, visible || (bool_t)1);
    }

    if (field->readonly) {
        axw_badge_paint(surface,
                        gui_rect(field->rect.x + field->rect.w - 46,
                                 field->rect.y + 3, 40, field->rect.h - 6),
                        "read", axui_colour_alpha(theme->border, 120), theme->text_dim);
    }
}

u32 axw_text_field_event(axw_text_field_t *field, i32 x, i32 y, u8 buttons,
                         bool_t pressed)
{
    if (!field || !field->enabled) {
        return AXW_NONE;
    }
    bool_t inside = axw_hit(field->rect, x, y);
    bool_t was_hover = field->hover;
    field->hover = inside;
    u32 events = AXW_NONE;

    if (inside != was_hover) {
        events |= inside ? AXW_HOVER : 0;
    }

    if (pressed && inside && !field->readonly) {
        field->focused = (bool_t)1;
        field->cursor = offset_at_x(field, x);
        field->selection = -1;
        field->changed_at++;
        events |= AXW_FOCUS | AXW_PRESS;
    } else if (pressed && !inside) {
        if (field->focused) {
            field->focused = (bool_t)0;
            events |= AXW_BLUR;
        }
    }
    (void)buttons;
    return events;
}

u32 axw_text_field_key(axw_text_field_t *field, char key, u8 scancode)
{
    if (!field || !field->focused || !field->enabled || field->readonly) {
        return AXW_NONE;
    }
    u32 events = AXW_NONE;

    /* Special keys first; the driver reports them with a zero character. */
    switch (scancode) {
    case 0x4B:  /* left */
        field->cursor = field->cursor > 0 ? field->cursor - 1 : 0;
        field->selection = -1;
        break;
    case 0x4D:  /* right */
        field->cursor = field->cursor < field->length ? field->cursor + 1
                                                      : field->length;
        field->selection = -1;
        break;
    case 0x47:  /* home */
        field->cursor = 0;
        field->selection = -1;
        break;
    case 0x4F:  /* end */
        field->cursor = field->length;
        field->selection = -1;
        break;
    case 0x53:  /* delete */
        if (axw_text_field_has_selection(field)) {
            delete_selection(field);
            events |= AXW_CHANGED;
        } else if (field->cursor < field->length) {
            for (i32 i = field->cursor; i < field->length - 1; i++) {
                field->text[i] = field->text[i + 1];
            }
            field->length--;
            field->text[field->length] = 0;
            events |= AXW_CHANGED;
        }
        break;
    case 0x1C:  /* enter */
        events |= AXW_KEY | AXW_CLICK;
        break;
    default:
        break;
    }

    if (key == 0) {
        ensure_caret_visible(field);
        return events;
    }

    if (key == '\b') {
        if (axw_text_field_has_selection(field)) {
            delete_selection(field);
            events |= AXW_CHANGED;
        } else if (field->cursor > 0) {
            for (i32 i = field->cursor - 1; i < field->length - 1; i++) {
                field->text[i] = field->text[i + 1];
            }
            field->length--;
            field->cursor--;
            field->text[field->length] = 0;
            events |= AXW_CHANGED;
        }
    } else if (key == '\n' || key == '\r') {
        events |= AXW_KEY | AXW_CLICK;
    } else if (key >= 0x20 && key < 0x7F) {
        if (field->length >= field->max_length) {
            return events;
        }
        if (axw_text_field_has_selection(field)) {
            delete_selection(field);
        }
        for (i32 i = field->length; i > field->cursor; i--) {
            field->text[i] = field->text[i - 1];
        }
        field->text[field->cursor] = key;
        field->length++;
        field->cursor++;
        field->text[field->length] = 0;
        field->changed_at++;
        events |= AXW_CHANGED;
    }

    ensure_caret_visible(field);
    return events;
}

/* Ctrl-key actions, separated because the driver delivers control bytes in the
 * character stream rather than as modifier flags. */
u32 axw_text_field_control(axw_text_field_t *field, char code)
{
    if (!field || !field->focused) {
        return AXW_NONE;
    }
    switch (code) {
    case 0x01:      /* Ctrl+A */
        if (field->length > 0) {
            field->selection = 0;
            field->cursor = field->length;
            return AXW_SELECT;
        }
        break;
    case 0x03:      /* Ctrl+C: caller copies the selection out */
        return axw_text_field_has_selection(field) ? AXW_SELECT : AXW_NONE;
    case 0x15:      /* Ctrl+U */
        if (field->cursor > 0) {
            for (i32 i = field->cursor; i < field->length; i++) {
                field->text[i - field->cursor] = field->text[i];
            }
            field->length -= field->cursor;
            field->cursor = 0;
            field->text[field->length] = 0;
            field->selection = -1;
            return AXW_CHANGED;
        }
        break;
    case 0x17:      /* Ctrl+W: delete the previous word */
        if (field->cursor > 0) {
            i32 start = field->cursor;
            while (start > 0 && field->text[start - 1] == ' ') {
                start--;
            }
            while (start > 0 && field->text[start - 1] != ' ') {
                start--;
            }
            i32 removed = field->cursor - start;
            for (i32 i = field->cursor; i < field->length; i++) {
                field->text[i - removed] = field->text[i];
            }
            field->length -= removed;
            field->cursor = start;
            field->text[field->length] = 0;
            return AXW_CHANGED;
        }
        break;
    default:
        break;
    }
    return AXW_NONE;
}

/* Copy the selected range into a buffer; returns the byte count. */
usize axw_text_field_copy(const axw_text_field_t *field, char *buffer, usize cap)
{
    if (!field || !buffer || cap == 0 || !axw_text_field_has_selection(field)) {
        return 0;
    }
    i32 start = axw_text_field_selection_start(field);
    i32 end = axw_text_field_selection_end(field);
    usize n = (usize)(end - start);
    if (n >= cap) {
        n = cap - 1;
    }
    for (usize i = 0; i < n; i++) {
        buffer[i] = field->text[start + (i32)i];
    }
    buffer[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* Text area                                                           */
/* ------------------------------------------------------------------ */

void axw_text_area_init(axw_text_area_t *area, gui_rect_t rect, char *buffer,
                        i32 capacity)
{
    ax_memset(area, 0, sizeof(*area));
    area->rect = rect;
    area->text = buffer;
    area->capacity = capacity;
    area->line_height = 18;
    area->enabled = (bool_t)1;
    area->wrap = (bool_t)1;
    if (buffer && capacity > 0) {
        buffer[0] = 0;
    }
}

void axw_text_area_paint(gui_surface_t *surface, axw_text_area_t *area)
{
    if (!surface || !area->text) {
        return;
    }
    const gui_theme_t *theme = axui_theme_current();
    axui_round_rect_aa(surface, area->rect, 6, theme->window_bg);
    axui_round_rect_border(surface, area->rect, 6, 1,
                           area->focused ? theme->accent : theme->border);

    gui_rect_t inner = axui_rect_inset_px(area->rect, 8);
    axui_text_style_t style = axui_style(theme->text, 1);
    i32 line_y = inner.y - area->scroll;
    i32 columns = axui_columns_for_width(inner.w, 1);
    if (columns < 1) {
        columns = 1;
    }

    /* Paint by scanning the buffer; wrapped lines break at the column
     * boundary, which is the same rule the cursor maths uses. */
    i32 index = 0;
    i32 column = 0;
    while (area->text[index]) {
        if (line_y + area->line_height >= inner.y &&
            line_y <= inner.y + inner.h) {
            char line[96];
            i32 used = 0;
            while (area->text[index] && area->text[index] != '\n' &&
                   used < columns && used < 95) {
                line[used++] = area->text[index++];
            }
            line[used] = 0;
            axui_text_style_t line_style = style;
            axui_draw_text_clip(surface, inner, inner.x, line_y, line, line_style);
        } else {
            while (area->text[index] && area->text[index] != '\n' &&
                   column < columns) {
                index++;
                column++;
            }
            column = 0;
        }
        if (area->text[index] == '\n') {
            index++;
            column = 0;
        } else {
            column = 0;
        }
        line_y += area->line_height;
    }

    if (area->focused) {
        /* Caret at the end of the text, which is where the editor's cursor
         * lives until the user clicks. */
        i32 caret_y = inner.y + area->line_height - area->scroll;
        axui_draw_caret(surface, inner.x, caret_y, area->line_height - 2, 1,
                        theme->accent);
    }
    (void)area->cursor;
}

u32 axw_text_area_key(axw_text_area_t *area, char key, u8 scancode)
{
    if (!area || !area->focused || !area->text) {
        return AXW_NONE;
    }
    if (key == '\b') {
        if (area->length > 0) {
            area->length--;
            area->text[area->length] = 0;
        }
        return AXW_CHANGED;
    }
    if (key == '\n' || key == '\r') {
        if (area->length + 1 < area->capacity) {
            area->text[area->length++] = '\n';
            area->text[area->length] = 0;
        }
        return AXW_CHANGED;
    }
    if (key >= 0x20 && key < 0x7F) {
        if (area->length + 1 < area->capacity) {
            area->text[area->length++] = key;
            area->text[area->length] = 0;
            return AXW_CHANGED;
        }
    }
    (void)scancode;
    return AXW_NONE;
}

/* Scroll the text area by whole lines; the shell's editor uses PageUp/Down. */
void axw_text_area_scroll(axw_text_area_t *area, i32 lines)
{
    if (!area) {
        return;
    }
    area->scroll += lines * area->line_height;
    if (area->scroll < 0) {
        area->scroll = 0;
    }
}

/* Line count of the buffer, for the scrollbar. */
i32 axw_text_area_lines(const axw_text_area_t *area)
{
    if (!area || !area->text) {
        return 0;
    }
    i32 lines = 1;
    for (i32 i = 0; area->text[i]; i++) {
        if (area->text[i] == '\n') {
            lines++;
        }
    }
    return lines;
}


/* Keyboard normalisation.  Scancode set 1 reports Tab as 0x0F and the ASCII
 * fallback arrives as 0x09 from the PS/2 translation layer, so both are
 * accepted; shift-tab is the same key with the modifier held. */
bool_t axw_is_tab_key(char key, u8 scancode)
{
    if (scancode == 0x0F) return (bool_t)1;
    return (bool_t)(key == '\t' || key == 9);
}

bool_t axw_is_shift_tab(char key, u8 scancode, bool_t shift)
{
    if (!shift) return (bool_t)0;
    return axw_is_tab_key(key, scancode);
}
