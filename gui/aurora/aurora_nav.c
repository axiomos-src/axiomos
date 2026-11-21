/*
 * AxiomOS - Aurora navigation containers.
 *
 * The shell uses one navigation pattern everywhere: a left pane with a
 * selection indicator, a header area, and content.  Rather than reimplement
 * that per application, the pane, tab strip, command bar and breadcrumb live
 * here and every app composes them.
 *
 * Selection animation is stored per item rather than per container because a
 * pane can have two items mid-transition when the user moves the pointer fast;
 * a single container-level progress would make the old indicator jump.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

/* ------------------------------------------------------------------ */
/* Navigation pane                                                     */
/* ------------------------------------------------------------------ */

void axau_navigation_init(axau_navigation_t *nav, gui_rect_t rect, i32 pane_width)
{
    if (!nav) return;
    ax_memset(nav, 0, sizeof(*nav));
    nav->rect = rect;
    nav->pane_width = pane_width > 0 ? pane_width : 240;
    nav->selected = -1;
    nav->hover = -1;
    nav->open = (bool_t)1;
    nav->anim = 1000;
}

void axau_navigation_add(axau_navigation_t *nav, const char *label,
                         const char *glyph)
{
    if (!nav || nav->count >= AXAU_MAX_NAV_ITEMS) return;
    axau_nav_item_t *item = &nav->items[nav->count];
    ax_memset(item, 0, sizeof(*item));
    ax_strlcpy(item->label, label ? label : "", sizeof(item->label));
    ax_strlcpy(item->glyph, glyph ? glyph : "", sizeof(item->glyph));
    item->selection_anim = 0;
    nav->count++;
}

static i32 nav_pane_width(const axau_navigation_t *nav,
                          const axau_metrics_t *metrics)
{
    i32 width = nav->pane_width;
    if (nav->compact) {
        width = 56;
    }
    if (metrics) {
        width = axau_dp(metrics, width);
    }
    return width;
}

void axau_navigation_paint(gui_surface_t *surface, axau_navigation_t *nav,
                           const axau_palette_t *palette,
                           const axau_metrics_t *metrics)
{
    if (!surface || !nav) return;
    i32 pane_width = nav_pane_width(nav, metrics);
    i32 row_height = metrics ? metrics->row_height : AXAU_ROW_HEIGHT;

    /* Pane background: one step off the content surface so the two read as
     * separate layers even when both are Mica. */
    gui_rect_t pane = gui_rect(nav->rect.x, nav->rect.y, pane_width,
                               nav->rect.h);
    gui_fill_rect(surface, pane,
                  axui_colour_alpha(axau_colour(palette, AXAU_COLOUR_CONTROL), 20));
    if (!nav->compact) {
        gui_rect_t header = gui_rect(pane.x + 16, pane.y + 12, pane.w - 32, 32);
        if (nav->header[0]) {
            axui_draw_text(surface, header.x, axau_text_center_y(header, 1),
                           nav->header,
                           axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        }
    }

    i32 start_y = pane.y + (nav->compact ? 8 : 56);
    for (i32 i = 0; i < nav->count; i++) {
        axau_nav_item_t *item = &nav->items[i];
        gui_rect_t row = gui_rect(pane.x + 4, start_y + i * row_height,
                                  pane.w - 8, row_height - 2);
        /* Selection pill: 3 px rounded indicator plus a tinted background. */
        if (item->selection_anim > 0) {
            u32 tint = axui_colour_opacity(axau_colour(palette, AXAU_COLOUR_ACCENT),
                                           12 * item->selection_anim / 1000);
            gui_fill_round_rect(surface, row, 4, tint);
            i32 bar_height = row.h / 2 - 4;
            i32 bar_y = row.y + (row.h - bar_height) / 2;
            gui_fill_round_rect(surface, gui_rect(row.x + 1, bar_y, 3, bar_height),
                                2, axau_colour(palette, AXAU_COLOUR_ACCENT));
        } else if (item->hover) {
            gui_fill_round_rect(surface, row, 4,
                                axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
        }
        u32 colour = item->selected
                         ? axau_colour(palette, AXAU_COLOUR_TEXT)
                         : axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY);
        i32 icon_x = nav->compact ? row.x + row.w / 2 : row.x + 12 + 8;
        i32 centre_y = row.y + row.h / 2;
        if (item->glyph[0]) {
            axau_icon_glyph(surface, icon_x, centre_y, AXAU_ICON_SIZE,
                            item->glyph, colour);
        }
        if (!nav->compact) {
            axui_draw_text_clip(surface, row, row.x + 44,
                                axau_text_center_y(row, 1), item->label,
                                axui_style(colour, 1));
            if (item->badge > 0) {
                char badge[8];
                ax_snprintf(badge, sizeof(badge), "%d", item->badge);
                i32 badge_w = axui_measure_text(badge, 1) + 10;
                gui_rect_t badge_rect = gui_rect(row.x + row.w - badge_w - 6,
                                                 row.y + (row.h - 18) / 2,
                                                 badge_w, 18);
                gui_fill_round_rect(surface, badge_rect, 9,
                                    axau_colour(palette, AXAU_COLOUR_ACCENT));
                axui_draw_text_center(surface, badge_rect, badge,
                                      axui_style(axau_colour(palette,
                                                             AXAU_COLOUR_ACCENT_TEXT), 1));
            }
        }
    }
}

i32 axau_navigation_hit(axau_navigation_t *nav, i32 x, i32 y)
{
    if (!nav) return -1;
    i32 pane_width = nav_pane_width(nav, &axau_context()->metrics);
    if (x < nav->rect.x || x >= nav->rect.x + pane_width) {
        return -1;
    }
    i32 start_y = nav->rect.y + (nav->compact ? 8 : 56);
    i32 row_height = axau_context()->metrics.row_height;
    i32 index = (y - start_y) / (row_height ? row_height : 1);
    nav->hover = index;
    if (index < 0 || index >= nav->count) {
        return -1;
    }
    if (y < start_y) {
        return -1;
    }
    if ((y - start_y) % row_height > row_height - 2) {
        return -1;
    }
    nav->selected = index;
    for (i32 i = 0; i < nav->count; i++) {
        nav->items[i].selected = (bool_t)(i == index);
    }
    if (nav->items[index].selection_anim < 1000) {
        nav->items[index].selection_anim = 1000;
    }
    return index;
}

/* ------------------------------------------------------------------ */
/* Tab strip                                                           */
/* ------------------------------------------------------------------ */

void axau_tabbar_init(axau_tabbar_t *bar, gui_rect_t rect)
{
    if (!bar) return;
    ax_memset(bar, 0, sizeof(*bar));
    bar->rect = rect;
    bar->selected = -1;
    bar->hover = -1;
}

void axau_tabbar_add(axau_tabbar_t *bar, const char *title, bool_t closeable)
{
    if (!bar || bar->count >= AXAU_MAX_TABS) return;
    axau_tab_t *tab = &bar->tabs[bar->count];
    ax_memset(tab, 0, sizeof(*tab));
    ax_strlcpy(tab->title, title ? title : "", sizeof(tab->title));
    tab->closeable = closeable;
    tab->width = axui_measure_text(tab->title, 1) + 36;
    if (bar->selected < 0) {
        bar->selected = 0;
        tab->selected = (bool_t)1;
    }
    bar->count++;
}

void axau_tabbar_paint(gui_surface_t *surface, axau_tabbar_t *bar,
                       const axau_palette_t *palette,
                       const axau_metrics_t *metrics)
{
    if (!surface || !bar) return;
    (void)metrics;
    /* The strip itself is transparent: tabs sit directly on the window body so
     * the material runs edge to edge without a seam. */
    i32 x = bar->rect.x - bar->scroll;
    i32 selected_x = bar->rect.x;
    i32 selected_width = 0;
    for (i32 i = 0; i < bar->count; i++) {
        axau_tab_t *tab = &bar->tabs[i];
        i32 width = tab->width;
        gui_rect_t tab_rect = gui_rect(x, bar->rect.y, width, bar->rect.h);
        if (tab_rect.x + tab_rect.w < bar->rect.x) {
            x += width;
            continue;
        }
        if (tab_rect.x > bar->rect.x + bar->rect.w) {
            break;
        }
        u32 text = tab->selected ? axau_colour(palette, AXAU_COLOUR_TEXT)
                                 : axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY);
        u32 fill = tab->selected
                       ? axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED)
                       : (tab->hover ? axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER) : 0);
        if (fill) {
            gui_fill_rect(surface, gui_rect(tab_rect.x + 2, tab_rect.y + 4,
                                            tab_rect.w - 4, tab_rect.h - 4), fill);
        }
        axui_draw_text_clip(surface, tab_rect, tab_rect.x + 14,
                            axau_text_center_y(tab_rect, 1), tab->title,
                            axui_style(text, 1));
        if (tab->closeable) {
            i32 cx = tab_rect.x + width - 16;
            i32 cy = tab_rect.y + tab_rect.h / 2;
            axui_line_aa(surface, cx - 4, cy - 4, cx + 4, cy + 4,
                         axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1);
            axui_line_aa(surface, cx - 4, cy + 4, cx + 4, cy - 4,
                         axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1);
        }
        if (tab->selected) {
            selected_x = tab_rect.x;
            selected_width = width;
        }
        x += width;
    }
    if (selected_width > 0) {
        /* Two-pixel underbar rather than a full outline: it reads as an
         * indicator without competing with the tab content. */
        gui_fill_round_rect(surface,
                            gui_rect(selected_x + 12, bar->rect.y + bar->rect.h - 3,
                                     selected_width - 24, 2), 1,
                            axau_colour(palette, AXAU_COLOUR_ACCENT));
    }
    if (bar->overflow) {
        gui_rect_t more = gui_rect(bar->rect.x + bar->rect.w - 24, bar->rect.y,
                                   24, bar->rect.h);
        axui_draw_text_center(surface, more, ">",
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_SECONDARY), 1));
    }
}

i32 axau_tabbar_hit(axau_tabbar_t *bar, i32 x, i32 y, bool_t *close_hit)
{
    if (close_hit) *close_hit = (bool_t)0;
    if (!bar) return -1;
    if (!gui_rect_contains(bar->rect, x, y)) {
        bar->hover = -1;
        return -1;
    }
    i32 cursor_x = bar->rect.x - bar->scroll;
    for (i32 i = 0; i < bar->count; i++) {
        i32 width = bar->tabs[i].width;
        if (x >= cursor_x && x < cursor_x + width) {
            bar->hover = i;
            if (bar->tabs[i].closeable && x >= cursor_x + width - 24) {
                if (close_hit) *close_hit = (bool_t)1;
                return i;
            }
            bar->selected = i;
            for (i32 j = 0; j < bar->count; j++) {
                bar->tabs[j].selected = (bool_t)(j == i);
            }
            return i;
        }
        cursor_x += width;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Command bar                                                         */
/* ------------------------------------------------------------------ */

void axau_command_bar_init(axau_command_bar_t *bar, gui_rect_t rect)
{
    if (!bar) return;
    ax_memset(bar, 0, sizeof(*bar));
    bar->rect = rect;
}

void axau_command_bar_add(axau_command_bar_t *bar, const char *label,
                          const char *glyph, bool_t accent)
{
    if (!bar || bar->count >= 8) return;
    axau_button_t *button = &bar->commands[bar->count];
    ax_memset(button, 0, sizeof(*button));
    ax_strlcpy(button->label, label ? label : "", sizeof(button->label));
    ax_strlcpy(button->glyph, glyph ? glyph : "", sizeof(button->glyph));
    button->accent = accent;
    button->has_icon = (bool_t)(button->glyph[0] != 0);
    button->enabled = (bool_t)1;
    button->rect = gui_rect(bar->rect.x, bar->rect.y, 0, bar->rect.h);
    bar->count++;
}

void axau_command_bar_paint(gui_surface_t *surface, axau_command_bar_t *bar,
                            const axau_palette_t *palette,
                            const axau_metrics_t *metrics)
{
    if (!surface || !bar) return;
    i32 x = bar->rect.x;
    for (i32 i = 0; i < bar->count; i++) {
        axau_button_t *button = &bar->commands[i];
        i32 label_width = button->label[0] ? axui_measure_text(button->label, 1) : 0;
        i32 width = label_width + (button->has_icon ? 28 : 0) + 24;
        button->rect = gui_rect(x, bar->rect.y, width, bar->rect.h);
        axau_button_paint(surface, button, palette, metrics);
        x += width + 4;
    }
    if (x > bar->rect.x + bar->rect.w) {
        bar->overflow_open = (bool_t)1;
        gui_rect_t more = gui_rect(bar->rect.x + bar->rect.w - 32,
                                   bar->rect.y, 32, bar->rect.h);
        gui_fill_round_rect(surface, more, 4,
                            axau_colour(palette, AXAU_COLOUR_CONTROL));
        axui_draw_text_center(surface, more, "...",
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_SECONDARY), 1));
    }
}

i32 axau_command_bar_hit(axau_command_bar_t *bar, i32 x, i32 y)
{
    if (!bar) return -1;
    for (i32 i = 0; i < bar->count; i++) {
        if (axau_button_hit(&bar->commands[i], x, y)) {
            return i;
        }
    }
    for (i32 i = 0; i < bar->count; i++) {
        bar->commands[i].hover = (bool_t)0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Breadcrumb                                                          */
/* ------------------------------------------------------------------ */

void axau_breadcrumb_set(axau_breadcrumb_t *crumb, gui_rect_t rect,
                         const char *path)
{
    if (!crumb) return;
    ax_memset(crumb, 0, sizeof(*crumb));
    crumb->rect = rect;
    crumb->hover = -1;
    if (!path || !path[0]) {
        return;
    }
    /* Split on '/' or '\'.  A path longer than four components keeps the last
     * four, because that is what fits and what users need to disambiguate. */
    char scratch[160];
    ax_strlcpy(scratch, path, sizeof(scratch));
    char parts[8][32];
    i32 count = 0;
    usize length = ax_strlen(scratch);
    usize start = 0;
    for (usize i = 0; i <= length; i++) {
        char c = scratch[i];
        if (c == '/' || c == '\\' || c == 0) {
            if (i > start && count < 8) {
                usize size = i - start;
                if (size >= 32) size = 31;
                for (usize j = 0; j < size; j++) {
                    parts[count][j] = scratch[start + j];
                }
                parts[count][size] = 0;
                count++;
            }
            start = i + 1;
        }
    }
    i32 first = count > 4 ? count - 4 : 0;
    crumb->count = count - first;
    for (i32 i = 0; i < crumb->count; i++) {
        ax_strlcpy(crumb->parts[i], parts[first + i], sizeof(crumb->parts[i]));
    }
}

void axau_breadcrumb_paint(gui_surface_t *surface, axau_breadcrumb_t *crumb,
                           const axau_palette_t *palette)
{
    if (!surface || !crumb) return;
    i32 x = crumb->rect.x;
    for (i32 i = 0; i < crumb->count; i++) {
        const char *part = crumb->parts[i];
        i32 width = axui_measure_text(part, 1);
        gui_rect_t item = gui_rect(x, crumb->rect.y, width + 12, crumb->rect.h);
        if (crumb->hover == i) {
            gui_fill_round_rect(surface, item, 4,
                                axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER));
        }
        axui_draw_text(surface, x + 6, axau_text_center_y(crumb->rect, 1), part,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 1));
        x += width + 12;
        if (i + 1 < crumb->count) {
            i32 cy = crumb->rect.y + crumb->rect.h / 2;
            axui_line_aa(surface, x, cy - 4, x + 4, cy,
                         axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1);
            axui_line_aa(surface, x + 4, cy, x, cy + 4,
                         axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1);
            x += 10;
        }
    }
}

i32 axau_breadcrumb_hit(axau_breadcrumb_t *crumb, i32 x, i32 y)
{
    if (!crumb) return -1;
    if (!gui_rect_contains(crumb->rect, x, y)) {
        crumb->hover = -1;
        return -1;
    }
    i32 cursor_x = crumb->rect.x;
    for (i32 i = 0; i < crumb->count; i++) {
        i32 width = axui_measure_text(crumb->parts[i], 1) + 12;
        if (x >= cursor_x && x < cursor_x + width) {
            crumb->hover = i;
            return i;
        }
        cursor_x += width + 10;
    }
    crumb->hover = -1;
    return -1;
}
