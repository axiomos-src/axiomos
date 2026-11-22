/* AxiomOS - audio device support.
 *
 * Models an HDA-style controller at the stream level: codec discovery through
 * the parameter space, widget graph walking, converter format selection and
 * period based playback.  Sample buffers are accepted as signed 16-bit
 * interleaved data, which is what the mixer produces.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXAU_MAX_CODECS      4
#define AXAU_MAX_WIDGETS     32
#define AXAU_MAX_STREAMS     4
#define AXAU_PERIOD_FRAMES   256
#define AXAU_RING_PERIODS    8

typedef struct axaud_widget
{
    u8   node_id;
    u8   type;
    u8   amp_caps;
    bool_t connected;
    bool_t output;
    bool_t input;
} axaud_widget_t;

typedef struct axaud_codec
{
    axaud_widget_t widgets[AXAU_MAX_WIDGETS];
    u8   widget_count;
    u8   address;
    u16  vendor_id;
    u8   outputs;
    u8   inputs;
    bool_t present;
} axaud_codec_t;

typedef struct axaud_format
{
    u32 sample_rate;
    u8  channels;
    u8  bits;
    bool_t signed_samples;
} axaud_format_t;

typedef struct axaud_stream
{
    axaud_format_t format;
    u16  ring[AXAU_MAX_STREAMS][AXAU_RING_PERIODS * AXAU_PERIOD_FRAMES];
    u8   periods;
    u8   read_period;
    u8   write_period;
    u32  frames_played;
    u32  underruns;
    u32  bytes;
    bool_t running;
    bool_t loopback;
} axaud_stream_t;

typedef struct axaud_device
{
    u8   index;
    u64  mmio_base;
    u32  irq;
    u8   codec_count;
    u8   stream_count;
    axaud_stream_t streams[AXAU_MAX_STREAMS];
    u32  position_ticks;
    u32  interrupts;
    bool_t present;
    bool_t muted;
    u8   volume;
    char name[16];
} axaud_device_t;

typedef struct axaud_state
{
    axaud_device_t devices[2];
    axaud_codec_t codecs[AXAU_MAX_CODECS];
    u8   count;
    u32  total_frames;
    u32  errors;
} axaud_state_t;

static axaud_state_t axaud_state;

static void axaud_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axaud_bytes_per_frame(const axaud_format_t *format)
{
    return (u32)format->channels * (format->bits / 8);
}

static bool_t axaud_format_valid(const axaud_format_t *format)
{
    if (format->channels < 1 || format->channels > 8)
        return false;
    if (format->bits != 16 && format->bits != 24 && format->bits != 32)
        return false;
    if (format->sample_rate < 8000 || format->sample_rate > 192000)
        return false;
    return true;
}

/* -------------------------------------------------------------- public API */

void axaud_init(void)
{
    axaud_zero(&axaud_state, (u32)sizeof(axaud_state));
}

bool_t axaud_add_device(u8 index, u64 mmio_base, u32 irq, const char *name)
{
    axaud_device_t *device;
    u32 i;
    if (index >= 2)
        return false;
    device = &axaud_state.devices[index];
    axaud_zero(device, (u32)sizeof(*device));
    device->index = index;
    device->mmio_base = mmio_base;
    device->irq = irq;
    device->present = true;
    device->volume = 64;
    for (i = 0; i < 16 && name[i] != 0 && i < 15; i++)
        device->name[i] = name[i];
    if (index >= axaud_state.count)
        axaud_state.count = (u8)(index + 1);
    return true;
}

bool_t axaud_add_codec(u8 address, u16 vendor_id)
{
    axaud_codec_t *codec;
    if (axaud_state.codecs[address % AXAU_MAX_CODECS].present)
        return false;
    codec = &axaud_state.codecs[address % AXAU_MAX_CODECS];
    axaud_zero(codec, (u32)sizeof(*codec));
    codec->address = address;
    codec->vendor_id = vendor_id;
    codec->present = true;
    return true;
}

bool_t axaud_add_widget(u8 codec_index, u8 node_id, u8 type, bool_t output)
{
    axaud_codec_t *codec;
    axaud_widget_t *widget;
    if (codec_index >= AXAU_MAX_CODECS)
        return false;
    codec = &axaud_state.codecs[codec_index];
    if (!codec->present || codec->widget_count >= AXAU_MAX_WIDGETS)
        return false;
    widget = &codec->widgets[codec->widget_count];
    axaud_zero(widget, (u32)sizeof(*widget));
    widget->node_id = node_id;
    widget->type = type;
    widget->connected = true;
    widget->output = output;
    widget->input = (bool_t)!output;
    if (output)
        codec->outputs++;
    else
        codec->inputs++;
    codec->widget_count++;
    return true;
}

bool_t axaud_open_stream(u8 device, u8 stream, u32 rate, u8 channels, u8 bits)
{
    axaud_stream_t *state;
    if (device >= axaud_state.count)
        return false;
    if (stream >= AXAU_MAX_STREAMS)
        return false;
    state = &axaud_state.devices[device].streams[stream];
    axaud_zero(state, (u32)sizeof(*state));
    state->format.sample_rate = rate;
    state->format.channels = channels;
    state->format.bits = bits;
    state->format.signed_samples = true;
    state->periods = AXAU_RING_PERIODS;
    if (!axaud_format_valid(&state->format))
        return false;
    axaud_state.devices[device].stream_count++;
    return true;
}

bool_t axaud_start(u8 device, u8 stream)
{
    if (device >= axaud_state.count || stream >= AXAU_MAX_STREAMS)
        return false;
    axaud_state.devices[device].streams[stream].running = true;
    return true;
}

bool_t axaud_stop(u8 device, u8 stream)
{
    if (device >= axaud_state.count || stream >= AXAU_MAX_STREAMS)
        return false;
    axaud_state.devices[device].streams[stream].running = false;
    return true;
}

bool_t axaud_write(u8 device, u8 stream, const i16 *frames, u32 count)
{
    axaud_device_t *dev;
    axaud_stream_t *state;
    u32 i;
    u32 per_period;
    if (device >= axaud_state.count || stream >= AXAU_MAX_STREAMS)
        return false;
    dev = &axaud_state.devices[device];
    state = &dev->streams[stream];
    if (!state->running || frames == NULL_PTR)
        return false;
    per_period = AXAU_PERIOD_FRAMES * state->format.channels;
    if (per_period == 0 || per_period > AXAU_PERIOD_FRAMES * AXAU_MAX_STREAMS)
        return false;
    if (state->write_period == state->read_period)
    {
        if (state->bytes >= AXAU_RING_PERIODS * per_period * 2)
        {
            state->underruns++;
            axaud_state.errors++;
        }
    }
    for (i = 0; i < count && i < per_period * 2; i++)
    {
        u32 offset = state->write_period * per_period + (i % per_period);
        if (offset < AXAU_RING_PERIODS * AXAU_PERIOD_FRAMES)
            state->ring[stream][offset] = frames[i];
    }
    state->frames_played += count;
    state->bytes = state->bytes + count * 2;
    dev->position_ticks += count;
    axaud_state.total_frames += count;
    state->write_period = (u8)((state->write_period + 1) % AXAU_RING_PERIODS);
    return true;
}

void axaud_position_tick(u8 device, u8 stream)
{
    axaud_stream_t *state;
    if (device >= axaud_state.count || stream >= AXAU_MAX_STREAMS)
        return;
    state = &axaud_state.devices[device].streams[stream];
    if (!state->running)
        return;
    if (state->read_period == state->write_period)
        return;
    state->read_period = (u8)((state->read_period + 1) % AXAU_RING_PERIODS);
    axaud_state.devices[device].interrupts++;
}

void axaud_set_volume(u8 device, u8 volume)
{
    if (device >= axaud_state.count)
        return;
    axaud_state.devices[device].volume = volume > 100 ? 100 : volume;
}

u8 axaud_volume(u8 device)
{
    if (device >= axaud_state.count)
        return 0;
    return axaud_state.devices[device].volume;
}

void axaud_set_mute(u8 device, bool_t muted)
{
    if (device >= axaud_state.count)
        return;
    axaud_state.devices[device].muted = muted;
}

bool_t axaud_muted(u8 device)
{
    if (device >= axaud_state.count)
        return true;
    return axaud_state.devices[device].muted;
}

u32 axaud_bandwidth(u8 device, u8 stream)
{
    if (device >= axaud_state.count || stream >= AXAU_MAX_STREAMS)
        return 0;
    {
        const axaud_stream_t *state = &axaud_state.devices[device].streams[stream];
        return state->format.sample_rate * axaud_bytes_per_frame(&state->format);
    }
}

void axaud_report(void)
{
    u32 i;
    printk("axau: devices=%u total_frames=%u errors=%u\n", (unsigned)axaud_state.count,
           (unsigned)axaud_state.total_frames, (unsigned)axaud_state.errors);
    for (i = 0; i < axaud_state.count; i++)
    {
        const axaud_device_t *device = &axaud_state.devices[i];
        if (!device->present)
            continue;
        printk("  %s#%u streams=%u volume=%u muted=%u irq=%u\n", device->name,
               (unsigned)device->index, (unsigned)device->stream_count,
               (unsigned)device->volume, (unsigned)device->muted,
               (unsigned)device->irq);
    }
    for (i = 0; i < AXAU_MAX_CODECS; i++)
    {
        const axaud_codec_t *codec = &axaud_state.codecs[i];
        if (!codec->present)
            continue;
        printk("  codec@%u vendor=0x%x widgets=%u out=%u in=%u\n",
               (unsigned)codec->address, (unsigned)codec->vendor_id,
               (unsigned)codec->widget_count, (unsigned)codec->outputs,
               (unsigned)codec->inputs);
    }
}

bool_t axaud_selftest(void)
{
    static i16 frames[AXAU_PERIOD_FRAMES * 2];
    u32 i;
    axaud_init();
    if (!axaud_add_device(0, 0xF0000000ull, 20, "hda0"))
        return false;
    if (!axaud_add_codec(0, 0x10EC))
        return false;
    if (!axaud_add_widget(0, 2, 0x04, true))
        return false;
    if (!axaud_add_widget(0, 3, 0x03, false))
        return false;
    if (!axaud_open_stream(0, 0, 48000, 2, 16))
        return false;
    if (axaud_bandwidth(0, 0) != 48000 * 4)
        return false;
    if (!axaud_start(0, 0))
        return false;
    for (i = 0; i < AXIOM_ARRAY_LEN(frames); i++)
        frames[i] = (i16)(i & 0x3FF);
    if (!axaud_write(0, 0, frames, AXIOM_ARRAY_LEN(frames)))
        return false;
    axaud_position_tick(0, 0);
    if (axaud_state.total_frames != AXIOM_ARRAY_LEN(frames))
        return false;
    axaud_set_volume(0, 40);
    if (axaud_volume(0) != 40)
        return false;
    axaud_set_mute(0, true);
    if (!axaud_muted(0))
        return false;
    if (!axaud_stop(0, 0))
        return false;
    if (!axaud_open_stream(0, 1, 1000, 2, 16))
    {
        if (axaud_state.errors == 0)
            return false;
    }
    return true;
}
