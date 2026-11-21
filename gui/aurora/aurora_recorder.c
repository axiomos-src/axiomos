/* AxiomOS Aurora - voice recorder and transcription browser.
 *
 * The recorder keeps a list of takes with duration, sample rate and channel
 * count, a live input level meter and a coarse transcript made of timestamped
 * segments.  Levels are computed from peak values driven by the audio driver;
 * the UI only ever reads the cached numbers, never the PCM buffer.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include "gui.h"

#define AXRC_MAX_TAKES       12
#define AXRC_MAX_SEGMENTS    16
#define AXRC_TITLE_LEN       36
#define AXRC_SEGMENT_LEN     72
#define AXRC_STAMP_LEN       12

#define AXRC_STATE_IDLE      0
#define AXRC_STATE_RECORDING 1
#define AXRC_STATE_PAUSED    2
#define AXRC_STATE_PLAYING   3
#define AXRC_STATE_SAVING    4

#define AXRC_SOURCE_MIC      0
#define AXRC_SOURCE_LINE     1
#define AXRC_SOURCE_MIX      2

#define AXRC_COLOUR_BG       GUI_RGB(0x13, 0x15, 0x1A)
#define AXRC_COLOUR_PANEL    GUI_RGB(0x1C, 0x20, 0x28)
#define AXRC_COLOUR_TEXT     GUI_RGB(0xE7, 0xE9, 0xEF)
#define AXRC_COLOUR_MUTED    GUI_RGB(0x8D, 0x94, 0xA3)
#define AXRC_COLOUR_ACCENT   GUI_RGB(0xF2, 0x6B, 0x6B)
#define AXRC_COLOUR_WAVE     GUI_RGB(0x57, 0x86, 0xE8)
#define AXRC_COLOUR_GRID     GUI_RGB(0x24, 0x29, 0x33)

typedef struct axrc_segment
{
    char text[AXRC_SEGMENT_LEN];
    u32  start_millis;
    u32  end_millis;
    u8   speaker;
    bool_t used;
} axrc_segment_t;

typedef struct axrc_take
{
    char title[AXRC_TITLE_LEN];
    u32  duration_millis;
    u32  sample_rate;
    u32  channels;
    u32  bytes;
    u32  segments;
    u16  peak_permille;
    u8   source;
    bool_t used;
    bool_t noise_suppressed;
    axrc_segment_t segment[AXRC_MAX_SEGMENTS];
} axrc_take_t;

typedef struct axrc_state
{
    axrc_take_t takes[AXRC_MAX_TAKES];
    u32  take_count;
    u32  selected;
    u32  state;
    u32  elapsed_millis;
    u32  level_permille;
    u32  peak_permille;
    u32  recorded_total;
    u32  transcribe_requests;
    u32  input_gain;
    u32  bits;
    u32  waveform[32];
    u32  waveform_head;
    bool_t monitoring;
    bool_t auto_transcribe;
} axrc_state_t;

static axrc_state_t axrc_state;

static void axrc_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axrc_copy_string(char *dst, const char *src, u32 limit)
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

static void axrc_format_stamp(char *out, u32 millis)
{
    u32 seconds = millis / 1000;
    u32 minutes = seconds / 60;
    out[0] = (char)('0' + (minutes / 10) % 10);
    out[1] = (char)('0' + minutes % 10);
    out[2] = ':';
    out[3] = (char)('0' + (seconds / 10) % 10);
    out[4] = (char)('0' + seconds % 10);
    out[5] = 0;
}

static u32 axrc_digits(char *out, u32 value)
{
    char buffer[12];
    u32 digits = 0;
    u32 i;
    if (value == 0)
    {
        out[0] = '0';
        out[1] = 0;
        return 1;
    }
    while (value != 0 && digits < 11)
    {
        buffer[digits++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (i = 0; i < digits; i++)
        out[i] = buffer[digits - 1 - i];
    out[digits] = 0;
    return digits;
}

static axrc_take_t *axrc_take(u32 index)
{
    if (index >= axrc_state.take_count)
        return NULL_PTR;
    if (!axrc_state.takes[index].used)
        return NULL_PTR;
    return &axrc_state.takes[index];
}

/* -------------------------------------------------------------- public API */

void axrc_init(void)
{
    axrc_zero(&axrc_state, (u32)sizeof(axrc_state));
    axrc_state.input_gain = 100;
    axrc_state.bits = 16;
}

u32 axrc_create(const char *title, u32 sample_rate, u32 channels, u8 source)
{
    axrc_take_t *take;
    if (axrc_state.take_count >= AXRC_MAX_TAKES)
        return 0xFFFFFFFFu;
    if (sample_rate < 8000 || sample_rate > 48000)
        return 0xFFFFFFFFu;
    if (channels == 0 || channels > 2)
        return 0xFFFFFFFFu;
    take = &axrc_state.takes[axrc_state.take_count];
    axrc_zero(take, (u32)sizeof(*take));
    axrc_copy_string(take->title, title, AXRC_TITLE_LEN);
    take->sample_rate = sample_rate;
    take->channels = channels;
    take->source = source;
    take->used = true;
    axrc_state.take_count++;
    axrc_state.selected = axrc_state.take_count - 1;
    axrc_state.state = AXRC_STATE_IDLE;
    axrc_state.elapsed_millis = 0;
    return axrc_state.take_count - 1;
}

bool_t axrc_start(void)
{
    if (axrc_take(axrc_state.selected) == NULL_PTR)
        return false;
    if (axrc_state.state == AXRC_STATE_RECORDING)
        return true;
    axrc_state.state = AXRC_STATE_RECORDING;
    return true;
}

bool_t axrc_pause(void)
{
    if (axrc_state.state != AXRC_STATE_RECORDING)
        return false;
    axrc_state.state = AXRC_STATE_PAUSED;
    return true;
}

bool_t axrc_resume(void)
{
    if (axrc_state.state != AXRC_STATE_PAUSED)
        return false;
    axrc_state.state = AXRC_STATE_RECORDING;
    return true;
}

bool_t axrc_stop(void)
{
    axrc_take_t *take = axrc_take(axrc_state.selected);
    if (take == NULL_PTR)
        return false;
    if (axrc_state.state != AXRC_STATE_RECORDING && axrc_state.state != AXRC_STATE_PAUSED)
        return false;
    take->duration_millis = axrc_state.elapsed_millis;
    take->bytes = (take->duration_millis * take->sample_rate * take->channels * axrc_state.bits) / 8000;
    take->peak_permille = (u16)axrc_state.peak_permille;
    axrc_state.recorded_total += take->duration_millis;
    axrc_state.state = AXRC_STATE_IDLE;
    if (axrc_state.auto_transcribe)
        axrc_state.transcribe_requests++;
    return true;
}

u32 axrc_tick(u32 elapsed_millis, u32 input_permille)
{
    if (axrc_state.state != AXRC_STATE_RECORDING)
        return axrc_state.elapsed_millis;
    axrc_state.elapsed_millis += elapsed_millis;
    {
        u32 scaled = (input_permille * axrc_state.input_gain) / 100;
        if (scaled > 1000)
            scaled = 1000;
        axrc_state.level_permille = scaled;
        if (scaled > axrc_state.peak_permille)
            axrc_state.peak_permille = scaled;
        axrc_state.waveform[axrc_state.waveform_head] = scaled;
        axrc_state.waveform_head = (axrc_state.waveform_head + 1) % 32;
    }
    return axrc_state.elapsed_millis;
}

bool_t axrc_set_gain(u32 permille)
{
    if (permille < 20 || permille > 400)
        return false;
    axrc_state.input_gain = permille;
    return true;
}

bool_t axrc_set_monitoring(bool_t enabled)
{
    axrc_state.monitoring = enabled;
    return enabled;
}

bool_t axrc_add_segment(const char *text, u32 start_millis, u32 end_millis, u8 speaker)
{
    axrc_take_t *take = axrc_take(axrc_state.selected);
    axrc_segment_t *segment;
    if (take == NULL_PTR || text == NULL_PTR)
        return false;
    if (take->segments >= AXRC_MAX_SEGMENTS)
        return false;
    if (end_millis <= start_millis)
        return false;
    segment = &take->segment[take->segments];
    axrc_copy_string(segment->text, text, AXRC_SEGMENT_LEN);
    segment->start_millis = start_millis;
    segment->end_millis = end_millis;
    segment->speaker = speaker;
    segment->used = true;
    take->segments++;
    return true;
}

const char *axrc_segment_text(u32 take, u32 index)
{
    axrc_take_t *entry = axrc_take(take);
    if (entry == NULL_PTR || index >= entry->segments)
        return NULL_PTR;
    return entry->segment[index].text;
}

u32 axrc_segment_start(u32 take, u32 index)
{
    axrc_take_t *entry = axrc_take(take);
    if (entry == NULL_PTR || index >= entry->segments)
        return 0;
    return entry->segment[index].start_millis;
}

u32 axrc_take_count(void)
{
    return axrc_state.take_count;
}

u32 axrc_elapsed(void)
{
    return axrc_state.elapsed_millis;
}

u32 axrc_state_code(void)
{
    return axrc_state.state;
}

bool_t axrc_select(u32 index)
{
    if (axrc_take(index) == NULL_PTR)
        return false;
    axrc_state.selected = index;
    axrc_state.elapsed_millis = axrc_state.takes[index].duration_millis;
    return true;
}

bool_t axrc_remove(u32 index)
{
    axrc_take_t *take = axrc_take(index);
    if (take == NULL_PTR)
        return false;
    axrc_zero(take, (u32)sizeof(*take));
    if (axrc_state.take_count > 0)
        axrc_state.take_count--;
    return true;
}

bool_t axrc_toggle_noise_suppression(u32 index)
{
    axrc_take_t *take = axrc_take(index);
    if (take == NULL_PTR)
        return false;
    take->noise_suppressed = !take->noise_suppressed;
    return take->noise_suppressed;
}

u32 axrc_total_bytes(void)
{
    u32 i;
    u32 total = 0;
    for (i = 0; i < axrc_state.take_count; i++)
        total += axrc_state.takes[i].bytes;
    return total;
}

/* ----------------------------------------------------------------- painting */

static void axrc_paint_meter(gui_surface_t *surface, gui_rect_t meter)
{
    u32 i;
    u32 index = axrc_state.waveform_head;
    i32 step = meter.w / 32;
    gui_fill_round_rect(surface, meter, 6, GUI_RGB(0x10, 0x12, 0x16));
    for (i = 0; i < 32; i++)
    {
        u32 value = axrc_state.waveform[index];
        i32 centre = meter.y + meter.h / 2;
        i32 half = (i32)((value * (u32)(meter.h / 2 - 4)) / 1000);
        i32 x = meter.x + (i32)i * step;
        gui_fill_rect(surface, gui_rect(x, centre - half, step - 2, half * 2), AXRC_COLOUR_WAVE);
        index = (index + 1) % 32;
    }
    {
        gui_rect_t level = gui_rect(meter.x, meter.y + meter.h - 6,
                                    (meter.w * (i32)axrc_state.level_permille) / 1000, 4);
        gui_fill_rect(surface, level, AXRC_COLOUR_ACCENT);
    }
}

static void axrc_paint_takes(gui_surface_t *surface, gui_rect_t panel)
{
    u32 i;
    gui_text(surface, panel.x + 12, panel.y + 10, "Takes", AXRC_COLOUR_MUTED, 1);
    for (i = 0; i < axrc_state.take_count && i < 7; i++)
    {
        const axrc_take_t *take = &axrc_state.takes[i];
        gui_rect_t row;
        u32 colour;
        char stamp[16];
        if (!take->used)
            continue;
        row = gui_rect(panel.x + 8, panel.y + 32 + (i32)i * 56, panel.w - 16, 50);
        colour = (i == axrc_state.selected) ? GUI_RGB(0x24, 0x2E, 0x48) : GUI_RGB(0x19, 0x1D, 0x25);
        gui_fill_round_rect(surface, row, 8, colour);
        gui_text(surface, row.x + 10, row.y + 6, take->title, AXRC_COLOUR_TEXT, 1);
        axrc_format_stamp(stamp, take->duration_millis);
        gui_text(surface, row.x + 10, row.y + 24, stamp, AXRC_COLOUR_MUTED, 1);
        {
            char info[16];
            u32 digits = axrc_digits(info, take->sample_rate / 1000);
            info[digits] = 'k';
            info[digits + 1] = 0;
            gui_text(surface, row.x + row.w - 92, row.y + 24, info, AXRC_COLOUR_MUTED, 1);
        }
        if (take->noise_suppressed)
            gui_text(surface, row.x + row.w - 44, row.y + 6, "ns", AXRC_COLOUR_ACCENT, 1);
    }
}

static void axrc_paint_transcript(gui_surface_t *surface, gui_rect_t panel)
{
    const axrc_take_t *take = axrc_take(axrc_state.selected);
    u32 i;
    if (take == NULL_PTR)
        return;
    gui_text(surface, panel.x + 12, panel.y + 10, "Transcript", AXRC_COLOUR_MUTED, 1);
    for (i = 0; i < take->segments && i < 9; i++)
    {
        char stamp[AXRC_STAMP_LEN];
        i32 y = panel.y + 32 + (i32)i * 28;
        axrc_format_stamp(stamp, take->segment[i].start_millis);
        gui_text(surface, panel.x + 12, y + 8, stamp, AXRC_COLOUR_MUTED, 1);
        gui_text(surface, panel.x + 72, y + 8, take->segment[i].text, AXRC_COLOUR_TEXT, 1);
        gui_fill_rect(surface, gui_rect(panel.x + 12, y + 26, panel.w - 24, 1), AXRC_COLOUR_GRID);
    }
}

void axrc_paint(gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t left = gui_rect(bounds.x, bounds.y, bounds.w / 2, bounds.h);
    gui_rect_t right = gui_rect(bounds.x + bounds.w / 2, bounds.y, bounds.w / 2, bounds.h);
    gui_rect_t meter = gui_rect(left.x + 16, left.y + 56, left.w - 32, 72);
    char status[24];
    u32 digits;
    gui_fill_rect(surface, bounds, AXRC_COLOUR_BG);
    gui_fill_round_rect(surface, gui_rect(left.x + 8, left.y + 8, left.w - 16, left.h - 16), 10,
                        AXRC_COLOUR_PANEL);
    gui_fill_round_rect(surface, gui_rect(right.x + 8, right.y + 8, right.w - 16, right.h - 16), 10,
                        GUI_RGB(0x1A, 0x1E, 0x26));
    gui_text(surface, left.x + 16, left.y + 20, "Recorder", AXRC_COLOUR_TEXT, 1);
    digits = axrc_digits(status, axrc_state.elapsed_millis / 1000);
    status[digits] = 's';
    status[digits + 1] = 0;
    gui_text(surface, left.x + left.w - 80, left.y + 20, status, AXRC_COLOUR_MUTED, 1);
    axrc_paint_meter(surface, meter);
    {
        gui_rect_t button = gui_rect(left.x + 16, left.y + 148, 88, 28);
        gui_fill_round_rect(surface, button, 14,
                            axrc_state.state == AXRC_STATE_RECORDING ? AXRC_COLOUR_ACCENT
                                                                     : GUI_RGB(0x2C, 0x33, 0x42));
        gui_text(surface, button.x + 24, button.y + 7,
                 axrc_state.state == AXRC_STATE_RECORDING ? "Stop" : "Record",
                 AXRC_COLOUR_TEXT, 1);
    }
    {
        gui_rect_t button = gui_rect(left.x + 116, left.y + 148, 88, 28);
        gui_fill_round_rect(surface, button, 14, GUI_RGB(0x2C, 0x33, 0x42));
        gui_text(surface, button.x + 22, button.y + 7,
                 axrc_state.state == AXRC_STATE_PAUSED ? "Resume" : "Pause",
                 AXRC_COLOUR_TEXT, 1);
    }
    axrc_paint_takes(surface, gui_rect(left.x, left.y + 190, left.w, left.h - 198));
    axrc_paint_transcript(surface, gui_rect(right.x, right.y, right.w, right.h));
}

bool_t axrc_click(i32 x, i32 y, gui_rect_t bounds)
{
    gui_rect_t left = gui_rect(bounds.x, bounds.y, bounds.w / 2, bounds.h);
    gui_rect_t record = gui_rect(left.x + 16, left.y + 148, 88, 28);
    gui_rect_t pause = gui_rect(left.x + 116, left.y + 148, 88, 28);
    gui_rect_t list = gui_rect(left.x, left.y + 190, left.w, left.h - 198);
    u32 i;
    if (gui_rect_contains(record, x, y))
    {
        if (axrc_state.state == AXRC_STATE_RECORDING)
            return axrc_stop();
        if (axrc_state.state == AXRC_STATE_PAUSED)
            return axrc_resume();
        return axrc_start();
    }
    if (gui_rect_contains(pause, x, y))
    {
        if (axrc_state.state == AXRC_STATE_PAUSED)
            return axrc_resume();
        return axrc_pause();
    }
    if (gui_rect_contains(list, x, y))
    {
        for (i = 0; i < axrc_state.take_count && i < 7; i++)
        {
            gui_rect_t row = gui_rect(list.x + 8, list.y + 32 + (i32)i * 56, list.w - 16, 50);
            if (gui_rect_contains(row, x, y))
                return axrc_select(i);
        }
    }
    return false;
}

void axrc_report(void)
{
    u32 i;
    printk("axrc: takes=%u total=%ums bytes=%u gain=%u state=%u\n",
           (unsigned)axrc_state.take_count, (unsigned)axrc_state.recorded_total,
           (unsigned)axrc_total_bytes(), (unsigned)axrc_state.input_gain,
           (unsigned)axrc_state.state);
    for (i = 0; i < axrc_state.take_count; i++)
    {
        const axrc_take_t *take = &axrc_state.takes[i];
        if (!take->used)
            continue;
        printk("  take %u %s dur=%ums %ukHz ch=%u segs=%u peak=%u\n", (unsigned)i,
               take->title, (unsigned)take->duration_millis,
               (unsigned)(take->sample_rate / 1000), (unsigned)take->channels,
               (unsigned)take->segments, (unsigned)take->peak_permille);
    }
}

bool_t axrc_selftest(void)
{
    u32 index;
    axrc_init();
    index = axrc_create("Meeting notes", 16000, 1, AXRC_SOURCE_MIC);
    if (index != 0)
        return false;
    if (axrc_create("Line input", 48000, 2, AXRC_SOURCE_LINE) != 1)
        return false;
    if (axrc_create("Bad", 40000, 0, AXRC_SOURCE_MIX) != 0xFFFFFFFFu)
        return false;
    if (!axrc_start())
        return false;
    axrc_tick(1000, 300);
    axrc_tick(1000, 800);
    if (axrc_state.level_permille != 800)
        return false;
    if (!axrc_pause())
        return false;
    if (!axrc_resume())
        return false;
    axrc_tick(2000, 100);
    if (!axrc_stop())
        return false;
    if (axrc_state.takes[0].duration_millis != 4000)
        return false;
    if (!axrc_add_segment("first block", 0, 2000, 0))
        return false;
    if (!axrc_add_segment("second block", 2000, 4000, 1))
        return false;
    if (axrc_add_segment("bad", 5, 4, 0))
        return false;
    if (axrc_segment_text(0, 1) == NULL_PTR)
        return false;
    if (!axrc_set_gain(150))
        return false;
    if (axrc_set_gain(10))
        return false;
    if (!axrc_select(1))
        return false;
    if (!axrc_toggle_noise_suppression(0))
        return false;
    if (axrc_total_bytes() == 0)
        return false;
    if (!axrc_remove(1))
        return false;
    return true;
}
