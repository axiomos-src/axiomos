/*
 * AxiomOS - Aurora shell implementation.
 *
 * Geometry is computed in one place per surface (`*_layout`) and stored in the
 * shell struct, so the paint and hit-test paths read the same rectangles.  The
 * launcher and the action panel both slide from the bar: their opening animation
 * is a vertical offset plus opacity, not a scale, because scaling text at these
 * sizes produces visible shimmer.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"
#include "aurora_shell.h"

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static void shell_layout_bar(axau_shell_t *shell)
{
    i32 bar_h = AXAU_BAR_HEIGHT;
    shell->bar = gui_rect(0, shell->height - bar_h, shell->width, bar_h);
    i32 icon = AXAU_BAR_ICON;
    i32 gap = 6;
    /* Five slots centred: launcher, search, task view, then the action cluster
     * on the right.  The clock sits inside the action cluster. */
    i32 centre = shell->width / 2;
    shell->launcher_button = gui_rect(centre - icon - gap - icon / 2,
                                      shell->height - bar_h + (bar_h - icon) / 2,
                                      icon, icon);
    shell->search_button = gui_rect(centre - icon / 2,
                                    shell->height - bar_h + (bar_h - icon) / 2,
                                    icon, icon);
    shell->action_button = gui_rect(shell->width - 180,
                                    shell->height - bar_h + (bar_h - icon) / 2,
                                    icon + 96, icon);
    shell->clock_rect = gui_rect(shell->width - 76,
                                 shell->height - bar_h + 8, 64, bar_h - 16);
}

static void shell_layout_launcher(axau_shell_t *shell)
{
    i32 width = AXAU_LAUNCHER_WIDTH;
    i32 height = AXAU_LAUNCHER_HEIGHT;
    if (width > shell->width - 32) width = shell->width - 32;
    if (height > shell->height - AXAU_BAR_HEIGHT - 32) {
        height = shell->height - AXAU_BAR_HEIGHT - 32;
    }
    i32 x = (shell->width - width) / 2;
    i32 y = shell->height - AXAU_BAR_HEIGHT - height - 8;
    shell->launcher_rect = gui_rect(x, y, width, height);
}

static void shell_layout_actions(axau_shell_t *shell)
{
    i32 width = AXAU_ACTION_WIDTH;
    i32 height = AXAU_ACTION_HEIGHT;
    if (height > shell->height - AXAU_BAR_HEIGHT - 24) {
        height = shell->height - AXAU_BAR_HEIGHT - 24;
    }
    i32 x = shell->width - width - 12;
    i32 y = shell->height - AXAU_BAR_HEIGHT - height - 8;
    shell->action_rect = gui_rect(x, y, width, height);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void axau_shell_init(axau_shell_t *shell, i32 width, i32 height)
{
    if (!shell) return;
    ax_memset(shell, 0, sizeof(*shell));
    shell->width = width > 0 ? width : 1024;
    shell->height = height > 0 ? height : 768;
    shell->bar_visible = (bool_t)1;
    shell->bar_reveal = 1000;
    shell->action_close_timer = 0;
    shell->volume = 60;
    shell->brightness = 70;
    shell->battery = 82;
    shell->charging = (bool_t)0;
    ax_strlcpy(shell->user_name, "Axiom User", sizeof(shell->user_name));
    shell_layout_bar(shell);
    shell_layout_launcher(shell);
    shell_layout_actions(shell);
    shell->initialised = (bool_t)1;
}

void axau_shell_tick(axau_shell_t *shell, i32 dt_ms)
{
    if (!shell) return;
    if (dt_ms < 0) dt_ms = 0;
    shell->frame++;
    if (shell->launcher_open && shell->launcher_anim < 1000) {
        shell->launcher_anim += dt_ms * 6;
        if (shell->launcher_anim > 1000) shell->launcher_anim = 1000;
    } else if (!shell->launcher_open && shell->launcher_anim > 0) {
        shell->launcher_anim -= dt_ms * 8;
        if (shell->launcher_anim < 0) shell->launcher_anim = 0;
    }
    if (shell->action_open && shell->action_anim < 1000) {
        shell->action_anim += dt_ms * 6;
        if (shell->action_anim > 1000) shell->action_anim = 1000;
    } else if (!shell->action_open && shell->action_anim > 0) {
        shell->action_anim -= dt_ms * 8;
        if (shell->action_anim < 0) shell->action_anim = 0;
    }
    for (i32 i = 0; i < shell->tile_count; i++) {
        axau_action_tile_t *tile = &shell->tiles[i];
        i32 target = tile->on ? 1000 : 0;
        if (tile->anim < target) {
            tile->anim += dt_ms * 8;
            if (tile->anim > target) tile->anim = target;
        } else if (tile->anim > target) {
            tile->anim -= dt_ms * 8;
            if (tile->anim < target) tile->anim = target;
        }
    }
    for (i32 i = 0; i < shell->notification_count; i++) {
        if (shell->notifications[i].anim < 1000) {
            shell->notifications[i].anim += dt_ms * 5;
            if (shell->notifications[i].anim > 1000) {
                shell->notifications[i].anim = 1000;
            }
        }
    }
    if (!shell->bar_visible && shell->bar_reveal > 0) {
        shell->bar_reveal -= dt_ms * 4;
        if (shell->bar_reveal < 0) shell->bar_reveal = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Bar                                                                 */
/* ------------------------------------------------------------------ */

static void paint_bar_icon(gui_surface_t *surface, gui_rect_t rect,
                           const char *glyph, bool_t active,
                           const axau_palette_t *palette)
{
    u32 colour = active ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                        : axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY);
    if (active) {
        gui_fill_round_rect(surface, rect, rect.h / 2,
                            axui_colour_opacity(
                                axau_colour(palette, AXAU_COLOUR_ACCENT), 16));
    }
    axau_icon_glyph(surface, rect.x + rect.w / 2, rect.y + rect.h / 2,
                    AXAU_ICON_SIZE, glyph, colour);
}

void axau_shell_paint(gui_surface_t *surface, axau_shell_t *shell)
{
    if (!surface || !shell) return;
    const axau_context_t *context = axau_context();
    const axau_palette_t *palette = &context->palette;
    shell_layout_bar(shell);

    /* Bar material: a single acrylic strip, no per-icon backgrounds. */
    i32 offset = 0;
    if (shell->bar_auto_hide && !shell->bar_visible) {
        offset = (AXAU_BAR_HEIGHT - 2) * (1000 - shell->bar_reveal) / 1000;
    }
    gui_rect_t bar = gui_rect(shell->bar.x, shell->bar.y + offset, shell->bar.w,
                              shell->bar.h);
    axau_material_t material;
    axau_material_acrylic(&material, palette, 0);
    material.opacity = 88;
    material.hairline = (bool_t)1;
    axau_paint_material(surface, bar, &material, palette);
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y,
              axau_colour(palette, AXAU_COLOUR_STROKE));

    if (offset < bar.h - 2) {
        paint_bar_icon(surface,
                       gui_rect(shell->launcher_button.x, shell->bar.y + offset - 4,
                                shell->launcher_button.w, shell->launcher_button.h),
                       "*", shell->launcher_open, palette);
        paint_bar_icon(surface,
                       gui_rect(shell->search_button.x, shell->bar.y + offset - 4,
                                shell->search_button.w, shell->search_button.h),
                       "?", shell->search_active, palette);
        paint_bar_icon(surface,
                       gui_rect(shell->action_button.x, shell->bar.y + offset - 4,
                                shell->action_button.w, shell->action_button.h),
                       "=", shell->action_open, palette);

        char clock[8];
        ax_snprintf(clock, sizeof(clock), "%02u:%02u", shell->hour, shell->minute);
        axui_draw_text_right(surface,
                             gui_rect(shell->clock_rect.x, shell->bar.y + offset,
                                      shell->clock_rect.w, shell->clock_rect.h),
                             clock,
                             axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
    }
}

/* ------------------------------------------------------------------ */
/* Launcher                                                            */
/* ------------------------------------------------------------------ */

static void paint_launcher_tile(gui_surface_t *surface, gui_rect_t rect,
                                const axau_shell_app_t *app, bool_t hover,
                                bool_t pinned, const axau_palette_t *palette)
{
    u32 fill = hover ? axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER)
                     : axau_colour(palette, AXAU_COLOUR_CONTROL);
    gui_fill_round_rect(surface, rect, AXAU_RADIUS_CARD, fill);
    /* App badge: a rounded square in the app's accent, glyph centred. */
    i32 badge = rect.w > 96 ? 32 : 24;
    gui_rect_t icon = gui_rect(rect.x + (rect.w - badge) / 2, rect.y + 10,
                               badge, badge);
    gui_fill_round_rect(surface, icon, badge / 2,
                        axui_colour_opacity(app->accent, 28));
    axau_icon_glyph(surface, icon.x + badge / 2, icon.y + badge / 2, 16,
                    app->glyph, app->accent);
    axui_draw_text_center(surface,
                          gui_rect(rect.x, rect.y + badge + 16, rect.w, 16),
                          app->name,
                          axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
    if (pinned) {
        i32 cx = rect.x + rect.w - 10;
        i32 cy = rect.y + 10;
        axui_circle(surface, cx, cy, 3,
                    axau_colour(palette, AXAU_COLOUR_ACCENT));
    }
}

void axau_shell_paint_overlays(gui_surface_t *surface, axau_shell_t *shell)
{
    if (!surface || !shell) return;
    const axau_palette_t *palette = &axau_context()->palette;
    shell_layout_launcher(shell);
    shell_layout_actions(shell);

    if (shell->launcher_anim > 0) {
        i32 progress = shell->launcher_anim;
        gui_rect_t rect = shell->launcher_rect;
        gui_blend_rect(surface, gui_rect(0, 0, surface->width, surface->height),
                       GUI_ARGB((u8)((0x40 * progress) / 1000), 0, 0, 0));
        i32 slide = (1000 - progress) * 24 / 1000;
        rect.y += slide;
        axau_paint_island(surface, rect, AXAU_RADIUS_FLYOUT, palette);

        i32 padding = 24;
        gui_rect_t search = gui_rect(rect.x + padding, rect.y + padding - 8,
                                     rect.w - padding * 2, 36);
        axau_textbox_t box;
        ax_memset(&box, 0, sizeof(box));
        box.rect = search;
        ax_strlcpy(box.text, shell->search, sizeof(box.text));
        ax_strlcpy(box.placeholder, "Search apps and documents",
                   sizeof(box.placeholder));
        box.hover = (bool_t)1;
        axau_textbox_paint(surface, &box, palette, &axau_context()->metrics);

        i32 grid_top = rect.y + padding + 44;
        i32 columns = 6;
        i32 tile_w = (rect.w - padding * 2 - (columns - 1) * 8) / columns;
        i32 tile_h = 88;
        for (i32 i = 0; i < shell->app_count && i < 18; i++) {
            i32 column = i % columns;
            i32 row = i / columns;
            gui_rect_t tile = gui_rect(rect.x + padding + column * (tile_w + 8),
                                       grid_top + row * (tile_h + 8),
                                       tile_w, tile_h);
            if (tile.y + tile.h > rect.y + rect.h - 56) {
                break;
            }
            paint_launcher_tile(surface, tile, &shell->apps[i],
                                (bool_t)(shell->hover_app == i),
                                shell->apps[i].pinned, palette);
        }

        /* Footer: user chip on the left, power on the right. */
        gui_rect_t footer = gui_rect(rect.x + padding, rect.y + rect.h - 48,
                                     rect.w - padding * 2, 32);
        gui_fill_round_rect(surface, footer, 16,
                            axau_colour(palette, AXAU_COLOUR_CONTROL));
        axui_circle(surface, footer.x + 18, footer.y + 16, 10,
                    axau_colour(palette, AXAU_COLOUR_ACCENT));
        axui_draw_text(surface, footer.x + 36, footer.y + 8, shell->user_name,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        axui_draw_text_right(surface,
                             gui_rect(footer.x, footer.y, footer.w - 12, footer.h),
                             "Power",
                             axui_style(axau_colour(palette,
                                                    AXAU_COLOUR_TEXT_SECONDARY), 1));
    }

    if (shell->action_anim > 0) {
        i32 progress = shell->action_anim;
        gui_rect_t rect = shell->action_rect;
        rect.y += (1000 - progress) * 16 / 1000;
        axau_paint_island(surface, rect, AXAU_RADIUS_FLYOUT, palette);

        i32 padding = 16;
        i32 columns = 3;
        i32 tile_w = (rect.w - padding * 2 - (columns - 1) * 8) / columns;
        i32 tile_h = 72;
        for (i32 i = 0; i < shell->tile_count; i++) {
            i32 column = i % columns;
            i32 row = i / columns;
            axau_action_tile_t *tile = &shell->tiles[i];
            tile->rect = gui_rect(rect.x + padding + column * (tile_w + 8),
                                  rect.y + padding + row * (tile_h + 8),
                                  tile_w, tile_h);
            u32 fill = tile->on
                           ? axui_colour_opacity(
                                 axau_colour(palette, AXAU_COLOUR_ACCENT),
                                 14 + tile->anim * 12 / 1000)
                           : axau_colour(palette, AXAU_COLOUR_CONTROL);
            if (tile->hover) {
                fill = axau_colour(palette, AXAU_COLOUR_CONTROL_HOVER);
            }
            gui_fill_round_rect(surface, tile->rect, AXAU_RADIUS_CARD, fill);
            axau_icon_glyph(surface, tile->rect.x + 20,
                            tile->rect.y + tile->rect.h / 2, 16, tile->glyph,
                            tile->on ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                                     : axau_colour(palette, AXAU_COLOUR_TEXT));
            axui_draw_text(surface, tile->rect.x + 36,
                           tile->rect.y + 14, tile->label,
                           axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
            if (tile->on) {
                axui_draw_text(surface, tile->rect.x + 36,
                               tile->rect.y + 34, "on",
                               axui_style(axau_colour(palette,
                                                      AXAU_COLOUR_TEXT_TERTIARY), 1));
            }
        }

        /* Sliders sit below the tiles: volume and brightness. */
        i32 slider_top = rect.y + padding + ((shell->tile_count + columns - 1) /
                                             columns) * (tile_h + 8) + 8;
        axau_slider_t slider;
        ax_memset(&slider, 0, sizeof(slider));
        slider.rect = gui_rect(rect.x + padding, slider_top, rect.w - padding * 2, 32);
        slider.value = shell->volume;
        slider.minimum = 0;
        slider.maximum = 100;
        slider.enabled = (bool_t)1;
        slider.show_value = (bool_t)1;
        axau_slider_paint(surface, &slider, palette, &axau_context()->metrics);

        ax_memset(&slider, 0, sizeof(slider));
        slider.rect = gui_rect(rect.x + padding, slider_top + 40, rect.w - padding * 2, 32);
        slider.value = shell->brightness;
        slider.minimum = 0;
        slider.maximum = 100;
        slider.enabled = (bool_t)1;
        axau_slider_paint(surface, &slider, palette, &axau_context()->metrics);

        axui_draw_text(surface, rect.x + padding, rect.y + rect.h - 28,
                       shell->vpn ? "VPN connected" : "Local network",
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 1));
    }

    /* Notifications stack above the bar on the right. */
    i32 notify_y = shell->height - AXAU_BAR_HEIGHT - 12;
    for (i32 i = 0; i < shell->notification_count; i++) {
        axau_shell_notification_t *note = &shell->notifications[i];
        i32 width = 340;
        i32 height = 76;
        i32 x = shell->width - width - 12;
        notify_y -= height + 8;
        gui_rect_t rect = gui_rect(x + (1000 - note->anim) * 24 / 1000, notify_y,
                                   width, height);
        axau_paint_island(surface, rect, AXAU_RADIUS_CARD, palette);
        u32 accent = note->level == 2 ? axau_colour(palette, AXAU_COLOUR_ERROR)
                                      : axau_colour(palette, AXAU_COLOUR_ACCENT);
        gui_fill_rect(surface, gui_rect(rect.x, rect.y + 8, 3, rect.h - 16), accent);
        axui_draw_text(surface, rect.x + 16, rect.y + 12, note->source,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1));
        axui_draw_text(surface, rect.x + 16, rect.y + 30, note->title,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        axui_draw_text_clip(surface,
                            gui_rect(rect.x + 16, rect.y + 46, rect.w - 32, 20),
                            rect.x + 16, rect.y + 46, note->body,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_SECONDARY), 1));
        if (shell->hover_notification == i) {
            axui_round_rect_border(surface, rect, AXAU_RADIUS_CARD, 1,
                                   axau_colour(palette, AXAU_COLOUR_STROKE_STRONG));
        }
    }
}
