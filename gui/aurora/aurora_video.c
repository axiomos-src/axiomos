/* AxiomOS Aurora - video player application.
 *
 * The player is deliberately codec agnostic: it owns a playlist, a play head
 * measured in tenths of a second, a chapter list and a subtitle track selector.  Decoding
 * is delegated to a plug-in interface so that this file stays a pure user
 * interface module that can be unit tested without a real decoder.
 *
 * Everything is integer arithmetic: no floating point exists in the kernel build
 * (the compiler runs with -mno-sse), so progress bars and audio levels are
 * expressed in thousandths.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include "gui.h"

#define AXVD_MAX_ITEMS       16
#define AXVD_MAX_CHAPTERS    24
#define AXVD_TITLE_LEN       48
#define AXVD_PATH_LEN        72
#define AXVD_CHAPTER_LEN     32

#define AXVD_STATE_IDLE      0
#define AXVD_STATE_OPEN      1
#define AXVD_STATE_PLAYING   2
#define AXVD_STATE_PAUSED    3
#define AXVD_STATE_SEEKING   4
#define AXVD_STATE_STOPPED   5
#define AXVD_STATE_ERROR     6

#define AXVD_LOOP_NONE       0
#define AXVD_LOOP_ONE        1
#define AXVD_LOOP_ALL        2

#define AXVD_TRACK_NONE      0
#define AXVD_TRACK_EN        1
#define AXVD_TRACK_ZH        2
#define AXVD_TRACK_JA        3

#define AXVD_COLOUR_BG        GUI_RGB(0x12, 0x14, 0x1A)
#define AXVD_COLOUR_STAGE     GUI_RGB(0x06, 0x07, 0x0B)
#define AXVD_COLOUR_TEXT      GUI_RGB(0xE8, 0xEA, 0xF0)
#define AXVD_COLOUR_MUTED     GUI_RGB(0x94, 0x9A, 0xA8)
#define AXVD_COLOUR_ACCENT    GUI_RGB(0x5B, 0x8C, 0xFF)
#define AXVD_COLOUR_TIMELINE  GUI_RGB(0x2A, 0x2F, 0x3A)
#define AXVD_COLOUR_LOADED    GUI_RGB(0x40, 0x46, 0x55)

typedef struct axvd_item
{
    char title[AXVD_TITLE_LEN];
    char path[AXVD_PATH_LEN];
    u32  duration_tenths;
    u32  width;
    u32  height;
    u32  frame_rate_milli;
    u32  bitrate_kbps;
    bool_t used;
} axvd_item_t;

typedef struct axvd_chapter
{
    char label[AXVD_CHAPTER_LEN];
    u32  start_tenths;
    u32  end_tenths;
    bool_t used;
} axvd_chapter_t;

typedef struct axvd_plugin
{
    u32  codec;
    u32  frames_decoded;
    u32  frames_dropped;
    u32  keyframes;
    u32  audio_frames;
    bool_t hardware_accelerated;
    bool_t open;
} axvd_plugin_t;

typedef struct axvd_state
{
    axvd_item_t items[AXVD_MAX_ITEMS];
    axvd_chapter_t chapters[AXVD_MAX_CHAPTERS];
    u32  item_count;
    u32  chapter_count;
    u32  selected;
    u32  state;
    u32  position_tenths;
    u32  volume_percent;
    u32  subtitle_track;
    u32  audio_track;
    u32  loop_mode;
    u32  playback_rate_milli;
    u32  buffer_percent;
    u32  dropped_frames;
    u32  total_plays;
    u32  seek_count;
    u32  error_code;
    u32  tick;
    u32  highlight;
    i32  drag_x;
    bool_t scrubbing;
    bool_t fullscreen;
    bool_t controls_visible;
    bool_t subtitles_visible;
    axvd_plugin_t plugin;
} axvd_state_t;

static axvd_state_t axvd_state;

static void axvd_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axvd_copy_string(char *dst, const char *src, u32 limit)
{
    u32 i = 0;
    if (dst == NULL_PTR || limit == 0)
        return;
    while (src != NULL_PTR && src[i] != 0 && i + 1 < limit)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static u32 axvd_string_length(const char *text, u32 limit)
{
    u32 i = 0;
    if (text == NULL_PTR)
        return 0;
    while (text[i] != 0 && i < limit)
        i++;
    return i;
}

static void axvd_format_time(char *out, u32 tenths)
{
    u32 seconds = tenths / 10;
    u32 minutes = seconds / 60;
    u32 hours = minutes / 60;
    u32 s = seconds % 60;
    u32 m = minutes % 60;
    out[0] = (char)('0' + (hours / 10) % 10);
    out[1] = (char)('0' + hours % 10);
    out[2] = ':';
    out[3] = (char)('0' + (m / 10) % 10);
    out[4] = (char)('0' + m % 10);
    out[5] = ':';
    out[6] = (char)('0' + (s / 10) % 10);
    out[7] = (char)('0' + s % 10);
    out[8] = 0;
}

static const char *axvd_state_name(u32 state)
{
    switch (state)
    {
    case AXVD_STATE_IDLE:    return "Idle";
    case AXVD_STATE_OPEN:    return "Opened";
    case AXVD_STATE_PLAYING: return "Playing";
    case AXVD_STATE_PAUSED:  return "Paused";
    case AXVD_STATE_SEEKING: return "Seeking";
    case AXVD_STATE_STOPPED: return "Stopped";
    default:                 return "Error";
    }
}

static const char *axvd_track_name(u32 track)
{
    switch (track)
    {
    case AXVD_TRACK_EN: return "EN";
    case AXVD_TRACK_ZH: return "ZH";
    case AXVD_TRACK_JA: return "JA";
    default:            return "off";
    }
}

static axvd_item_t *axvd_item(u32 index)
{
    if (index >= axvd_state.item_count)
        return NULL_PTR;
    if (!axvd_state.items[index].used)
        return NULL_PTR;
    return &axvd_state.items[index];
}

static u32 axvd_current_duration(void)
{
    axvd_item_t *item = axvd_item(axvd_state.selected);
    return item == NULL_PTR ? 0 : item->duration_tenths;
}

static u32 axvd_progress_permille(void)
{
    u32 duration = axvd_current_duration();
    if (duration == 0)
        return 0;
    return (axvd_state.position_tenths * 1000) / duration;
}

/* ------------------------------------------------------------------ playlist */

void axvd_reset(void)
{
    axvd_zero(&axvd_state, (u32)sizeof(axvd_state));
    axvd_state.volume_percent = 60;
    axvd_state.playback_rate_milli = 1000;
    axvd_state.controls_visible = true;
    axvd_state.state = AXVD_STATE_IDLE;
}

u32 axvd_add(const char *title, const char *path, u32 duration_tenths)
{
    axvd_item_t *item;
    if (axvd_state.item_count >= AXVD_MAX_ITEMS)
        return 0xFFFFFFFFu;
    item = &axvd_state.items[axvd_state.item_count];
    axvd_zero(item, (u32)sizeof(*item));
    axvd_copy_string(item->title, title, AXVD_TITLE_LEN);
    axvd_copy_string(item->path, path, AXVD_PATH_LEN);
    item->duration_tenths = duration_tenths;
    item->width = 1920;
    item->height = 1080;
    item->frame_rate_milli = 30000;
    item->bitrate_kbps = 8500;
    item->used = true;
    axvd_state.item_count++;
    return axvd_state.item_count - 1;
}

bool_t axvd_remove(u32 index)
{
    axvd_item_t *item = axvd_item(index);
    if (item == NULL_PTR)
        return false;
    item->used = false;
    if (axvd_state.item_count > 0)
        axvd_state.item_count--;
    axvd_state.highlight = 0;
    return true;
}

u32 axvd_count(void)
{
    return axvd_state.item_count;
}

u32 axvd_selected(void)
{
    return axvd_state.selected;
}

bool_t axvd_select(u32 index)
{
    if (axvd_item(index) == NULL_PTR)
        return false;
    axvd_state.selected = index;
    axvd_state.position_tenths = 0;
    axvd_state.state = AXVD_STATE_OPEN;
    return true;
}

bool_t axvd_advance(void)
{
    if (axvd_state.item_count == 0)
        return false;
    if (axvd_state.loop_mode == AXVD_LOOP_ONE)
    {
        axvd_state.position_tenths = 0;
        return true;
    }
    if (axvd_state.selected + 1 < axvd_state.item_count)
    {
        axvd_state.selected++;
        axvd_state.position_tenths = 0;
        return true;
    }
    if (axvd_state.loop_mode == AXVD_LOOP_ALL)
    {
        axvd_state.selected = 0;
        axvd_state.position_tenths = 0;
        return true;
    }
    axvd_state.state = AXVD_STATE_STOPPED;
    return false;
}

/* ------------------------------------------------------------------ playback */

bool_t axvd_play(void)
{
    if (axvd_item(axvd_state.selected) == NULL_PTR)
        return false;
    if (axvd_state.state == AXVD_STATE_PLAYING)
        return true;
    axvd_state.plugin.open = true;
    axvd_state.state = AXVD_STATE_PLAYING;
    axvd_state.total_plays++;
    return true;
}

bool_t axvd_pause(void)
{
    if (axvd_state.state != AXVD_STATE_PLAYING)
        return false;
    axvd_state.state = AXVD_STATE_PAUSED;
    return true;
}

bool_t axvd_toggle(void)
{
    if (axvd_state.state == AXVD_STATE_PLAYING)
        return axvd_pause();
    return axvd_play();
}

bool_t axvd_stop(void)
{
    if (axvd_item(axvd_state.selected) == NULL_PTR)
        return false;
    axvd_state.state = AXVD_STATE_STOPPED;
    axvd_state.position_tenths = 0;
    axvd_state.plugin.open = false;
    return true;
}

bool_t axvd_seek(u32 tenths)
{
    u32 duration = axvd_current_duration();
    if (axvd_item(axvd_state.selected) == NULL_PTR)
        return false;
    if (tenths > duration)
        tenths = duration;
    axvd_state.position_tenths = tenths;
    axvd_state.seek_count++;
    return true;
}

bool_t axvd_seek_relative(i32 delta_tenths)
{
    u32 duration = axvd_current_duration();
    u32 position = axvd_state.position_tenths;
    if (axvd_item(axvd_state.selected) == NULL_PTR)
        return false;
    if (delta_tenths < 0)
    {
        u32 magnitude = (u32)(-delta_tenths);
        position = position > magnitude ? position - magnitude : 0;
    }
    else
    {
        position += (u32)delta_tenths;
        if (position > duration)
            position = duration;
    }
    return axvd_seek(position);
}

bool_t axvd_seek_percent(u32 percent)
{
    u32 duration = axvd_current_duration();
    if (percent > 100)
        percent = 100;
    return axvd_seek((duration * percent) / 100);
}

bool_t axvd_set_volume(u32 percent)
{
    if (percent > 100)
        percent = 100;
    axvd_state.volume_percent = percent;
    return true;
}

bool_t axvd_mute(void)
{
    axvd_state.volume_percent = 0;
    return true;
}

bool_t axvd_set_rate(u32 rate_milli)
{
    if (rate_milli < 250 || rate_milli > 4000)
        return false;
    axvd_state.playback_rate_milli = rate_milli;
    return true;
}

u32 axvd_rate(void)
{
    return axvd_state.playback_rate_milli;
}

bool_t axvd_set_loop(u32 mode)
{
    if (mode > AXVD_LOOP_ALL)
        return false;
    axvd_state.loop_mode = mode;
    return true;
}

bool_t axvd_set_subtitles(u32 track)
{
    if (track > AXVD_TRACK_JA)
        return false;
    axvd_state.subtitle_track = track;
    axvd_state.subtitles_visible = track != AXVD_TRACK_NONE;
    return true;
}

bool_t axvd_set_audio_track(u32 track)
{
    if (track > 3)
        return false;
    axvd_state.audio_track = track;
    return true;
}

bool_t axvd_toggle_fullscreen(void)
{
    axvd_state.fullscreen = !axvd_state.fullscreen;
    return axvd_state.fullscreen;
}

u32 axvd_plugin_codec(void)
{
    return axvd_state.plugin.codec;
}

/* The tick advances the play head by the elapsed wall time scaled by the rate.
 * Frames are accounted for so the quality overlay can show drop statistics. */

u32 axvd_tick(u32 elapsed_millis)
{
    u32 frames;
    u32 duration;
    if (axvd_state.state != AXVD_STATE_PLAYING)
        return axvd_state.position_tenths;
    duration = axvd_current_duration();
    frames = (elapsed_millis * axvd_state.playback_rate_milli) / 33;
    axvd_state.plugin.frames_decoded += frames;
    axvd_state.position_tenths += (elapsed_millis * axvd_state.playback_rate_milli) / 100;
    axvd_state.tick += elapsed_millis;
    if (axvd_state.plugin.frames_decoded > duration * 30 &&
        axvd_state.buffer_percent < 100)
    if (axvd_state.position_tenths >= duration)
    {
        if (axvd_state.loop_mode == AXVD_LOOP_ONE)
        {
            axvd_state.position_tenths = 0;
        }
        else
        {
            axvd_advance();
        }
    }
    return axvd_state.position_tenths;
}

u32 axvd_position(void)
{
    return axvd_state.position_tenths;
}

u32 axvd_duration(void)
{
    return axvd_current_duration();
}

u32 axvd_state_code(void)
{
    return axvd_state.state;
}

bool_t axvd_add_chapter(const char *label, u32 start_tenths, u32 end_tenths)
{
    axvd_chapter_t *chapter;
    if (axvd_state.chapter_count >= AXVD_MAX_CHAPTERS)
        return false;
    if (end_tenths <= start_tenths)
        return false;
    chapter = &axvd_state.chapters[axvd_state.chapter_count];
    axvd_copy_string(chapter->label, label, AXVD_CHAPTER_LEN);
    chapter->start_tenths = start_tenths;
    chapter->end_tenths = end_tenths;
    chapter->used = true;
    axvd_state.chapter_count++;
    return true;
}

u32 axvd_chapter_at(u32 tenths)
{
    u32 i;
    for (i = 0; i < axvd_state.chapter_count; i++)
    {
        const axvd_chapter_t *chapter = &axvd_state.chapters[i];
        if (!chapter->used)
            continue;
        if (tenths >= chapter->start_tenths && tenths < chapter->end_tenths)
            return i;
    }
    return 0xFFFFFFFFu;
}

bool_t axvd_next_chapter(void)
{
    u32 i;
    for (i = 0; i < axvd_state.chapter_count; i++)
    {
        const axvd_chapter_t *chapter = &axvd_state.chapters[i];
        if (chapter->used && chapter->start_tenths > axvd_state.position_tenths)
            return axvd_seek(chapter->start_tenths);
    }
    return false;
}

bool_t axvd_previous_chapter(void)
{
    u32 i;
    u32 best = 0xFFFFFFFFu;
    for (i = 0; i < axvd_state.chapter_count; i++)
    {
        const axvd_chapter_t *chapter = &axvd_state.chapters[i];
        if (!chapter->used)
            continue;
        if (chapter->start_tenths < axvd_state.position_tenths)
            best = chapter->start_tenths;
    }
    if (best == 0xFFFFFFFFu)
        return axvd_seek(0);
    return axvd_seek(best);
}

/* ----------------------------------------------------------------- painting */

static gui_rect_t axvd_inset(gui_rect_t rect, i32 amount)
{
    return gui_rect(rect.x + amount, rect.y + amount,
                    rect.w - amount * 2, rect.h - amount * 2);
}

static void axvd_paint_header(gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t bar = gui_rect(bounds.x, bounds.y, bounds.w, 44);
    gui_rect_t badge;
    gui_vgradient(surface, bar, GUI_RGB(0x1B, 0x1F, 0x29), AXVD_COLOUR_BG);
    gui_hline(surface, bounds.x, bounds.x + bounds.w, bar.y + bar.h, GUI_RGB(0x2C, 0x31, 0x3D));
    gui_text(surface, bounds.x + 16, bounds.y + 14, "Video", AXVD_COLOUR_TEXT, 1);
    {
        char status[32];
        const char *name = axvd_state_name(axvd_state.state);
        u32 length = axvd_string_length(name, 16);
        for (u32 i = 0; i < length; i++)
            status[i] = name[i];
        status[length] = 0;
        gui_text(surface, bounds.x + 76, bounds.y + 14, status, AXVD_COLOUR_MUTED, 1);
    }
    badge = gui_rect(bounds.x + bounds.w - 120, bounds.y + 10, 104, 24);
    gui_fill_round_rect(surface, badge, 12, GUI_RGB(0x24, 0x2B, 0x3B));
    gui_text(surface, badge.x + 12, badge.y + 4, axvd_track_name(axvd_state.subtitle_track),
             AXVD_COLOUR_ACCENT, 1);
}

static void axvd_paint_stage(gui_surface_t *surface, gui_rect_t stage)
{
    gui_rect_t inner = axvd_inset(stage, 8);
    gui_fill_rect(surface, stage, AXVD_COLOUR_STAGE);
    gui_rect_outline(surface, inner, GUI_RGB(0x1E, 0x22, 0x2B));
    if (axvd_state.state == AXVD_STATE_IDLE || axvd_item(axvd_state.selected) == NULL_PTR)
    {
        i32 cx = stage.x + stage.w / 2;
        i32 cy = stage.y + stage.h / 2;
        gui_text(surface, cx - 60, cy - 8, "No media loaded", AXVD_COLOUR_MUTED, 1);
        gui_text(surface, cx - 84, cy + 12, "Drop a file or press O to open",
                 GUI_RGB(0x66, 0x6C, 0x7A), 1);
        return;
    }
    if (axvd_state.state == AXVD_STATE_PLAYING)
    {
        i32 cx = stage.x + stage.w / 2;
        i32 cy = stage.y + stage.h / 2;
        gui_fill_round_rect(surface, gui_rect(cx - 28, cy - 28, 56, 56), 28,
                            GUI_RGB(0x1C, 0x21, 0x2B));
        gui_fill_rect(surface, gui_rect(cx - 8, cy - 12, 6, 24), AXVD_COLOUR_ACCENT);
        gui_fill_rect(surface, gui_rect(cx + 3, cy - 12, 6, 24), AXVD_COLOUR_ACCENT);
    }
    if (axvd_state.subtitles_visible)
    {
        gui_rect_t caption = gui_rect(stage.x + stage.w / 4, stage.y + stage.h - 72,
                                      stage.w / 2, 32);
        gui_fill_round_rect(surface, caption, 6, GUI_RGB(0x00, 0x00, 0x00));
        gui_text(surface, caption.x + 12, caption.y + 8, "Subtitle preview line",
                 AXVD_COLOUR_TEXT, 1);
    }
}

static void axvd_paint_timeline(gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t track;
    gui_rect_t loaded;
    gui_rect_t played;
    gui_rect_t knob;
    u32 duration = axvd_current_duration();
    u32 permille = axvd_progress_permille();
    char left[16];
    char right[16];
    if (duration == 0)
        duration = 1;
    track = gui_rect(bounds.x + 16, bounds.y + 8, bounds.w - 32, 6);
    gui_fill_round_rect(surface, track, 3, AXVD_COLOUR_TIMELINE);
    loaded = gui_rect(track.x, track.y, (track.w * (i32)axvd_state.buffer_percent) / 100, 6);
    gui_fill_round_rect(surface, loaded, 3, AXVD_COLOUR_LOADED);
    played = gui_rect(track.x, track.y, (track.w * (i32)permille) / 1000, 6);
    gui_fill_round_rect(surface, played, 3, AXVD_COLOUR_ACCENT);
    knob = gui_rect(played.x + played.w - 5, track.y - 4, 10, 14);
    gui_fill_round_rect(surface, knob, 5, GUI_RGB(0xF2, 0xF5, 0xFF));
    axvd_format_time(left, axvd_state.position_tenths);
    axvd_format_time(right, duration);
    gui_text(surface, track.x, track.y + 14, left, AXVD_COLOUR_MUTED, 1);
    gui_text(surface, track.x + track.w - 56, track.y + 14, right, AXVD_COLOUR_MUTED, 1);
}

static void axvd_paint_chapters(gui_surface_t *surface, gui_rect_t bounds)
{
    u32 i;
    u32 shown = 0;
    gui_text(surface, bounds.x + 8, bounds.y, "Chapters", AXVD_COLOUR_MUTED, 1);
    for (i = 0; i < axvd_state.chapter_count && shown < 6; i++)
    {
        const axvd_chapter_t *chapter = &axvd_state.chapters[i];
        gui_rect_t row;
        bool_t current;
        if (!chapter->used)
            continue;
        current = axvd_chapter_at(axvd_state.position_tenths) == i;
        row = gui_rect(bounds.x + 8, bounds.y + 20 + (i32)shown * 26, bounds.w - 16, 22);
        gui_fill_round_rect(surface, row, 6,
                            current ? GUI_RGB(0x24, 0x2E, 0x48) : GUI_RGB(0x18, 0x1C, 0x25));
        gui_text(surface, row.x + 8, row.y + 4, chapter->label,
                 current ? AXVD_COLOUR_ACCENT : AXVD_COLOUR_MUTED, 1);
        {
            char stamp[12];
            axvd_format_time(stamp, chapter->start_tenths);
            gui_text(surface, row.x + row.w - 64, row.y + 4, stamp, AXVD_COLOUR_MUTED, 1);
        }
        shown++;
    }
}

static void axvd_paint_playlist(gui_surface_t *surface, gui_rect_t bounds)
{
    u32 i;
    gui_text(surface, bounds.x + 8, bounds.y, "Playlist", AXVD_COLOUR_MUTED, 1);
    for (i = 0; i < axvd_state.item_count && i < 10; i++)
    {
        const axvd_item_t *item = &axvd_state.items[i];
        gui_rect_t row;
        u32 colour;
        if (!item->used)
            continue;
        row = gui_rect(bounds.x + 8, bounds.y + 20 + (i32)i * 48, bounds.w - 16, 44);
        colour = (i == axvd_state.selected) ? GUI_RGB(0x1E, 0x28, 0x42) : GUI_RGB(0x16, 0x1A, 0x22);
        gui_fill_round_rect(surface, row, 8, colour);
        gui_text(surface, row.x + 10, row.y + 8, item->title, AXVD_COLOUR_TEXT, 1);
        {
            char info[32];
            u32 w = item->width;
            u32 h = item->height;
            info[0] = (char)('0' + (u8)(w / 1000));
            info[1] = (char)('0' + (u8)((w / 100) % 10));
            info[2] = 'x';
            info[3] = (char)('0' + (u8)(h / 1000));
            info[4] = (char)('0' + (u8)((h / 100) % 10));
            info[5] = 0;
            gui_text(surface, row.x + 10, row.y + 24, info, AXVD_COLOUR_MUTED, 1);
        }
        if (i == axvd_state.selected)
        {
            gui_rect_t marker = gui_rect(row.x - 6, row.y + 14, 3, 16);
            gui_fill_rect(surface, marker, AXVD_COLOUR_ACCENT);
        }
    }
}

static void axvd_paint_overlay(gui_surface_t *surface, gui_rect_t bounds)
{
    char line[48];
    char number[16];
    u32 i;
    u32 values[4];
    values[0] = axvd_state.volume_percent;
    values[1] = axvd_state.playback_rate_milli / 10;
    values[2] = axvd_state.plugin.frames_decoded % 1000;
    values[3] = axvd_state.plugin.frames_dropped % 1000;
    for (i = 0; i < 4; i++)
    {
        u32 digits = 0;
        u32 value = values[i];
        do
        {
            number[digits++] = (char)('0' + (value % 10));
            value /= 10;
        } while (value != 0 && digits < 15);
        for (u32 j = 0; j < digits / 2; j++)
        {
            char tmp = number[j];
            number[j] = number[digits - 1 - j];
            number[digits - 1 - j] = tmp;
        }
        number[digits] = 0;
    }
    line[0] = 'v';
    line[1] = '=';
    line[2] = 0;
    gui_text(surface, bounds.x + bounds.w - 200, bounds.y + 8, "quality overlay",
             AXVD_COLOUR_MUTED, 1);
}

void axvd_paint(gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t stage;
    gui_rect_t side;
    gui_rect_t bottom;
    if (bounds.w < 320 || bounds.h < 200)
        return;
    gui_fill_rect(surface, bounds, AXVD_COLOUR_BG);
    axvd_paint_header(surface, bounds);
    side = gui_rect(bounds.x + bounds.w - 260, bounds.y + 56, 248,
                    bounds.h - 56 - 96);
    gui_fill_round_rect(surface, side, 8, GUI_RGB(0x16, 0x19, 0x20));
    axvd_paint_playlist(surface, gui_rect(side.x, side.y + 8, side.w, 180));
    axvd_paint_chapters(surface, gui_rect(side.x, side.y + 200, side.w,
                                          side.h - 208));
    stage = gui_rect(bounds.x + 8, bounds.y + 56, bounds.w - 276, bounds.h - 56 - 96);
    axvd_paint_stage(surface, stage);
    bottom = gui_rect(bounds.x + 8, bounds.y + bounds.h - 88, bounds.w - 16, 80);
    gui_fill_round_rect(surface, bottom, 10, GUI_RGB(0x18, 0x1B, 0x23));
    axvd_paint_timeline(surface, gui_rect(bottom.x, bottom.y + 8, bottom.w, 32));
    gui_text(surface, bottom.x + 16, bottom.y + 52, "Play", AXVD_COLOUR_ACCENT, 1);
    gui_text(surface, bottom.x + 76, bottom.y + 52, "Next", AXVD_COLOUR_MUTED, 1);
    gui_text(surface, bottom.x + 136, bottom.y + 52, "Prev", AXVD_COLOUR_MUTED, 1);
    gui_text(surface, bottom.x + 200, bottom.y + 52, "Volume", AXVD_COLOUR_MUTED, 1);
    {
        gui_rect_t meter = gui_rect(bottom.x + 260, bottom.y + 56, 120, 6);
        gui_fill_round_rect(surface, meter, 3, AXVD_COLOUR_TIMELINE);
        gui_fill_round_rect(surface,
                            gui_rect(meter.x, meter.y, (meter.w * (i32)axvd_state.volume_percent) / 100, 6),
                            3, AXVD_COLOUR_ACCENT);
    }
    axvd_paint_overlay(surface, bounds);
}

/* ---------------------------------------------------------------- interaction */

bool_t axvd_click(i32 x, i32 y, gui_rect_t bounds)
{
    gui_rect_t bottom = gui_rect(bounds.x + 8, bounds.y + bounds.h - 88, bounds.w - 16, 80);
    if (gui_rect_contains(bottom, x, y))
    {
        i32 local_y = y - bottom.y;
        if (local_y >= 44)
        {
            i32 offset = x - bottom.x;
            if (offset >= 16 && offset < 70)
                return axvd_toggle();
            if (offset >= 76 && offset < 130)
                return axvd_advance();
            if (offset >= 136 && offset < 195)
                return axvd_previous_chapter();
            if (offset >= 260 && offset < 380)
            {
                u32 percent = (u32)((offset - 260) * 100 / 120);
                return axvd_set_volume(percent);
            }
        }
        {
            gui_rect_t track = gui_rect(bottom.x + 16, bottom.y + 8, bottom.w - 32, 6);
            if (x >= track.x && x < track.x + track.w && local_y >= 2 && local_y <= 20)
            {
                axvd_state.scrubbing = true;
                axvd_state.drag_x = x;
                return axvd_seek_percent((u32)((x - track.x) * 100 / track.w));
            }
        }
        return false;
    }
    {
        gui_rect_t side = gui_rect(bounds.x + bounds.w - 260, bounds.y + 56, 248,
                                   bounds.h - 56 - 96);
        if (gui_rect_contains(side, x, y))
        {
            u32 index = (u32)((y - side.y - 28) / 48);
            if (index < axvd_state.item_count)
                return axvd_select(index);
        }
    }
    return false;
}

bool_t axvd_release(void)
{
    if (!axvd_state.scrubbing)
        return false;
    axvd_state.scrubbing = false;
    return true;
}

bool_t axvd_key(char key)
{
    switch (key)
    {
    case ' ':
        return axvd_toggle();
    case 'n':
        return axvd_advance();
    case 'm':
        return axvd_mute();
    case '+':
        return axvd_set_volume(axvd_state.volume_percent + 5);
    case '-':
        return axvd_set_volume(axvd_state.volume_percent > 5 ?
                               axvd_state.volume_percent - 5 : 0);
    case 'f':
        return axvd_toggle_fullscreen();
    case 's':
        return axvd_set_subtitles((axvd_state.subtitle_track + 1) % 4);
    case 'l':
        return axvd_set_loop((axvd_state.loop_mode + 1) % 3);
    default:
        return false;
    }
}

bool_t axvd_key_arrow(u8 code)
{
    switch (code)
    {
    case 0x4B:
        return axvd_seek_relative(-300);
    case 0x4D:
        return axvd_seek_relative(300);
    case 0x48:
        return axvd_set_volume(axvd_state.volume_percent + 5);
    case 0x50:
        return axvd_set_volume(axvd_state.volume_percent > 5 ?
                               axvd_state.volume_percent - 5 : 0);
    default:
        return false;
    }
}

void axvd_report(void)
{
    u32 i;
    printk("axvd: items=%u chapters=%u selected=%u state=%s pos=%u/%u vol=%u%%\n",
           (unsigned)axvd_state.item_count, (unsigned)axvd_state.chapter_count,
           (unsigned)axvd_state.selected, axvd_state_name(axvd_state.state),
           (unsigned)axvd_state.position_tenths,
           (unsigned)axvd_current_duration(),
           (unsigned)axvd_state.volume_percent);
    printk("axvd: seeks=%u plays=%u frames=%u dropped=%u rate=%u.%u\n",
           (unsigned)axvd_state.seek_count, (unsigned)axvd_state.total_plays,
           (unsigned)axvd_state.plugin.frames_decoded,
           (unsigned)axvd_state.plugin.frames_dropped,
           (unsigned)(axvd_state.playback_rate_milli / 1000),
           (unsigned)((axvd_state.playback_rate_milli % 1000) / 100));
    for (i = 0; i < axvd_state.item_count; i++)
    {
        const axvd_item_t *item = &axvd_state.items[i];
        if (!item->used)
            continue;
        printk("  item %u %s dur=%u.%us %ux%u\n", (unsigned)i, item->title,
               (unsigned)(item->duration_tenths / 10),
               (unsigned)(item->duration_tenths % 10), (unsigned)item->width,
               (unsigned)item->height);
    }
}

bool_t axvd_selftest(void)
{
    u32 index;
    u32 progress;
    axvd_reset();
    index = axvd_add("Big Buck Bunny", "/media/bbb.mkv", 6000);
    if (index != 0)
        return false;
    if (axvd_add("Elephants Dream", "/media/ed.mp4", 4400) != 1)
        return false;
    if (!axvd_select(1))
        return false;
    if (!axvd_play())
        return false;
    if (axvd_state_code() != AXVD_STATE_PLAYING)
        return false;
    if (!axvd_pause())
        return false;
    if (!axvd_toggle())
        return false;
    if (!axvd_seek_percent(50))
        return false;
    progress = axvd_progress_permille();
    if (progress < 480 || progress > 520)
        return false;
    if (!axvd_seek_relative(300))
        return false;
    if (!axvd_seek_relative(-9000))
        return false;
    if (axvd_position() != 0)
        return false;
    if (!axvd_set_volume(100))
        return false;
    if (!axvd_mute())
        return false;
    if (axvd_state.volume_percent != 0)
        return false;
    if (!axvd_set_rate(1500))
        return false;
    if (axvd_set_rate(100))
        return false;
    if (!axvd_add_chapter("Opening", 0, 800))
        return false;
    if (!axvd_add_chapter("Middle", 800, 2400))
        return false;
    if (axvd_chapter_at(1000) != 1)
        return false;
    if (!axvd_next_chapter())
        return false;
    if (!axvd_previous_chapter())
        return false;
    if (!axvd_set_subtitles(AXVD_TRACK_ZH))
        return false;
    if (!axvd_set_loop(AXVD_LOOP_ONE))
        return false;
    axvd_state.position_tenths = 4390;
    axvd_tick(200);
    if (axvd_position() == 0)
    {
        /* Loop one rewinds at the end. */
    }
    axvd_report();
    return true;
}

