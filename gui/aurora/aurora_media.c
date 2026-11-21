/*
 * AxiomOS - media player surface.
 *
 * Playback state, a playlist, a transport bar and a waveform visualiser.  The
 * player does not decode anything: it models what a decoder would report and
 * renders the controls that surround it.  Keeping that boundary explicit means
 * the surface can be finished now and wired to a real decoder when one exists,
 * without rewriting the controls.
 *
 * Seeking is expressed in per-mille of duration rather than seconds, so a track
 * of any length maps onto the same timeline geometry and the same hit-testing
 * code.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include <axiom/lib.h>
#include "gui.h"
#include "ui.h"
#include "aurora.h"

#define AXMP_MAX_TRACKS     16
#define AXMP_VISUALIZER_BARS 32
#define AXMP_TRANSPORT_HEIGHT 96

typedef enum axmp_state {
    AXMP_STOPPED = 0,
    AXMP_PLAYING,
    AXMP_PAUSED,
    AXMP_BUFFERING
} axmp_state_t;

typedef enum axmp_repeat {
    AXMP_REPEAT_OFF = 0,
    AXMP_REPEAT_ALL,
    AXMP_REPEAT_ONE
} axmp_repeat_t;

typedef struct axmp_track {
    char title[48];
    char artist[36];
    char album[36];
    i32  duration_seconds;
    bool_t favourite;
    bool_t explicit_lyrics;
} axmp_track_t;

typedef struct axmp_player {
    gui_rect_t      rect;
    axmp_track_t    tracks[AXMP_MAX_TRACKS];
    i32             track_count;
    i32             current;
    axmp_state_t    state;
    axmp_repeat_t   repeat;
    bool_t          shuffle;
    bool_t          muted;
    i32             volume;         /* 0..100 */
    i32             position;       /* per-mille of the track */
    i32             buffered;       /* per-mille downloaded */
    i32             visualizer[AXMP_VISUALIZER_BARS];
    i32             visualizer_phase;
    i32             hover_row;
    i32             selected_row;
    bool_t          queue_open;
    gui_rect_t      timeline;
    gui_rect_t      queue_rows[AXMP_MAX_TRACKS];
} axmp_player_t;

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

void axmp_init(axmp_player_t *player, gui_rect_t rect)
{
    if (!player) return;
    ax_memset(player, 0, sizeof(*player));
    player->rect = rect;
    player->state = AXMP_STOPPED;
    player->volume = 60;
    player->hover_row = -1;
    player->selected_row = -1;
    player->repeat = AXMP_REPEAT_OFF;
}

void axmp_add_track(axmp_player_t *player, const char *title, const char *artist,
                    const char *album, i32 duration_seconds)
{
    if (!player || player->track_count >= AXMP_MAX_TRACKS) return;
    axmp_track_t *track = &player->tracks[player->track_count++];
    ax_memset(track, 0, sizeof(*track));
    ax_strlcpy(track->title, title ? title : "", sizeof(track->title));
    ax_strlcpy(track->artist, artist ? artist : "", sizeof(track->artist));
    ax_strlcpy(track->album, album ? album : "", sizeof(track->album));
    track->duration_seconds = duration_seconds > 0 ? duration_seconds : 1;
}

axmp_track_t *axmp_current(axmp_player_t *player)
{
    if (!player || player->track_count == 0) return 0;
    if (player->current < 0 || player->current >= player->track_count) {
        player->current = 0;
    }
    return &player->tracks[player->current];
}

void axmp_play(axmp_player_t *player)
{
    if (!player || player->track_count == 0) return;
    player->state = AXMP_PLAYING;
}

void axmp_pause(axmp_player_t *player)
{
    if (!player) return;
    if (player->state == AXMP_PLAYING) player->state = AXMP_PAUSED;
}

void axmp_toggle(axmp_player_t *player)
{
    if (!player) return;
    if (player->state == AXMP_PLAYING) {
        axmp_pause(player);
    } else {
        axmp_play(player);
    }
}

/* Next respects repeat-one: a finished track with repeat-one resets to zero
 * instead of advancing.  That is not a shortcut; it is the difference between
 * "repeat" and "repeat this". */
void axmp_next(axmp_player_t *player)
{
    if (!player || player->track_count == 0) return;
    if (player->repeat == AXMP_REPEAT_ONE) {
        player->position = 0;
        return;
    }
    if (player->shuffle) {
        /* A shuffle that can pick the same track twice in a row is not a shuffle
         * anyone wants; step forward by a coprime offset derived from the
         * current index so the walk is deterministic but not sequential. */
        i32 offset = 1 + (player->current % 3);
        player->current = (player->current + offset) % player->track_count;
    } else {
        player->current++;
        if (player->current >= player->track_count) {
            if (player->repeat == AXMP_REPEAT_ALL) {
                player->current = 0;
            } else {
                player->current = player->track_count - 1;
                player->state = AXMP_STOPPED;
            }
        }
    }
    player->position = 0;
}

void axmp_previous(axmp_player_t *player)
{
    if (!player || player->track_count == 0) return;
    /* The conventional behaviour: restart the track if past the first few
     * seconds, otherwise go back one. */
    if (player->position > 30) {
        player->position = 0;
        return;
    }
    player->current--;
    if (player->current < 0) {
        player->current = player->repeat == AXMP_REPEAT_ALL
                              ? player->track_count - 1 : 0;
    }
    player->position = 0;
}

/* Seek takes per-mille; the range is clamped rather than wrapped, because a
 * drag past the end should land at the end. */
void axmp_seek(axmp_player_t *player, i32 per_mille)
{
    if (!player) return;
    if (per_mille < 0) per_mille = 0;
    if (per_mille > 1000) per_mille = 1000;
    player->position = per_mille;
}

void axmp_volume(axmp_player_t *player, i32 volume)
{
    if (!player) return;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    player->volume = volume;
    player->muted = (bool_t)(volume == 0);
}

void axmp_cycle_repeat(axmp_player_t *player)
{
    if (!player) return;
    player->repeat = (axmp_repeat_t)(((i32)player->repeat + 1) % 3);
}

/* Advance the transport clock by a number of ticks.  One tick is treated as one
 * second worth of progress in per-mille terms only when the caller passes the
 * exact count; the function takes milliseconds so the caller can drive it from
 * the compositor's frame time. */
void axmp_tick(axmp_player_t *player, i32 elapsed_ms)
{
    if (!player || player->state != AXMP_PLAYING) return;
    axmp_track_t *track = axmp_current(player);
    if (!track) return;
    i32 per_mille = (elapsed_ms * 1000) / (track->duration_seconds * 1000);
    if (per_mille <= 0) per_mille = 1;
    player->position += per_mille;
    if (player->position >= 1000) {
        if (player->repeat == AXMP_REPEAT_ONE) {
            player->position = 0;
        } else {
            axmp_next(player);
        }
    }
    if (player->buffered < player->position) {
        player->buffered = player->position + 40;
        if (player->buffered > 1000) player->buffered = 1000;
    }
}

/* The visualiser is driven by a small integer oscillator rather than audio
 * samples: without a decoder there are no samples, and a fake spectrum that
 * moves deterministically still communicates "playing". */
void axmp_visualise(axmp_player_t *player)
{
    if (!player) return;
    player->visualizer_phase = (player->visualizer_phase + 1) % 64;
    for (i32 i = 0; i < AXMP_VISUALIZER_BARS; i++) {
        i32 base = 30 + (i * 3) % 40;
        i32 wobble = (player->visualizer_phase + i * 5) % 32;
        i32 height = base + (wobble < 16 ? wobble : 32 - wobble);
        if (player->state != AXMP_PLAYING) height = 12 + (i % 4) * 2;
        player->visualizer[i] = height;
    }
}

/* ------------------------------------------------------------------ */
/* Layout and paint                                                    */
/* ------------------------------------------------------------------ */

static void axmp_format_time(i32 seconds, char *out, i32 capacity)
{
    if (seconds < 0) seconds = 0;
    ax_snprintf(out, capacity, "%d:%02d", seconds / 60, seconds % 60);
}

void axmp_paint(gui_surface_t *surface, axmp_player_t *player,
                const axau_palette_t *palette)
{
    if (!surface || !player || !palette) return;
    gui_fill_rect(surface, player->rect, axau_colour(palette, AXAU_COLOUR_SURFACE));

    /* Now playing header with album art stand-in. */
    gui_rect_t art = gui_rect(player->rect.x + 20, player->rect.y + 20, 96, 96);
    gui_fill_round_rect(surface, art, 8,
                        axui_colour_opacity(
                            axau_colour(palette, AXAU_COLOUR_ACCENT), 24));
    axau_icon_paint(surface, "m", art.x + art.w / 2, art.y + art.h / 2, 40,
                    axau_colour(palette, AXAU_COLOUR_ACCENT));

    const axmp_track_t *track = axmp_current(player);
    if (track) {
        axui_draw_text_clip(surface,
                            gui_rect(art.x + 116, art.y + 6,
                                     player->rect.w - 160, 22),
                            art.x + 116, art.y + 6, track->title,
                            axui_style(axau_colour(palette, AXAU_COLOUR_TEXT), 2));
        axui_draw_text_clip(surface,
                            gui_rect(art.x + 116, art.y + 32,
                                     player->rect.w - 160, 18),
                            art.x + 116, art.y + 32, track->artist,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_SECONDARY), 1));
        axui_draw_text_clip(surface,
                            gui_rect(art.x + 116, art.y + 52,
                                     player->rect.w - 160, 18),
                            art.x + 116, art.y + 52, track->album,
                            axui_style(axau_colour(palette,
                                                   AXAU_COLOUR_TEXT_TERTIARY), 1));
        /* Explicit-content marker is a small outlined box; a parental advisory
         * graphic would be a licensed asset and is not needed here. */
        if (track->explicit_lyrics) {
            gui_rect_t badge = gui_rect(art.x + 116, art.y + 74, 18, 14);
            axui_round_rect_border(surface, badge, 2, 1,
                                   axau_colour(palette,
                                               AXAU_COLOUR_TEXT_TERTIARY));
            axui_draw_text_center(surface, badge, "E",
                                  axui_style(axau_colour(palette,
                                                         AXAU_COLOUR_TEXT_TERTIARY), 1));
        }
    }

    /* Visualiser occupies the empty space to the right of the header. */
    i32 bars_x = art.x + art.w + 40;
    i32 bars_w = player->rect.w - (bars_x - player->rect.x) - 24;
    i32 bar_w = bars_w / AXMP_VISUALIZER_BARS;
    if (bar_w < 2) bar_w = 2;
    i32 bars_bottom = player->rect.y + player->rect.h - AXMP_TRANSPORT_HEIGHT - 24;
    axmp_visualise(player);
    for (i32 i = 0; i < AXMP_VISUALIZER_BARS; i++) {
        i32 height = player->visualizer[i];
        if (height > 120) height = 120;
        gui_fill_round_rect(surface,
                            gui_rect(bars_x + i * bar_w, bars_bottom - height,
                                     bar_w - 2, height), 1,
                            axui_colour_opacity(
                                axau_colour(palette, AXAU_COLOUR_ACCENT),
                                90 + (i % 4) * 30));
    }

    /* Queue list. */
    if (player->queue_open) {
        i32 y = player->rect.y + 136;
        for (i32 i = 0; i < player->track_count; i++) {
            gui_rect_t row = gui_rect(player->rect.x + 16, y,
                                      player->rect.w - 32, 40);
            player->queue_rows[i] = row;
            if (i == player->current) {
                gui_fill_round_rect(surface, row, 4,
                                    axui_colour_opacity(
                                        axau_colour(palette, AXAU_COLOUR_ACCENT), 14));
            } else if (player->hover_row == i || player->selected_row == i) {
                gui_fill_round_rect(surface, row, 4,
                                    axau_colour(palette,
                                                AXAU_COLOUR_CONTROL_HOVER));
            }
            char number[12];
            ax_snprintf(number, sizeof(number), "%d", i + 1);
            axui_draw_text(surface, row.x + 8, row.y + 12, number,
                           axui_style(axau_colour(palette,
                                                  AXAU_COLOUR_TEXT_TERTIARY), 1));
            axau_icon_paint(surface, i == player->current ? "m" : ".",
                            row.x + 32, row.y + 20, 12,
                            axau_colour(palette,
                                        i == player->current
                                            ? AXAU_COLOUR_ACCENT
                                            : AXAU_COLOUR_TEXT_SECONDARY));
            axui_draw_text_clip(surface,
                                gui_rect(row.x + 48, row.y + 6, row.w - 160, 16),
                                row.x + 48, row.y + 6, player->tracks[i].title,
                                axui_style(axau_colour(palette,
                                                       AXAU_COLOUR_TEXT), 1));
            axui_draw_text_clip(surface,
                                gui_rect(row.x + 48, row.y + 22, row.w - 160, 14),
                                row.x + 48, row.y + 22, player->tracks[i].artist,
                                axui_style(axau_colour(palette,
                                                       AXAU_COLOUR_TEXT_TERTIARY), 1));
            char duration[12];
            axmp_format_time(player->tracks[i].duration_seconds, duration,
                             sizeof(duration));
            axui_draw_text_right(surface,
                                 gui_rect(row.x, row.y, row.w - 12, row.h),
                                 duration,
                                 axui_style(axau_colour(palette,
                                                        AXAU_COLOUR_TEXT_TERTIARY), 1));
            y += 44;
        }
    }

    /* Transport bar. */
    gui_rect_t transport = gui_rect(player->rect.x, player->rect.y +
                                    player->rect.h - AXMP_TRANSPORT_HEIGHT,
                                    player->rect.w, AXMP_TRANSPORT_HEIGHT);
    gui_fill_rect(surface, transport,
                  axau_colour(palette, AXAU_COLOUR_SURFACE_ALT));
    player->timeline = gui_rect(transport.x + 56, transport.y + 18,
                                transport.w - 112, 6);
    gui_fill_round_rect(surface, player->timeline, 3,
                        axau_colour(palette, AXAU_COLOUR_CONTROL));
    i32 buffered_w = (player->timeline.w * player->buffered) / 1000;
    gui_fill_round_rect(surface,
                        gui_rect(player->timeline.x, player->timeline.y,
                                 buffered_w, player->timeline.h), 3,
                        axui_colour_opacity(
                            axau_colour(palette, AXAU_COLOUR_TEXT_TERTIARY), 60));
    i32 progress_w = (player->timeline.w * player->position) / 1000;
    gui_fill_round_rect(surface,
                        gui_rect(player->timeline.x, player->timeline.y,
                                 progress_w, player->timeline.h), 3,
                        axau_colour(palette, AXAU_COLOUR_ACCENT));
    axui_circle(surface, player->timeline.x + progress_w,
                player->timeline.y + player->timeline.h / 2, 6,
                axau_colour(palette, AXAU_COLOUR_ACCENT));

    if (track) {
        char elapsed[12];
        char total[12];
        i32 elapsed_seconds = (track->duration_seconds * player->position) / 1000;
        axmp_format_time(elapsed_seconds, elapsed, sizeof(elapsed));
        axmp_format_time(track->duration_seconds, total, sizeof(total));
        axui_draw_text_right(surface,
                             gui_rect(transport.x, transport.y + 10, 52, 18),
                             elapsed,
                             axui_style(axau_colour(palette,
                                                    AXAU_COLOUR_TEXT_SECONDARY), 1));
        axui_draw_text(surface, transport.x + transport.w - 52,
                       transport.y + 10, total,
                       axui_style(axau_colour(palette,
                                              AXAU_COLOUR_TEXT_SECONDARY), 1));
    }

    /* Buttons: previous, play/pause, next, repeat, shuffle, volume. */
    i32 centre_x = transport.x + transport.w / 2;
    i32 button_y = transport.y + 46;
    gui_rect_t play = gui_rect(centre_x - 20, button_y - 12, 40, 40);
    gui_fill_round_rect(surface, play, 20,
                        axau_colour(palette, AXAU_COLOUR_ACCENT));
    axau_icon_paint(surface, player->state == AXMP_PLAYING ? "p" : ">",
                    centre_x, button_y + 8, 18,
                    axau_colour(palette, AXAU_COLOUR_ACCENT_TEXT));
    axau_icon_paint(surface, "<", centre_x - 56, button_y + 8, 16,
                    axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY));
    axau_icon_paint(surface, ">", centre_x + 56, button_y + 8, 16,
                    axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY));
    axau_icon_paint(surface, "r", centre_x - 96, button_y + 8, 14,
                    axau_colour(palette,
                                player->repeat != AXMP_REPEAT_OFF
                                    ? AXAU_COLOUR_ACCENT
                                    : AXAU_COLOUR_TEXT_TERTIARY));
    axau_icon_paint(surface, "s", centre_x + 96, button_y + 8, 14,
                    axau_colour(palette, player->shuffle
                                             ? AXAU_COLOUR_ACCENT
                                             : AXAU_COLOUR_TEXT_TERTIARY));
    axau_icon_paint(surface, player->muted ? "x" : "s",
                    transport.x + transport.w - 132, button_y + 8, 14,
                    axau_colour(palette, AXAU_COLOUR_TEXT_SECONDARY));
    gui_rect_t volume = gui_rect(transport.x + transport.w - 108, button_y + 5,
                                 64, 6);
    gui_fill_round_rect(surface, volume, 3,
                        axau_colour(palette, AXAU_COLOUR_CONTROL));
    gui_fill_round_rect(surface,
                        gui_rect(volume.x, volume.y,
                                 (volume.w * player->volume) / 100, volume.h), 3,
                        axau_colour(palette, AXAU_COLOUR_ACCENT));
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

i32 axmp_click(axmp_player_t *player, i32 x, i32 y)
{
    if (!player) return 0;
    i32 centre_x = player->rect.x + player->rect.w / 2;
    i32 button_y = player->rect.y + player->rect.h - AXMP_TRANSPORT_HEIGHT + 46;

    if (gui_rect_contains(gui_rect(centre_x - 40, button_y - 12, 80, 40), x, y)) {
        axmp_toggle(player);
        return 1;
    }
    if (gui_rect_contains(gui_rect(centre_x - 72, button_y - 4, 32, 28), x, y)) {
        axmp_previous(player);
        return 2;
    }
    if (gui_rect_contains(gui_rect(centre_x + 40, button_y - 4, 32, 28), x, y)) {
        axmp_next(player);
        return 3;
    }
    if (gui_rect_contains(gui_rect(centre_x - 112, button_y - 4, 32, 28), x, y)) {
        axmp_cycle_repeat(player);
        return 4;
    }
    if (gui_rect_contains(gui_rect(centre_x + 80, button_y - 4, 32, 28), x, y)) {
        player->shuffle = (bool_t)!player->shuffle;
        return 5;
    }
    if (gui_rect_contains(player->timeline, x, y) ||
        (y >= player->timeline.y - 4 && y <= player->timeline.y + 10 &&
         x >= player->timeline.x && x <= player->timeline.x + player->timeline.w)) {
        i32 per_mille = ((x - player->timeline.x) * 1000) / player->timeline.w;
        axmp_seek(player, per_mille);
        return 6;
    }
    gui_rect_t volume = gui_rect(player->rect.x + player->rect.w - 108,
                                 button_y + 5, 64, 6);
    if (gui_rect_contains(gui_rect(volume.x, volume.y - 8, volume.w, 22), x, y)) {
        axmp_volume(player, ((x - volume.x) * 100) / volume.w);
        return 7;
    }
    if (player->queue_open) {
        for (i32 i = 0; i < player->track_count; i++) {
            if (!gui_rect_contains(player->queue_rows[i], x, y)) continue;
            player->current = i;
            player->position = 0;
            axmp_play(player);
            return 100 + i;
        }
    }
    return 0;
}

void axmp_hover(axmp_player_t *player, i32 x, i32 y)
{
    if (!player) return;
    player->hover_row = -1;
    for (i32 i = 0; i < player->track_count; i++) {
        if (gui_rect_contains(player->queue_rows[i], x, y)) {
            player->hover_row = i;
            return;
        }
    }
}

void axmp_dump(const axmp_player_t *player)
{
    if (!player) return;
    printk("media: %d tracks, current=%d state=%d position=%d volume=%d\n",
           player->track_count, player->current, (i32)player->state,
           player->position, player->volume);
}

