/*
 * AxiomOS - Aurora dialogs and transient surfaces.
 *
 * Dialogs are modal by default and flyouts are not.  Both use the same island
 * material, but a dialog dims the surface behind it while a flyout leaves the
 * background readable, because a flyout is a continuation of the control that
 * opened it and a dialog is an interruption.
 *
 * The dialog stores its button rectangles so hit testing and painting cannot
 * disagree; they are recomputed in one layout pass at paint time and reused by
 * the hit test until the next paint.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define DIALOG_WIDTH 420
#define DIALOG_BUTTON_HEIGHT 32
#define MENU_ITEM_HEIGHT 32
#define MENU_WIDTH 220

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */
/* ------------------------------------------------------------------ */

static void dialog_layout_buttons(axau_dialog_t *dialog, i32 scale)
{
    i32 padding = 20 * scale;
    i32 gap = 8 * scale;
    i32 width = DIALOG_WIDTH * scale;
    i32 total = 0;
    for (i32 i = 0; i < dialog->button_count; i++) {
        i32 label = axui_measure_text(dialog->buttons[i], 1) + 32;
        if (label < 88) label = 88;
        total += label;
        if (i > 0) total += gap;
    }
    i32 x = dialog->rect.x + width - padding - total;
    i32 y = dialog->rect.y + dialog->rect.h - padding - DIALOG_BUTTON_HEIGHT * scale;
    for (i32 i = 0; i < dialog->button_count; i++) {
        i32 label = axui_measure_text(dialog->buttons[i], 1) + 32;
        if (label < 88) label = 88;
        dialog->button_rects[i] = gui_rect(x, y, label, DIALOG_BUTTON_HEIGHT * scale);
        x += label + gap;
    }
}

void axau_dialog_open(axau_dialog_t *dialog, const char *title, const char *body,
                      const char *primary, const char *secondary)
{
    if (!dialog) return;
    ax_memset(dialog, 0, sizeof(*dialog));
    ax_strlcpy(dialog->title, title ? title : "", sizeof(dialog->title));
    ax_strlcpy(dialog->body, body ? body : "", sizeof(dialog->body));
    if (primary && primary[0]) {
        ax_strlcpy(dialog->buttons[dialog->button_count++], primary,
                   sizeof(dialog->buttons[0]));
    }
    if (secondary && secondary[0]) {
        ax_strlcpy(dialog->buttons[dialog->button_count++], secondary,
                   sizeof(dialog->buttons[0]));
    }
    if (dialog->button_count == 0) {
        ax_strlcpy(dialog->buttons[dialog->button_count++], "Close",
                   sizeof(dialog->buttons[0]));
    }
    dialog->default_button = 0;
    dialog->hover_button = -1;
    dialog->open = (bool_t)1;
    dialog->modal = (bool_t)1;
}

void axau_dialog_paint(gui_surface_t *surface, axau_dialog_t *dialog,
                       const axau_palette_t *palette,
                       const axau_metrics_t *metrics)
{
    if (!surface || !dialog || !dialog->open) return;
    i32 scale = 1;
    if (metrics && metrics->scale_percent >= 150) scale = 2;
    i32 width = DIALOG_WIDTH * scale;
    i32 screen_w = surface->width;
    i32 screen_h = surface->height;
    i32 height = 200 * scale;
    if (ax_strlen(dialog->body) > 120) height += 40 * scale;
    dialog->rect = gui_rect((screen_w - width) / 2,
                            (screen_h - height) / 2 - 24, width, height);

    /* Dim the whole surface; a modal without a scrim looks like a large
     * floating card rather than something that must be answered. */
    gui_blend_rect(surface, gui_rect(0, 0, screen_w, screen_h),
                   GUI_ARGB(0x66, 0x00, 0x00, 0x00));
    axau_paint_island(surface, dialog->rect, AXAU_RADIUS_FLYOUT * scale, palette);

    i32 padding = 20 * scale;
    gui_rect_t title_rect = gui_rect(dialog->rect.x + padding,
                                     dialog->rect.y + padding,
                                     width - padding * 2, 32 * scale);
    axui_draw_text(surface, title_rect.x,
                   title_rect.y, dialog->title,
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT),
                              (u8)(scale > 1 ? 2 : 1)));

    gui_rect_t body_rect = gui_rect(dialog->rect.x + padding,
                                    dialog->rect.y + padding + 44 * scale,
                                    width - padding * 2, 72 * scale);
    axui_draw_text_wrapped(surface, body_rect, dialog->body,
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT_SECONDARY), 1));

    dialog_layout_buttons(dialog, scale);
    for (i32 i = 0; i < dialog->button_count; i++) {
        axau_button_t button;
        ax_memset(&button, 0, sizeof(button));
        button.rect = dialog->button_rects[i];
        ax_strlcpy(button.label, dialog->buttons[i], sizeof(button.label));
        button.enabled = (bool_t)1;
        button.accent = (bool_t)(i == dialog->default_button);
        button.hover = (bool_t)(i == dialog->hover_button);
        axau_button_paint(surface, &button, palette, metrics);
    }
}

i32 axau_dialog_hit(axau_dialog_t *dialog, i32 x, i32 y)
{
    if (!dialog || !dialog->open) return -1;
    for (i32 i = 0; i < dialog->button_count; i++) {
        if (gui_rect_contains(dialog->button_rects[i], x, y)) {
            dialog->hover_button = i;
            dialog->open = (bool_t)0;
            return i;
        }
    }
    dialog->hover_button = -1;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Flyouts                                                             */
/* ------------------------------------------------------------------ */

void axau_flyout_open(axau_flyout_t *flyout, i32 anchor_x, i32 anchor_y,
                      i32 width, i32 height, i32 screen_w, i32 screen_h)
{
    if (!flyout) return;
    ax_memset(flyout, 0, sizeof(*flyout));
    flyout->anchor_x = anchor_x;
    flyout->anchor_y = anchor_y;
    i32 x = anchor_x;
    i32 y = anchor_y;
    /* Prefer opening above the anchor when there is no room below; keep an 8 px
     * margin from every screen edge so the shadow is never clipped. */
    bool_t above = (bool_t)0;
    if (y + height + 8 > screen_h && y - height > 8) {
        y -= height;
        above = (bool_t)1;
    }
    if (x + width + 8 > screen_w) {
        x = screen_w - width - 8;
    }
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    flyout->rect = gui_rect(x, y, width, height);
    flyout->above = above;
    flyout->open = (bool_t)1;
    flyout->anim = 0;
}

void axau_flyout_paint(gui_surface_t *surface, axau_flyout_t *flyout,
                       const axau_palette_t *palette)
{
    if (!surface || !flyout || !flyout->open) return;
    gui_rect_t rect = flyout->rect;
    if (flyout->anim < 1000) {
        /* Scale from the anchor: 96 percent at the start, full size at the end,
         * anchored on the corner nearest the opening control. */
        i32 inset = (1000 - flyout->anim) * 4 / 1000;
        rect = gui_rect(rect.x + inset, rect.y + inset,
                        rect.w - inset * 2, rect.h - inset * 2);
    }
    axau_paint_island(surface, rect, AXAU_RADIUS_FLYOUT, palette);
    /* Tail: a small triangle pointing at the anchor. */
    i32 tail_x = flyout->anchor_x;
    if (flyout->above) {
        i32 base_y = rect.y + rect.h;
        axui_fill_triangle(surface,
                           (axui_point_t){tail_x - 6, base_y},
                           (axui_point_t){tail_x + 6, base_y},
                           (axui_point_t){tail_x, base_y + 6},
                           axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    }
}

/* ------------------------------------------------------------------ */
/* Tooltip                                                             */
/* ------------------------------------------------------------------ */

void axau_tooltip_tick(axau_tooltip_t *tooltip, i32 dt_ms)
{
    if (!tooltip) return;
    if (!tooltip->visible) {
        tooltip->elapsed_ms += (u32)(dt_ms > 0 ? dt_ms : 0);
        if (tooltip->elapsed_ms >= tooltip->delay_ms) {
            tooltip->visible = (bool_t)1;
        }
        return;
    }
    if (tooltip->anim < 1000) {
        tooltip->anim += dt_ms * 5;
        if (tooltip->anim > 1000) tooltip->anim = 1000;
    }
}

void axau_tooltip_paint(gui_surface_t *surface, axau_tooltip_t *tooltip,
                        const axau_palette_t *palette)
{
    if (!surface || !tooltip || !tooltip->visible || !tooltip->text[0]) return;
    i32 width = axui_measure_text(tooltip->text, 1) + 16;
    i32 height = 26;
    i32 x = tooltip->anchor.x + tooltip->anchor.w / 2 - width / 2;
    i32 y = tooltip->anchor.y - height - 8;
    if (y < 4) {
        y = tooltip->anchor.y + tooltip->anchor.h + 8;
    }
    if (x < 4) x = 4;
    if (x + width > surface->width - 4) x = surface->width - width - 4;
    gui_rect_t rect = gui_rect(x, y, width, height);
    gui_fill_round_rect(surface, rect, 4,
                        axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
    axui_round_rect_border(surface, rect, 4, 1,
                           axau_colour(palette, AXAU_COLOUR_STROKE));
    axui_draw_text_center(surface, rect, tooltip->text,
                          axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */
/* ------------------------------------------------------------------ */

void axau_menu_init(axau_menu_t *menu, i32 x, i32 y)
{
    if (!menu) return;
    ax_memset(menu, 0, sizeof(*menu));
    menu->rect = gui_rect(x, y, MENU_WIDTH, 8);
    menu->hover = -1;
    menu->selected = -1;
}

void axau_menu_add(axau_menu_t *menu, const char *label, const char *shortcut,
                   const char *glyph)
{
    if (!menu || menu->count >= 8) return;
    axau_menu_item_t *item = &menu->items[menu->count];
    ax_memset(item, 0, sizeof(*item));
    ax_strlcpy(item->label, label ? label : "", sizeof(item->label));
    ax_strlcpy(item->shortcut, shortcut ? shortcut : "", sizeof(item->shortcut));
    ax_strlcpy(item->glyph, glyph ? glyph : "", sizeof(item->glyph));
    item->enabled = (bool_t)1;
    menu->count++;
    menu->rect.h += MENU_ITEM_HEIGHT;
}

void axau_menu_add_separator(axau_menu_t *menu)
{
    if (!menu || menu->count >= 8) return;
    axau_menu_item_t *item = &menu->items[menu->count];
    ax_memset(item, 0, sizeof(*item));
    item->separator = (bool_t)1;
    menu->count++;
    menu->rect.h += 9;
}

void axau_menu_paint(gui_surface_t *surface, axau_menu_t *menu,
                     const axau_palette_t *palette)
{
    if (!surface || !menu || !menu->open) return;
    axau_paint_island(surface, menu->rect, AXAU_RADIUS_CARD, palette);
    i32 y = menu->rect.y + 4;
    for (i32 i = 0; i < menu->count; i++) {
        axau_menu_item_t *item = &menu->items[i];
        if (item->separator) {
            axau_paint_divider(surface, gui_rect(menu->rect.x + 12, y + 4,
                                                 menu->rect.w - 24, 1), palette);
            y += 9;
            continue;
        }
        gui_rect_t row = gui_rect(menu->rect.x + 4, y, menu->rect.w - 8,
                                  MENU_ITEM_HEIGHT);
        if (menu->hover == i && item->enabled) {
            gui_fill_round_rect(surface, row, 4,
                                axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
        }
        u32 text = item->enabled ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                 : axau_colour(palette, AXAU_COLOUR_TEXT_DISABLED);
        if (item->glyph[0]) {
            axau_icon_glyph(surface, row.x + 16, row.y + row.h / 2,
                            AXAU_ICON_SIZE, item->glyph, text);
        }
        axui_draw_text(surface, row.x + 32, axau_text_center_y(row, 1),
                       item->label, axui_style(text, 1));
        if (item->shortcut[0]) {
            axui_draw_text_right(surface,
                                 gui_rect(row.x, row.y, row.w - 12, row.h),
                                 item->shortcut,
                                 axui_style(axau_colour(palette,
                                                        AXAU_COLOUR_TEXT_TERTIARY), 1));
        }
        if (item->submenu) {
            i32 cx = row.x + row.w - 10;
            i32 cy = row.y + row.h / 2;
            axui_line_aa(surface, cx - 3, cy - 4, cx + 1, cy,
                         axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1);
            axui_line_aa(surface, cx + 1, cy, cx - 3, cy + 4,
                         axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1);
        }
        y += MENU_ITEM_HEIGHT;
    }
}

i32 axau_menu_hit(axau_menu_t *menu, i32 x, i32 y)
{
    if (!menu || !menu->open) return -1;
    if (!gui_rect_contains(menu->rect, x, y)) {
        menu->hover = -1;
        return -1;
    }
    i32 cursor_y = menu->rect.y + 4;
    for (i32 i = 0; i < menu->count; i++) {
        axau_menu_item_t *item = &menu->items[i];
        i32 height = item->separator ? 9 : MENU_ITEM_HEIGHT;
        if (y >= cursor_y && y < cursor_y + height) {
            menu->hover = i;
            if (!item->separator && item->enabled) {
                menu->selected = i;
                menu->open = (bool_t)0;
                return i;
            }
            return -1;
        }
        cursor_y += height;
    }
    return -1;
}
