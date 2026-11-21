/*
 * AxiomOS - Aurora shell interaction.
 *
 * Pointer routing for the shell follows one rule: the topmost overlay gets the
 * event first, then the bar, then the desktop.  That ordering is what makes a
 * launcher click land on the launcher even when it overlaps the bar, and it
 * avoids the classic bug where clicking a tile also toggles the bar underneath.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"
#include "aurora_shell.h"

/* ------------------------------------------------------------------ */
/* App registry                                                        */
/* ------------------------------------------------------------------ */

void axau_shell_add_app(axau_shell_t *shell, const char *name, const char *glyph,
                        u32 accent, bool_t pinned)
{
    if (!shell || !name || shell->app_count >= AXAU_LAUNCHER_APPS) {
        return;
    }
    axau_shell_app_t *app = &shell->apps[shell->app_count];
    ax_memset(app, 0, sizeof(*app));
    ax_strlcpy(app->name, name, sizeof(app->name));
    ax_strlcpy(app->glyph, glyph ? glyph : "*", sizeof(app->glyph));
    app->accent = accent ? accent : axau_colour(&axau_context()->palette,
                                                AXAU_COLOUR_ACCENT);
    app->pinned = pinned;
    shell->app_count++;
}

i32 axau_shell_find_app(const axau_shell_t *shell, const char *name)
{
    if (!shell || !name) return -1;
    for (i32 i = 0; i < shell->app_count; i++) {
        if (ax_strcmp(shell->apps[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

void axau_shell_launch(axau_shell_t *shell, i32 index)
{
    if (!shell || index < 0 || index >= shell->app_count) {
        return;
    }
    axau_shell_app_t *app = &shell->apps[index];
    app->running = (bool_t)1;
    app->launches++;
    axau_shell_close_launcher(shell);
    printk("aurora: launch %s (%u)\n", app->name, app->launches);
}

/* ------------------------------------------------------------------ */
/* Notifications                                                       */
/* ------------------------------------------------------------------ */

void axau_shell_notify(axau_shell_t *shell, const char *source,
                       const char *title, const char *body, i32 level)
{
    if (!shell || !title) return;
    axau_shell_notification_t *note = 0;
    if (shell->notification_count < AXAU_NOTIFY_MAX) {
        note = &shell->notifications[shell->notification_count++];
    } else {
        /* Oldest is dropped; a notification stack that grows without bound is
         * worse than losing the oldest toast. */
        for (i32 i = 0; i < AXAU_NOTIFY_MAX - 1; i++) {
            shell->notifications[i] = shell->notifications[i + 1];
        }
        note = &shell->notifications[AXAU_NOTIFY_MAX - 1];
    }
    ax_memset(note, 0, sizeof(*note));
    ax_strlcpy(note->source, source ? source : "System", sizeof(note->source));
    ax_strlcpy(note->title, title, sizeof(note->title));
    ax_strlcpy(note->body, body ? body : "", sizeof(note->body));
    note->level = level;
    note->unread = (bool_t)1;
    note->timestamp_ns = 0;
    note->anim = 0;
}

void axau_shell_dismiss(axau_shell_t *shell, i32 index)
{
    if (!shell || index < 0 || index >= shell->notification_count) return;
    for (i32 i = index; i + 1 < shell->notification_count; i++) {
        shell->notifications[i] = shell->notifications[i + 1];
    }
    shell->notification_count--;
    if (shell->hover_notification >= shell->notification_count) {
        shell->hover_notification = -1;
    }
}

void axau_shell_clear_notifications(axau_shell_t *shell)
{
    if (!shell) return;
    shell->notification_count = 0;
    shell->hover_notification = -1;
}

i32 axau_shell_unread(const axau_shell_t *shell)
{
    if (!shell) return 0;
    i32 count = 0;
    for (i32 i = 0; i < shell->notification_count; i++) {
        if (shell->notifications[i].unread) count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Overlay state                                                       */
/* ------------------------------------------------------------------ */

void axau_shell_open_launcher(axau_shell_t *shell)
{
    if (!shell) return;
    shell->launcher_open = (bool_t)1;
    shell->action_open = (bool_t)0;
    shell->search_active = (bool_t)1;
}

void axau_shell_close_launcher(axau_shell_t *shell)
{
    if (!shell) return;
    shell->launcher_open = (bool_t)0;
    shell->search_active = (bool_t)0;
}

void axau_shell_open_actions(axau_shell_t *shell)
{
    if (!shell) return;
    shell->action_open = (bool_t)1;
    shell->launcher_open = (bool_t)0;
}

void axau_shell_close_actions(axau_shell_t *shell)
{
    if (!shell) return;
    shell->action_open = (bool_t)0;
}

bool_t axau_shell_any_overlay(const axau_shell_t *shell)
{
    return (bool_t)(shell && (shell->launcher_open || shell->action_open));
}

/* ------------------------------------------------------------------ */
/* Clock and status                                                    */
/* ------------------------------------------------------------------ */

void axau_shell_set_clock(axau_shell_t *shell, u8 hour, u8 minute,
                          const char *date)
{
    if (!shell) return;
    shell->hour = hour;
    shell->minute = minute;
    if (date) {
        ax_strlcpy(shell->date_text, date, sizeof(shell->date_text));
    }
}

/* ------------------------------------------------------------------ */
/* Pointer routing                                                     */
/* ------------------------------------------------------------------ */

static void launcher_layout_tiles(const axau_shell_t *shell, gui_rect_t *tiles,
                                  i32 capacity)
{
    i32 padding = 24;
    i32 columns = 6;
    i32 tile_w = (shell->launcher_rect.w - padding * 2 - (columns - 1) * 8) / columns;
    i32 tile_h = 88;
    i32 top = shell->launcher_rect.y + padding + 44;
    for (i32 i = 0; i < shell->app_count && i < capacity; i++) {
        i32 column = i % columns;
        i32 row = i / columns;
        tiles[i] = gui_rect(shell->launcher_rect.x + padding +
                                column * (tile_w + 8),
                            top + row * (tile_h + 8), tile_w, tile_h);
    }
}

static i32 launcher_hit_app(axau_shell_t *shell, i32 x, i32 y)
{
    gui_rect_t tiles[18];
    launcher_layout_tiles(shell, tiles, 18);
    for (i32 i = 0; i < shell->app_count && i < 18; i++) {
        if (gui_rect_contains(tiles[i], x, y)) {
            return i;
        }
    }
    return -1;
}

i32 axau_shell_pointer_move(axau_shell_t *shell, i32 x, i32 y)
{
    if (!shell) return 0;
    if (shell->launcher_open) {
        shell->hover_app = launcher_hit_app(shell, x, y);
        return 1;
    }
    if (shell->action_open) {
        const axau_context_t *context = axau_context();
        i32 padding = 16;
        i32 columns = 3;
        i32 tile_w = (shell->action_rect.w - padding * 2 - (columns - 1) * 8) / columns;
        i32 tile_h = 72;
        for (i32 i = 0; i < shell->tile_count; i++) {
            i32 column = i % columns;
            i32 row = i / columns;
            gui_rect_t tile = gui_rect(shell->action_rect.x + padding +
                                           column * (tile_w + 8),
                                       shell->action_rect.y + padding +
                                           row * (tile_h + 8),
                                       tile_w, tile_h);
            shell->tiles[i].hover = gui_rect_contains(tile, x, y);
        }
        (void)context;
        return 1;
    }
    if (!shell->bar_visible && y >= shell->height - 4) {
        shell->bar_visible = (bool_t)1;
        shell->bar_reveal = 0;
        return 1;
    }
    if (shell->bar_visible && y >= shell->height - AXAU_BAR_HEIGHT) {
        return 1;
    }
    return 0;
}

/* A click on the action button's right half opens the panel; the left half is
 * reserved for the notification glyph, matching the two-zone cluster. */
i32 axau_shell_pointer_down(axau_shell_t *shell, i32 x, i32 y, u32 buttons)
{
    if (!shell) return 0;
    if (buttons != 0x01) {
        return 0;
    }
    if (shell->launcher_open && !gui_rect_contains(shell->launcher_rect, x, y)) {
        axau_shell_close_launcher(shell);
        return 1;
    }
    if (shell->action_open && !gui_rect_contains(shell->action_rect, x, y)) {
        axau_shell_close_actions(shell);
        return 1;
    }
    if (shell->launcher_open) {
        i32 index = launcher_hit_app(shell, x, y);
        if (index >= 0) {
            axau_shell_launch(shell, index);
            return 1;
        }
        return 1;
    }
    if (shell->action_open) {
        for (i32 i = 0; i < shell->tile_count; i++) {
            axau_action_tile_t *tile = &shell->tiles[i];
            if (gui_rect_contains(tile->rect, x, y)) {
                tile->on = (bool_t)!tile->on;
                return 1;
            }
        }
        return 1;
    }
    if (gui_rect_contains(shell->launcher_button, x, y)) {
        axau_shell_open_launcher(shell);
        return 1;
    }
    if (gui_rect_contains(shell->action_button, x, y)) {
        axau_shell_open_actions(shell);
        return 1;
    }
    if (gui_rect_contains(shell->search_button, x, y)) {
        axau_shell_open_launcher(shell);
        return 1;
    }
    return 0;
}

i32 axau_shell_pointer_up(axau_shell_t *shell, i32 x, i32 y, u32 buttons)
{
    (void)buttons;
    if (!shell) return 0;
    /* Nothing latches on release; selection already happened on press. */
    (void)x;
    (void)y;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

i32 axau_shell_key(axau_shell_t *shell, char key)
{
    if (!shell) return 0;
    if (key == 0x1B) {
        if (shell->launcher_open) {
            axau_shell_close_launcher(shell);
            return 1;
        }
        if (shell->action_open) {
            axau_shell_close_actions(shell);
            return 1;
        }
        return 0;
    }
    if (!shell->launcher_open) {
        return 0;
    }
    usize length = ax_strlen(shell->search);
    if (key == 8) {
        if (length > 0) {
            shell->search[length - 1] = 0;
        }
        return 1;
    }
    if (key == '\n' || key == '\r') {
        if (shell->app_count > 0) {
            axau_shell_launch(shell, shell->selected_result);
        }
        return 1;
    }
    if (key >= 0x20 && key <= 0x7E && length + 1 < sizeof(shell->search)) {
        shell->search[length] = key;
        shell->search[length + 1] = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

void axau_shell_dump(const axau_shell_t *shell)
{
    if (!shell) return;
    printk("aurora: bar %dx%d at %d,%d apps=%d notifications=%d unread=%d\n",
           shell->bar.w, shell->bar.h, shell->bar.x, shell->bar.y,
           shell->app_count, shell->notification_count,
           axau_shell_unread(shell));
    printk("aurora: launcher=%d anim=%d action=%d anim=%d clock=%02u:%02u\n",
           shell->launcher_open, shell->launcher_anim, shell->action_open,
           shell->action_anim, shell->hour, shell->minute);
}

/* Small invariant suite: geometry must stay on screen, toggles must be
 * reversible, and unread counts must not drift. */
i32 axau_shell_selftest(void)
{
    axau_shell_t shell;
    axau_shell_init(&shell, 1024, 768);
    axau_shell_add_app(&shell, "Terminal", ">", 0, (bool_t)1);
    axau_shell_add_app(&shell, "Files", "=", 0, (bool_t)1);
    axau_shell_add_app(&shell, "Settings", "*", 0, (bool_t)0);
    if (shell.app_count != 3) {
        return -1;
    }
    if (shell.bar.y + shell.bar.h != shell.height) {
        return -2;
    }
    if (shell.launcher_rect.x < 0 ||
        shell.launcher_rect.x + shell.launcher_rect.w > shell.width) {
        return -3;
    }
    axau_shell_notify(&shell, "System", "Test", "Body", 0);
    if (axau_shell_unread(&shell) != 1) {
        return -4;
    }
    axau_shell_dismiss(&shell, 0);
    if (shell.notification_count != 0 || axau_shell_unread(&shell) != 0) {
        return -5;
    }
    axau_shell_open_launcher(&shell);
    if (!shell.launcher_open || shell.action_open) {
        return -6;
    }
    axau_shell_open_actions(&shell);
    if (shell.launcher_open || !shell.action_open) {
        return -7;
    }
    axau_shell_close_actions(&shell);
    if (shell.action_open) {
        return -8;
    }
    if (axau_shell_find_app(&shell, "Settings") != 2) {
        return -9;
    }
    axau_shell_tick(&shell, 16);
    return 0;
}
