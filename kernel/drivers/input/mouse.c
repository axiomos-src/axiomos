/*
 * AxiomOS - PS/2 mouse driver.
 *
 * A PS/2 mouse sends fixed-size packets, three bytes for a plain device and
 * four for one with a wheel.  The first byte always has bit 3 set; that bit is
 * the only synchronisation marker the protocol offers, so a driver that loses a
 * byte must resynchronise by scanning for it.  We keep the packet length
 * explicit and never assume the next byte is byte zero.
 *
 * Movement arrives as a signed 9-bit value split across the flags byte and the
 * movement byte.  Sign extension is the classic bug in this driver class, so it
 * is done in one place, `decode_delta`, and the test vectors in selftest.c
 * exercise both the positive and negative boundaries.
 *
 * Acceleration is a small piecewise curve rather than a multiplier: pointer
 * movement on a 1024x768 surface with a 100 dpi mouse needs sub-pixel
 * precision at low speed and a boost at high speed, and a pure multiplier
 * cannot give both.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>
#include <axiom/axinput.h>

static mouse_driver_t g_mouse;
static axinput_ring_t *g_ring;

#define MOUSE_WHEEL_SCALE 1
#define MOUSE_ACCEL_THRESHOLD 4
#define MOUSE_ACCEL_FACTOR_NUM 3
#define MOUSE_ACCEL_FACTOR_DEN 2

static u64 mouse_now(void)
{
    return axbpf_now_ns();
}

/* ------------------------------------------------------------------ */
/* Packet decoding                                                     */
/* ------------------------------------------------------------------ */

/* Sign-extend a 9-bit delta assembled from the overflow bits and the data byte.
 * Bit 8 lives in the flags byte (0x10 for x, 0x20 for y). */
static i32 decode_delta(u8 flags, u8 data, u8 sign_mask)
{
    i32 value = (i32)data;
    if (flags & sign_mask) {
        value -= 256;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Event emission                                                      */
/* ------------------------------------------------------------------ */

static void emit_move(i32 dx, i32 dy, u32 buttons)
{
    if (!g_ring) {
        return;
    }
    axinput_event_t event;
    ax_memset(&event, 0, sizeof(event));
    event.timestamp_ns = mouse_now();
    event.class = AXINPUT_MOUSE_MOVE;
    event.code = 0;
    event.value = 0;
    event.x = g_mouse.state.x;
    event.y = g_mouse.state.y;
    event.modifiers = buttons;
    event.device = AXINPUT_DEVICE_MOUSE;
    (void)axinput_ring_push(g_ring, &event);
}

static void emit_button(u32 changed, u32 buttons)
{
    if (!g_ring) {
        return;
    }
    for (u32 bit = 0; bit < 5; bit++) {
        u32 mask = 1u << bit;
        if (!(changed & mask)) {
            continue;
        }
        axinput_event_t event;
        ax_memset(&event, 0, sizeof(event));
        event.timestamp_ns = mouse_now();
        event.class = AXINPUT_MOUSE_BUTTON;
        event.code = (u16)mask;
        event.value = (buttons & mask) ? 1 : 0;
        event.x = g_mouse.state.x;
        event.y = g_mouse.state.y;
        event.modifiers = buttons;
        event.device = AXINPUT_DEVICE_MOUSE;
        (void)axinput_ring_push(g_ring, &event);
    }
}

static void emit_wheel(i32 delta)
{
    if (!g_ring || delta == 0) {
        return;
    }
    axinput_event_t event;
    ax_memset(&event, 0, sizeof(event));
    event.timestamp_ns = mouse_now();
    event.class = AXINPUT_MOUSE_WHEEL;
    event.code = 0;
    event.value = delta;
    event.x = g_mouse.state.x;
    event.y = g_mouse.state.y;
    event.device = AXINPUT_DEVICE_MOUSE;
    (void)axinput_ring_push(g_ring, &event);
}

/* ------------------------------------------------------------------ */
/* Acceleration                                                        */
/* ------------------------------------------------------------------ */

i32 mouse_accelerate(i32 delta, u32 speed_factor)
{
    i32 magnitude = delta < 0 ? -delta : delta;
    if (magnitude <= MOUSE_ACCEL_THRESHOLD) {
        return delta;
    }
    i32 boosted = (delta * MOUSE_ACCEL_FACTOR_NUM) / MOUSE_ACCEL_FACTOR_DEN;
    /* Clamp so a single packet cannot teleport the cursor. */
    i32 limit = 64 * (i32)(speed_factor ? speed_factor : 1);
    if (boosted > limit) {
        boosted = limit;
    }
    if (boosted < -limit) {
        boosted = -limit;
    }
    return boosted;
}

/* ------------------------------------------------------------------ */
/* Packet assembly                                                     */
/* ------------------------------------------------------------------ */

static void clamp_position(void)
{
    if (g_mouse.state.x < 0) g_mouse.state.x = 0;
    if (g_mouse.state.y < 0) g_mouse.state.y = 0;
    if (g_mouse.state.x > g_mouse.state.max_x - 1) {
        g_mouse.state.x = g_mouse.state.max_x - 1;
    }
    if (g_mouse.state.y > g_mouse.state.max_y - 1) {
        g_mouse.state.y = g_mouse.state.max_y - 1;
    }
}

i32 mouse_process_packet(const u8 *packet, u32 length, mouse_protocol_t protocol)
{
    if (!packet || length < 3) {
        return -22;
    }
    u8 flags = packet[0];
    if (!(flags & 0x08)) {
        /* Not a first byte; caller should resynchronise. */
        g_mouse.state.resyncs++;
        return -71;
    }
    if (flags & 0xC0) {
        /* Overflow: discard the packet rather than jump the pointer. */
        g_mouse.state.resyncs++;
        return -75;
    }

    i32 dx = decode_delta(flags, packet[1], 0x10);
    i32 dy = decode_delta(flags, packet[2], 0x20);
    /* The y axis is inverted relative to screen coordinates. */
    dy = -dy;

    u32 buttons = 0;
    if (flags & 0x01) buttons |= AXMOUSE_LEFT;
    if (flags & 0x02) buttons |= AXMOUSE_RIGHT;
    if (flags & 0x04) buttons |= AXMOUSE_MIDDLE;
    if (protocol == MOUSE_PROTO_IMPS2_5BUTTON && length >= 4) {
        if (packet[3] & 0x10) buttons |= AXMOUSE_BUTTON4;
        if (packet[3] & 0x20) buttons |= AXMOUSE_BUTTON5;
    }

    i32 wheel = 0;
    if ((protocol == MOUSE_PROTO_IMPS2 || protocol == MOUSE_PROTO_IMPS2_5BUTTON)
        && length >= 4) {
        i8 raw = (i8)packet[3];
        /* Only the low nibble carries wheel movement; the high bits are the
         * button 4/5 state. */
        i8 nibble = (i8)(raw & 0x0F);
        if (nibble > 7) {
            nibble = (i8)(nibble - 16);
        }
        wheel = (i32)nibble;
    }

    i32 adx = mouse_accelerate(dx, 1);
    i32 ady = mouse_accelerate(dy, 1);
    g_mouse.state.movement_accum_x += adx;
    g_mouse.state.movement_accum_y += ady;
    (void)ady;

    if (dx || dy) {
        g_mouse.state.x += adx;
        g_mouse.state.y += ady;
        clamp_position();
        emit_move(adx, ady, buttons);
    }

    u32 changed = g_mouse.state.buttons ^ buttons;
    if (changed) {
        g_mouse.state.buttons = buttons;
        emit_button(changed, buttons);
    }

    if (wheel) {
        g_mouse.state.wheel_accum += wheel;
        emit_wheel(wheel);
    }

    g_mouse.state.packets++;
    return 0;
}

/* Feed a byte from the controller.  Returns 0 when consumed, 1 when a packet
 * completed, negative on error. */
i32 mouse_feed_byte(u8 byte)
{
    if (!g_mouse.initialised) {
        return -19;
    }
    g_mouse.bytes_seen++;

    mouse_state_t *state = &g_mouse.state;
    u32 expected = state->expected_len ? state->expected_len : 3;

    if (state->packet_len == 0) {
        if (!(byte & 0x08)) {
            /* Not a packet start.  Count it and drop. */
            state->bad_bytes++;
            return 0;
        }
    } else if (expected == 4 && state->packet_len == 3) {
        /* Fourth byte of an Intellimouse packet; no start-bit requirement. */
    } else if (!(byte & 0x08)) {
        /* A mid-packet byte without the start bit means we lost sync; restart
         * with this byte as a potential first byte. */
        state->packet_len = 0;
        state->resyncs++;
        if (!(byte & 0x08)) {
            state->bad_bytes++;
            return 0;
        }
    }

    state->packet[state->packet_len++] = byte;
    if (state->packet_len < expected) {
        return 0;
    }

    i32 rc = mouse_process_packet(state->packet, expected, state->protocol);
    state->packet_len = 0;
    return rc < 0 ? rc : 1;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

void mouse_set_bounds(i32 width, i32 height)
{
    if (width <= 0) width = AXMOUSE_MAX_X;
    if (height <= 0) height = AXMOUSE_MAX_Y;
    g_mouse.state.max_x = width;
    g_mouse.state.max_y = height;
    clamp_position();
}

void mouse_get_position(i32 *x, i32 *y)
{
    if (x) *x = g_mouse.state.x;
    if (y) *y = g_mouse.state.y;
}

u32 axmouse_buttons(void)
{
    return g_mouse.state.buttons;
}

void mouse_set_speed(u8 sample_rate, u8 resolution)
{
    if (sample_rate == 0) sample_rate = 100;
    if (resolution == 0 || resolution > 3) resolution = 3;
    g_mouse.state.sample_rate = sample_rate;
    g_mouse.state.resolution = resolution;
    if (!g_mouse.controller || !g_mouse.controller->mouse_present) {
        return;
    }
    (void)i8042_write_aux(I8042_MOUSE_CMD_SET_SAMPLE);
    (void)i8042_write_aux(sample_rate);
    (void)i8042_write_aux(I8042_MOUSE_CMD_SET_RES);
    (void)i8042_write_aux(resolution);
}

void mouse_reset(void)
{
    ax_memset(&g_mouse.state.packet, 0, sizeof(g_mouse.state.packet));
    g_mouse.state.packet_len = 0;
    g_mouse.state.buttons = 0;
    g_mouse.state.wheel_accum = 0;
}

/* Move the pointer by a relative amount without hardware, used by the selftest
 * and by keyboard-driven pointer emulation. */
void mouse_emulate_move(i32 dx, i32 dy)
{
    u32 buttons = g_mouse.state.buttons;
    g_mouse.state.x += dx;
    g_mouse.state.y += dy;
    clamp_position();
    emit_move(dx, dy, buttons);
}

/* ------------------------------------------------------------------ */
/* Init and reporting                                                   */
/* ------------------------------------------------------------------ */

mouse_driver_t *mouse_init(axinput_ring_t *ring)
{
    ax_memset(&g_mouse, 0, sizeof(g_mouse));
    g_ring = ring;
    g_mouse.ring = ring;
    g_mouse.controller = i8042_init();
    g_mouse.state.max_x = AXMOUSE_MAX_X;
    g_mouse.state.max_y = AXMOUSE_MAX_Y;
    g_mouse.state.sample_rate = 100;
    g_mouse.state.resolution = 3;
    g_mouse.state.enabled = g_mouse.controller->mouse_present;
    if (g_mouse.controller->mouse_5button) {
        g_mouse.state.protocol = MOUSE_PROTO_IMPS2_5BUTTON;
        g_mouse.state.expected_len = 4;
    } else if (g_mouse.controller->mouse_wheel) {
        g_mouse.state.protocol = MOUSE_PROTO_IMPS2;
        g_mouse.state.expected_len = 4;
    } else {
        g_mouse.state.protocol = MOUSE_PROTO_PS2;
        g_mouse.state.expected_len = 3;
    }
    g_mouse.initialised = (bool_t)1;
    printk("mouse: %s protocol=%u packet=%u buttons=%u\n",
           g_mouse.state.enabled ? "ready" : "absent",
           (u32)g_mouse.state.protocol, g_mouse.state.expected_len,
           g_mouse.controller->mouse_5button ? 5u : (g_mouse.controller->mouse_wheel ? 3u : 2u));
    return &g_mouse;
}

bool_t mouse_ready(void)
{
    return (bool_t)(g_mouse.initialised && g_mouse.state.enabled);
}

void mouse_dump(void)
{
    printk("mouse: pos=%d,%d bounds=%dx%d buttons=%02x proto=%u\n",
           g_mouse.state.x, g_mouse.state.y, g_mouse.state.max_x,
           g_mouse.state.max_y, g_mouse.state.buttons,
           (u32)g_mouse.state.protocol);
    printk("mouse: packets=%u resyncs=%u bad=%u partial=%u rate=%u res=%u\n",
           g_mouse.state.packets, g_mouse.state.resyncs,
           g_mouse.state.bad_bytes, g_mouse.state.packet_len,
           g_mouse.state.sample_rate, g_mouse.state.resolution);
}
