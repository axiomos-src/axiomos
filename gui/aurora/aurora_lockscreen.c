/*
 * AxiomOS - lock screen, notification centre and quick settings.
 *
 * Three surfaces that all sit on top of everything and all share one property:
 * they are about the *session*, not about any application.  Keeping them in one
 * file makes that property explicit and lets them share the same overlay
 * animation timing.
 *
 * The lock screen is deliberately not a login form.  AxiomOS has one local
 * account; the lock surface shows presence (clock, date, battery, notifications)
 * and requires a deliberate gesture to leave, which is enough to make an
 * unattended screen unusable without inventing an authentication protocol that
 * the kernel does not yet implement.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXLS_MAX_NOTIFICATIONS 6
#define AXLS_NOTIFICATION_HEIGHT 84

typedef enum axls_notify_kind {
    AXLS_NOTIFY_INFO = 0,
    AXLS_NOTIFY_MESSAGE,
    AXLS_NOTIFY_CALENDAR,
    AXLS_NOTIFY_SYSTEM,
    AXLS_NOTIFY_WARNING
} axls_notify_kind_t;

typedef struct axls_notification {
    char                title[48];
    char                body[96];
    char                source[24];
    char                time[16];
    axls_notify_kind_t  kind;
    bool_t              read;
    bool_t              pinned;
} axls_notification_t;

typedef struct axls_quick_tile {
    char        label[24];
    char        detail[28];
    const char *glyph;
    bool_t      enabled;
} axls_quick_tile_t;

typedef struct axls_centre {
    gui_rect_t           rect;
    axls_notification_t  items[AXLS_MAX_NOTIFICATIONS];
    i32                  count;
    i32                  hover;
    i32                  selected;
    axls_quick_tile_t    tiles[6];
    i32                  tile_count;
    i32                  tile_hover;
    i32                  progress;      /* 0..1000 open animation */
    bool_t               open;
    bool_t               closing;
    i32                  unread;
} axls_centre_t;

typedef struct axls_lock {
    gui_rect_t rect;
    char       clock[16];
    char       date[40];
    char       greeting[48];
    char       user[24];
    char       hint[56];
    i32        progress;      /* 0..1000 unlock slide */
    bool_t     locked;
    bool_t     unlocking;
    i32        clock_blink;
    i32        notification_count;
} axls_lock_t;

/* ------------------------------------------------------------------ */
/* Lock screen                                                         */
/* ------------------------------------------------------------------ */

void axls_lock_init(axls_lock_t *lock, gui_rect_t rect)
{
    if (!lock) return;
    ax_memset(lock, 0, sizeof(*lock));
    lock->rect = rect;
    lock->locked = (bool_t)1;
    ax_strlcpy(lock->clock, "09:41", sizeof(lock->clock));
    ax_strlcpy(lock->date, "Saturday, 21 November", sizeof(lock->date));
    ax_strlcpy(lock->greeting, "Good morning", sizeof(lock->greeting));
    ax_strlcpy(lock->user, "Axiom", sizeof(lock->user));
    ax_strlcpy(lock->hint, "Press any key to continue", sizeof(lock->hint));
}

void axls_lock_set_time(axls_lock_t *lock, const char *clock, const char *date,
                        const char *greeting)
{
    if (!lock) return;
    if (clock) ax_strlcpy(lock->clock, clock, sizeof(lock->clock));
    if (date) ax_strlcpy(lock->date, date, sizeof(lock->date));
    if (greeting) ax_strlcpy(lock->greeting, greeting, sizeof(lock->greeting));
}

void axls_lock_unlock(axls_lock_t *lock)
{
    if (!lock || !lock->locked) return;
    lock->unlocking = (bool_t)1;
    lock->progress = 0;
}

/* Returns true while the unlock animation still needs frames.  When it finishes
 * the surface reports the session as unlocked exactly once, so the caller can
 * raise the shell. */
bool_t axls_lock_tick(axls_lock_t *lock)
{
    if (!lock) return (bool_t)0;
    lock->clock_blink = (lock->clock_blink + 1) % 60;
    if (!lock->unlocking) return (bool_t)0;
    lock->progress += 100;
    if (lock->progress >= 1000) {
        lock->progress = 1000;
        lock->unlocking = (bool_t)0;
        lock->locked = (bool_t)0;
        return (bool_t)0;
    }
    return (bool_t)1;
}

void axls_lock_paint(gui_surface_t *surface, axls_lock_t *lock,
                     const axau_palette_t *palette, u32 accent)
{
    if (!surface || !lock || !palette) return;
    /* A full-bleed field with a single large soft bloom behind the clock.  The
     * bloom is three concentric translucent circles rather than a gradient
     * texture; it reads the same and costs nothing to tint. */
    gui_fill_rect(surface, lock->rect,
                  axau_colour(palette, AXAU_COLOUR_SURFACE));
    i32 cx = lock->rect.x + lock->rect.w / 2;
    i32 cy = lock->rect.y + lock->rect.h / 3;
    if (accent) {
        for (i32 i = 3; i >= 1; i--) {
            axui_circle(surface, cx, cy, 120 + i * 40,
                        axui_colour_opacity(accent, 6 - i));
        }
    }

    i32 slide = lock->progress;
    i32 offset_y = (slide * lock->rect.h) / 1000;
    i32 clock_y = cy - 40 - offset_y * -1;
    axui_draw_text_center(surface,
                          gui_rect(lock->rect.x, clock_y - 20, lock->rect.w, 80),
                          lock->clock,
                          axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 6));
    axui_draw_text_center(surface,
                          gui_rect(lock->rect.x, clock_y + 56, lock->rect.w, 24),
                          lock->date,
                          axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 2));
    axui_draw_text_center(surface,
                          gui_rect(lock->rect.x, clock_y + 88, lock->rect.w, 20),
                          lock->greeting,
                          axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1));

    /* Presence badge at the bottom centre: avatar, name, hint. */
    i32 base_y = lock->rect.y + lock->rect.h - 96;
    gui_rect_t avatar = gui_rect(cx - 18, base_y, 36, 36);
    gui_fill_round_rect(surface, avatar, 18,
                        axui_colour_opacity(accent ? accent
                                                   : axau_colour(palette,
                                                                 AXAU_COLOUR_ACCENT),
                                            40));
    axau_icon_paint(surface, "u", cx, base_y + 18, 20,
                    axau_colour(palette, AXAU_COLOUR_ACCENT));
    axui_draw_text_center(surface, gui_rect(lock->rect.x, base_y + 44,
                                            lock->rect.w, 20), lock->user,
                          axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
    /* The hint pulses while the screen is idle; the pulse is what tells a user
     * that the surface is waiting for them rather than showing them a banner. */
    if (lock->locked && !lock->unlocking) {
        u32 hint_colour = axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY);
        if (lock->clock_blink < 20) {
            hint_colour = axau_colour(palette, AXAU_COLOUR_TEXT_DISABLED);
        }
        axui_draw_text_center(surface, gui_rect(lock->rect.x, base_y + 66,
                                                lock->rect.w, 18),
                              lock->hint, axui_style(hint_colour, 1));
    }

    if (lock->notification_count > 0) {
        char count[24];
        ax_snprintf(count, sizeof(count), "%d waiting", lock->notification_count);
        axui_draw_text_center(surface,
                              gui_rect(lock->rect.x, 24, lock->rect.w, 18), count,
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
}

/* Any pointer press or printable key can start the unlock.  The surface returns
 * whether it consumed the input so the shell does not also route it. */
bool_t axls_lock_input(axls_lock_t *lock, bool_t key_or_pointer)
{
    if (!lock) return (bool_t)0;
    if (!lock->locked) return (bool_t)0;
    if (!key_or_pointer) return (bool_t)1;
    axls_lock_unlock(lock);
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Notification centre                                                 */
/* ------------------------------------------------------------------ */

void axls_centre_init(axls_centre_t *centre, gui_rect_t rect)
{
    if (!centre) return;
    ax_memset(centre, 0, sizeof(*centre));
    centre->rect = rect;
    centre->hover = -1;
    centre->selected = -1;

    const char *labels[6] = {"Wi-Fi", "Bluetooth", "Night light", "Focus",
                             "Airplane", "Battery saver"};
    const char *details[6] = {"Axiom-5G", "Off", "Scheduled", "Working hours",
                              "Off", "82%"};
    const char *glyphs[6] = {"w", "L", "D", "t", "P", "b"};
    bool_t enabled[6] = {(bool_t)1, (bool_t)0, (bool_t)0, (bool_t)1,
                         (bool_t)0, (bool_t)0};
    for (i32 i = 0; i < 6; i++) {
        axls_quick_tile_t *tile = &centre->tiles[i];
        ax_strlcpy(tile->label, labels[i], sizeof(tile->label));
        ax_strlcpy(tile->detail, details[i], sizeof(tile->detail));
        tile->glyph = glyphs[i];
        tile->enabled = enabled[i];
    }
    centre->tile_count = 6;
}

void axls_centre_add(axls_centre_t *centre, axls_notify_kind_t kind,
                     const char *source, const char *title, const char *body,
                     const char *time)
{
    if (!centre || centre->count >= AXLS_MAX_NOTIFICATIONS) return;
    axls_notification_t *item = &centre->items[centre->count++];
    ax_memset(item, 0, sizeof(*item));
    item->kind = kind;
    ax_strlcpy(item->source, source ? source : "System", sizeof(item->source));
    ax_strlcpy(item->title, title ? title : "", sizeof(item->title));
    ax_strlcpy(item->body, body ? body : "", sizeof(item->body));
    ax_strlcpy(item->time, time ? time : "now", sizeof(item->time));
    centre->unread++;
}

void axls_centre_open(axls_centre_t *centre)
{
    if (!centre) return;
    centre->open = (bool_t)1;
    centre->closing = (bool_t)0;
    centre->progress = 0;
}

void axls_centre_close(axls_centre_t *centre)
{
    if (!centre) return;
    if (centre->open) centre->closing = (bool_t)1;
}

bool_t axls_centre_tick(axls_centre_t *centre)
{
    if (!centre) return (bool_t)0;
    if (centre->closing) {
        centre->progress -= 130;
        if (centre->progress <= 0) {
            centre->progress = 0;
            centre->open = (bool_t)0;
            centre->closing = (bool_t)0;
            return (bool_t)0;
        }
        return (bool_t)1;
    }
    if (centre->open && centre->progress < 1000) {
        centre->progress += 160;
        if (centre->progress > 1000) centre->progress = 1000;
        return (bool_t)1;
    }
    return (bool_t)0;
}

void axls_centre_dismiss(axls_centre_t *centre, i32 index)
{
    if (!centre || index < 0 || index >= centre->count) return;
    for (i32 i = index; i + 1 < centre->count; i++) {
        centre->items[i] = centre->items[i + 1];
    }
    centre->count--;
    if (centre->selected >= centre->count) centre->selected = centre->count - 1;
}

void axls_centre_clear(axls_centre_t *centre)
{
    if (!centre) return;
    centre->count = 0;
    centre->unread = 0;
    centre->selected = -1;
    centre->hover = -1;
}

static u32 notify_accent(axls_notify_kind_t kind, const axau_palette_t *palette)
{
    switch (kind) {
    case AXLS_NOTIFY_WARNING: return axau_colour(palette, AXAU_COLOUR_ERROR);
    case AXLS_NOTIFY_CALENDAR: return axau_colour(palette, AXAU_COLOUR_WARNING);
    case AXLS_NOTIFY_MESSAGE: return axau_colour(palette, AXAU_COLOUR_ACCENT);
    case AXLS_NOTIFY_SYSTEM: return axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY);
    case AXLS_NOTIFY_INFO:
    default: return axau_colour(palette, AXAU_COLOUR_SUCCESS);
    }
}

static const char *notify_glyph(axls_notify_kind_t kind)
{
    switch (kind) {
    case AXLS_NOTIFY_WARNING: return "!";
    case AXLS_NOTIFY_CALENDAR: return "c";
    case AXLS_NOTIFY_MESSAGE: return "e";
    case AXLS_NOTIFY_SYSTEM: return "*";
    case AXLS_NOTIFY_INFO:
    default: return "t";
    }
}

void axls_centre_paint(gui_surface_t *surface, axls_centre_t *centre,
                       const axau_palette_t *palette)
{
    if (!surface || !centre || !palette) return;
    if (!centre->open) return;
    i32 slide = (1000 - centre->progress) / 8;
    gui_rect_t panel = gui_rect(centre->rect.x + slide, centre->rect.y,
                                centre->rect.w, centre->rect.h);
    gui_shadow(surface, panel, 16, 40);
    gui_fill_round_rect(surface, panel, AXAU_RADIUS_FLYOUT,
                        axau_colour(palette, AXAU_COLOUR_SURFACE_ELEVATED));
    axui_round_rect_border(surface, panel, AXAU_RADIUS_FLYOUT, 1,
                           axau_colour(palette, AXAU_COLOUR_STROKE));

    axui_draw_text(surface, panel.x + 16, panel.y + 14, "Quick settings",
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
    i32 tile_w = (panel.w - 32 - 16) / 3;
    i32 tile_h = 64;
    for (i32 i = 0; i < centre->tile_count; i++) {
        i32 column = i % 3;
        i32 row = i / 3;
        gui_rect_t tile = gui_rect(panel.x + 16 + column * (tile_w + 8),
                                   panel.y + 44 + row * (tile_h + 8), tile_w,
                                   tile_h);
        axls_quick_tile_t *quick = &centre->tiles[i];
        gui_fill_round_rect(surface, tile, 6,
                            quick->enabled
                                ? axui_colour_opacity(
                                      axau_colour(palette, AXAU_COLOUR_ACCENT), 22)
                                : axau_colour(palette, AXAU_COLOUR_CONTROL));
        if (centre->tile_hover == i) {
            axui_round_rect_border(surface, tile, 6, 1,
                                   axau_colour(palette, AXAU_COLOUR_ACCENT));
        }
        axau_icon_paint(surface, quick->glyph, tile.x + 18, tile.y + 20, 14,
                        quick->enabled
                            ? axau_colour(palette, AXAU_COLOUR_ACCENT)
                            : axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY));
        axui_draw_text(surface, tile.x + 10, tile.y + 34, quick->label,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        axui_draw_text(surface, tile.x + 10, tile.y + 48, quick->detail,
                       axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 1));
    }

    i32 header_y = panel.y + 44 + 2 * (tile_h + 8) + 12;
    axui_draw_text(surface, panel.x + 16, header_y, "Notifications",
                   axui_style(axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY), 1));
    if (centre->unread > 0) {
        char badge[16];
        ax_snprintf(badge, sizeof(badge), "%d new", centre->unread);
        axui_draw_text_right(surface,
                             gui_rect(panel.x, header_y, panel.w - 16, 16), badge,
                             axui_style(axau_colour(palette, AXAU_COLOUR_ACCENT), 1));
    }

    i32 y = header_y + 26;
    for (i32 i = 0; i < centre->count; i++) {
        axls_notification_t *item = &centre->items[i];
        if (y + AXLS_NOTIFICATION_HEIGHT > panel.y + panel.h - 8) break;
        gui_rect_t card = gui_rect(panel.x + 12, y, panel.w - 24,
                                   AXLS_NOTIFICATION_HEIGHT - 8);
        gui_fill_round_rect(surface, card, AXAU_RADIUS_CARD,
                            axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
        if (centre->hover == i || centre->selected == i) {
            axui_round_rect_border(surface, card, AXAU_RADIUS_CARD, 1,
                                   axau_colour(palette, AXAU_COLOUR_STROKE_STRONG));
        }
        u32 accent = notify_accent(item->kind, palette);
        gui_fill_round_rect(surface, gui_rect(card.x, card.y, 3, card.h), 1,
                            accent);
        axau_icon_paint(surface, notify_glyph(item->kind), card.x + 22,
                        card.y + 20, 14, accent);
        axui_draw_text_clip(surface,
                            gui_rect(card.x + 38, card.y + 10, card.w - 80, 18),
                            card.x + 38, card.y + 10, item->title,
                            axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 1));
        axui_draw_text_right(surface,
                             gui_rect(card.x, card.y + 10, card.w - 12, 18),
                             item->time,
                             axui_style(axau_colour(palette,
                                                    AXAU_COLOUR_TEXT_TERTIARY), 1));
        axui_draw_text_clip(surface,
                            gui_rect(card.x + 12, card.y + 30, card.w - 24, 18),
                            card.x + 12, card.y + 30, item->body,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_SECONDARY), 1));
        axui_draw_text(surface, card.x + 12, card.y + 48, item->source,
                       axui_style(accent, 1));
        y += AXLS_NOTIFICATION_HEIGHT;
    }
    if (centre->count == 0) {
        axui_draw_text_center(surface,
                              gui_rect(panel.x, y + 16, panel.w, 24),
                              "No notifications",
                              axui_style(axau_colour(palette,
                                                     AXAU_COLOUR_TEXT_TERTIARY), 1));
    }
}

i32 axls_centre_hit(axls_centre_t *centre, i32 x, i32 y)
{
    if (!centre || !centre->open) return -1;
    if (!gui_rect_contains(centre->rect, x, y)) return -1;
    i32 tile_w = (centre->rect.w - 48) / 3;
    for (i32 i = 0; i < centre->tile_count; i++) {
        i32 column = i % 3;
        i32 row = i / 3;
        gui_rect_t tile = gui_rect(centre->rect.x + 16 + column * (tile_w + 8),
                                   centre->rect.y + 44 + row * 72, tile_w, 64);
        if (gui_rect_contains(tile, x, y)) {
            centre->tile_hover = i;
            return 1000 + i;
        }
    }
    i32 list_y = centre->rect.y + 44 + 144 + 38;
    i32 index = (y - list_y) / AXLS_NOTIFICATION_HEIGHT;
    if (index >= 0 && index < centre->count) {
        centre->selected = index;
        centre->items[index].read = (bool_t)1;
        if (centre->unread > 0) centre->unread--;
        return index;
    }
    return -1;
}

bool_t axls_centre_toggle_tile(axls_centre_t *centre, i32 index)
{
    if (!centre || index < 0 || index >= centre->tile_count) return (bool_t)0;
    centre->tiles[index].enabled = (bool_t)!centre->tiles[index].enabled;
    return centre->tiles[index].enabled;
}

i32 axls_centre_unread(const axls_centre_t *centre)
{
    return centre ? centre->unread : 0;
}

void axls_dump(const axls_centre_t *centre, const axls_lock_t *lock)
{
    if (centre) {
        printk("centre: %d notifications, %d unread, open=%d\n", centre->count,
               centre->unread, centre->open);
    }
    if (lock) {
        printk("lock: locked=%d unlocking=%d clock=%s\n", lock->locked,
               lock->unlocking, lock->clock);
    }
}

