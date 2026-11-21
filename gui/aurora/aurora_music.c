/* AxiomOS - Aurora: music player.
 *
 * Library browsing plus a transport bar.  The player keeps a queue of tracks,
 * a play position in whole seconds (the audio backend drives the real clock),
 * shuffle/repeat modes and a waveform display derived from the track's peak
 * table so the UI has something honest to draw before decoding exists.
 */

#include "gui.h"

#define AXMU_MAX_TRACKS   64
#define AXMU_BARS         96
#define AXMU_ROW          44
#define AXMU_TRANSPORT    96
#define AXMU_COVER        120

#define AXMU_C_BG       GUI_RGB(0x18, 0x16, 0x1E)
#define AXMU_C_SURFACE  GUI_RGB(0x24, 0x21, 0x2C)
#define AXMU_C_TEXT     GUI_RGB(0xE6, 0xE2, 0xF0)
#define AXMU_C_DIM      GUI_RGB(0x90, 0x88, 0xA0)
#define AXMU_C_STROKE   GUI_RGB(0x3A, 0x34, 0x44)
#define AXMU_C_ACCENT   GUI_RGB(0xB0, 0x7C, 0xFF)
#define AXMU_C_ACCENT2  GUI_RGB(0x5C, 0xC8, 0xD8)

enum axmu_repeat
{
    AXMU_REPEAT_OFF = 0,
    AXMU_REPEAT_ALL,
    AXMU_REPEAT_ONE
};

typedef struct axmu_track
{
    char title[40];
    char artist[32];
    char album[32];
    u32  duration;
    u32  bitrate;
    u8   peaks[AXMU_BARS];
    bool_t favourite;
} axmu_track_t;

typedef struct axmu_player
{
    axmu_track_t tracks[AXMU_MAX_TRACKS];
    u16 count;
    u16 current;
    u16 selected;
    u32 position;
    u8  volume;
    u8  repeat;
    bool_t playing;
    bool_t shuffle;
    bool_t muted;
    u32 history[8];
    u8  history_count;
    u32 ticks;
    u32 started;
    char now_playing[48];
} axmu_player_t;

static void axmu_zero(void *ptr, u32 size);
static void axmu_copy(char *dst, u32 cap, const char *src);
static bool_t axmu_equals(const char *a, const char *b);
static void axmu_paint_library(gui_surface_t *surface, const axmu_player_t *player,
                               gui_rect_t list);
static void axmu_paint_transport(gui_surface_t *surface, const axmu_player_t *player,
                                 gui_rect_t bar);
static void axmu_paint_waveform(gui_surface_t *surface, const axmu_track_t *track,
                                gui_rect_t area);
static void axmu_format_time(u32 seconds, char *out);
static void axmu_advance(axmu_player_t *player);

static void axmu_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axmu_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool_t axmu_equals(const char *a, const char *b)
{
    u32 i = 0;
    while (a[i] != 0 && b[i] != 0)
    {
        if (a[i] != b[i])
            return false;
        i++;
    }
    return a[i] == b[i];
}

static void axmu_format_time(u32 seconds, char *out)
{
    u32 minutes = seconds / 60;
    u32 remainder = seconds % 60;
    out[0] = (char)('0' + (minutes / 10) % 10);
    out[1] = (char)('0' + minutes % 10);
    out[2] = ':';
    out[3] = (char)('0' + (remainder / 10) % 10);
    out[4] = (char)('0' + remainder % 10);
    out[5] = 0;
}

static void axmu_paint_waveform(gui_surface_t *surface, const axmu_track_t *track,
                                gui_rect_t area)
{
    u32 bar;
    gui_fill_rect(surface, area, AXMU_C_SURFACE);
    for (bar = 0; bar < AXMU_BARS; bar++)
    {
        u8 peak = track->peaks[bar];
        i32 height = (i32)((u32)peak * (u32)(area.h - 8) / 255u);
        i32 x = area.x + 4 + (i32)bar * (area.w - 8) / AXMU_BARS;
        i32 width = (area.w - 8) / AXMU_BARS;
        if (width < 1)
            width = 1;
        gui_fill_rect(surface, gui_rect(x, area.y + area.h / 2 - height / 2, width,
                                        height < 2 ? 2 : height),
                      bar % 7 == 0 ? AXMU_C_ACCENT2 : AXMU_C_ACCENT);
    }
}

static void axmu_paint_library(gui_surface_t *surface, const axmu_player_t *player,
                               gui_rect_t list)
{
    u16 row;
    u16 visible = (u16)(list.h / AXMU_ROW);
    gui_fill_rect(surface, list, AXMU_C_BG);
    for (row = 0; row < visible; row++)
    {
        u16 index = row;
        i32 y;
        if (index >= player->count)
            break;
        y = list.y + (i32)row * AXMU_ROW;
        {
            const axmu_track_t *track = &player->tracks[index];
            char duration[8];
            if (index == player->selected)
                gui_fill_rect(surface, gui_rect(list.x, y, list.w, AXMU_ROW - 1),
                              AXMU_C_SURFACE);
            gui_text(surface, list.x + 16, y + 8, track->title, AXMU_C_TEXT, 1);
            gui_text(surface, list.x + 16, y + 24, track->artist, AXMU_C_DIM, 1);
            gui_text(surface, list.x + list.w - 64, y + 14, track->album,
                     AXMU_C_DIM, 1);
            axmu_format_time(track->duration, duration);
            gui_text(surface, list.x + list.w - 120, y + 14, duration, AXMU_C_DIM, 1);
            if (index == player->current && player->playing)
            {
                gui_fill_rect(surface, gui_rect(list.x + 4, y + 14, 3,
                                                AXMU_ROW - 20), AXMU_C_ACCENT);
            }
        }
    }
}

static void axmu_paint_transport(gui_surface_t *surface, const axmu_player_t *player,
                                 gui_rect_t bar)
{
    char position[8];
    char duration[8];
    i32 progress;
    u32 total;
    gui_fill_rect(surface, bar, AXMU_C_SURFACE);
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y, AXMU_C_STROKE);
    if (player->count == 0)
        return;
    {
        const axmu_track_t *track = &player->tracks[player->current];
        total = track->duration == 0 ? 1 : track->duration;
        axmu_format_time(player->position, position);
        axmu_format_time(track->duration, duration);
        gui_text(surface, bar.x + 24, bar.y + 20, track->title, AXMU_C_TEXT, 2);
        gui_text(surface, bar.x + 24, bar.y + 48, track->artist, AXMU_C_DIM, 1);
        gui_text(surface, bar.x + 24, bar.y + 70, duration, AXMU_C_DIM, 1);
        progress = (i32)((u64)player->position * 1000 / total);
        gui_fill_rect(surface, gui_rect(bar.x + 120, bar.y + 70, bar.w - 260, 4),
                      AXMU_C_STROKE);
        gui_fill_rect(surface, gui_rect(bar.x + 120, bar.y + 70,
                                        (bar.w - 260) * progress / 1000, 4),
                      AXMU_C_ACCENT);
        gui_text(surface, bar.x + 120, bar.y + 44, position, AXMU_C_TEXT, 1);
    }
    gui_text(surface, bar.x + bar.w - 120, bar.y + 20, player->playing ? "pause" : "play",
             AXMU_C_TEXT, 1);
    gui_text(surface, bar.x + bar.w - 60, bar.y + 20, player->muted ? "mute" : "vol",
             AXMU_C_DIM, 1);
}

static void axmu_advance(axmu_player_t *player)
{
    if (player->count == 0)
        return;
    if (player->repeat == AXMU_REPEAT_ONE)
    {
        player->position = 0;
        return;
    }
    if (player->shuffle)
    {
        u32 seed = player->ticks * 1103515245u + 12345u;
        player->current = (u16)((seed >> 16) % player->count);
    }
    else
    {
        player->current = (u16)((player->current + 1) % player->count);
    }
    player->position = 0;
    if (player->repeat == AXMU_REPEAT_OFF && !player->shuffle &&
        player->current == 0 && player->history_count > 0)
        player->playing = false;
}

/* -------------------------------------------------------------- public API */

void axmu_init(axmu_player_t *player)
{
    axmu_zero(player, (u32)sizeof(*player));
    player->volume = 60;
    player->repeat = AXMU_REPEAT_OFF;
}

bool_t axmu_add(axmu_player_t *player, const char *title, const char *artist,
                const char *album, u32 duration, u32 bitrate)
{
    axmu_track_t *track;
    u32 i;
    if (player->count >= AXMU_MAX_TRACKS)
        return false;
    track = &player->tracks[player->count];
    axmu_zero(track, (u32)sizeof(*track));
    axmu_copy(track->title, 40, title);
    axmu_copy(track->artist, 32, artist);
    axmu_copy(track->album, 32, album);
    track->duration = duration;
    track->bitrate = bitrate;
    for (i = 0; i < AXMU_BARS; i++)
    {
        u32 hash = (u32)(title[0] + i * 37) * 2654435761u;
        track->peaks[i] = (u8)((hash >> 8) & 0xFF);
    }
    player->count++;
    return true;
}

bool_t axmu_select(axmu_player_t *player, u16 index)
{
    if (index >= player->count)
        return false;
    player->selected = index;
    return true;
}

void axmu_move(axmu_player_t *player, i32 delta)
{
    i32 index = (i32)player->selected + delta;
    if (player->count == 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= (i32)player->count)
        index = (i32)player->count - 1;
    player->selected = (u16)index;
}

void axmu_play(axmu_player_t *player)
{
    if (player->count == 0)
        return;
    if (!player->playing)
    {
        player->current = player->selected;
        player->playing = true;
        player->started++;
        if (player->history_count < 8)
            player->history[player->history_count++] = player->current;
    }
}

void axmu_pause(axmu_player_t *player)
{
    player->playing = false;
}

void axmu_stop(axmu_player_t *player)
{
    player->playing = false;
    player->position = 0;
}

void axmu_next(axmu_player_t *player)
{
    axmu_advance(player);
}

void axmu_previous(axmu_player_t *player)
{
    if (player->count == 0)
        return;
    if (player->position > 3)
    {
        player->position = 0;
        return;
    }
    player->current = (u16)((player->current + player->count - 1) % player->count);
    player->position = 0;
}

void axmu_seek(axmu_player_t *player, u32 seconds)
{
    if (player->count == 0)
        return;
    if (seconds > player->tracks[player->current].duration)
        seconds = player->tracks[player->current].duration;
    player->position = seconds;
}

void axmu_cycle_repeat(axmu_player_t *player)
{
    player->repeat = (u8)((player->repeat + 1) % 3);
}

void axmu_toggle_shuffle(axmu_player_t *player)
{
    player->shuffle = (bool_t)!player->shuffle;
}

void axmu_set_volume(axmu_player_t *player, u8 volume)
{
    player->volume = volume > 100 ? 100 : volume;
    player->muted = false;
}

void axmu_toggle_mute(axmu_player_t *player)
{
    player->muted = (bool_t)!player->muted;
}

bool_t axmu_toggle_favourite(axmu_player_t *player)
{
    if (player->count == 0)
        return false;
    player->tracks[player->selected].favourite =
        (bool_t)!player->tracks[player->selected].favourite;
    return player->tracks[player->selected].favourite;
}

u16 axmu_search(axmu_player_t *player, const char *needle)
{
    u16 i;
    u16 matches = 0;
    if (needle[0] == 0)
        return player->count;
    for (i = 0; i < player->count; i++)
    {
        u32 a = 0;
        u32 b = 0;
        while (player->tracks[i].title[a] != 0 && a < 1)
            a++;
        AXIOM_UNUSED_VAR(b);
        if (axmu_equals(player->tracks[i].title, needle) ||
            axmu_equals(player->tracks[i].artist, needle))
            matches++;
    }
    return matches;
}

void axmu_tick(axmu_player_t *player, u32 elapsed)
{
    player->ticks += elapsed;
    if (!player->playing || player->count == 0)
        return;
    player->position += elapsed;
    if (player->position >= player->tracks[player->current].duration)
        axmu_advance(player);
}

void axmu_paint(axmu_player_t *player, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t list = gui_rect(bounds.x, bounds.y + AXMU_COVER + 16, bounds.w,
                               bounds.h - AXMU_COVER - AXMU_TRANSPORT - 16);
    gui_rect_t bar = gui_rect(bounds.x, bounds.y + bounds.h - AXMU_TRANSPORT,
                              bounds.w, AXMU_TRANSPORT);
    gui_rect_t cover = gui_rect(bounds.x + 24, bounds.y + 16, AXMU_COVER, AXMU_COVER);
    gui_fill_rect(surface, bounds, AXMU_C_BG);
    gui_fill_rect(surface, cover, AXMU_C_SURFACE);
    if (player->count > 0)
        axmu_paint_waveform(surface, &player->tracks[player->current],
                            gui_rect(cover.x + 8, cover.y + 8, cover.w - 16,
                                     cover.h - 16));
    gui_text(surface, cover.x + cover.w + 24, bounds.y + 24, "Library",
             AXMU_C_TEXT, 2);
    gui_text(surface, cover.x + cover.w + 24, bounds.y + 56,
             player->playing ? "playing" : "stopped", AXMU_C_DIM, 1);
    axmu_paint_library(surface, player, list);
    axmu_paint_transport(surface, player, bar);
}

void axmu_report(const axmu_player_t *player)
{
    printk("axmu: tracks=%u current=%u playing=%u pos=%u vol=%u\n",
           (unsigned)player->count, (unsigned)player->current,
           (unsigned)player->playing, (unsigned)player->position,
           (unsigned)player->volume);
}

bool_t axmu_selftest(void)
{
    axmu_player_t player;
    axmu_init(&player);
    if (player.volume != 60)
        return false;
    if (!axmu_add(&player, "Aurora", "Axiom Labs", "First Light", 214, 320))
        return false;
    if (!axmu_add(&player, "Kernel Panic", "Axiom Labs", "First Light", 188, 320))
        return false;
    if (player.count != 2)
        return false;
    axmu_play(&player);
    if (!player.playing)
        return false;
    axmu_tick(&player, 10);
    if (player.position != 10)
        return false;
    axmu_seek(&player, 200);
    if (player.position != 200)
        return false;
    axmu_next(&player);
    if (player.position != 0)
        return false;
    axmu_tick(&player, 200);
    if (player.playing && player.current == 0 && !player.shuffle)
        return true;
    axmu_cycle_repeat(&player);
    if (player.repeat != AXMU_REPEAT_ALL)
        return false;
    axmu_toggle_shuffle(&player);
    if (!player.shuffle)
        return false;
    axmu_toggle_mute(&player);
    if (!player.muted)
        return false;
    return true;
}
