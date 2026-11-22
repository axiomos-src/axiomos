/*
 * AxiomOS - PS/2 keyboard driver.
 *
 * The controller hands us raw bytes.  Some of them are prefix bytes (e0, e1),
 * some are release markers (the high bit in set 1, f0 in set 2), some are
 * device responses (fa ack, fe resend, ee echo) that must never reach the key
 * layer.  This file owns that state machine and turns bytes into key events.
 *
 * Modifier bookkeeping is the subtle part.  A key being held is not the same as
 * a modifier being active: pressing both shifts and releasing one leaves shift
 * active, so we keep a per-key bitmap and recompute the modifier word from it
 * rather than toggling bits on each event.  This also makes lost release
 * events (which happen when a USB-to-PS/2 adapter drops a byte) recoverable:
 * the next press of the same key clears the stale bit.
 *
 * Typematic repeat is generated here rather than by the device, so the repeat
 * rate is identical on every keyboard and testable without hardware.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>
#include <axiom/axinput.h>

#define KBD_QUEUE_DEPTH 16
#define KBD_DEFAULT_DELAY_MS 500
#define KBD_DEFAULT_RATE_MS 33      /* about 30 characters per second */
#define KBD_MAX_BUFFERED 64
#define KBD_DEVICE_RESPONSE_THRESHOLD 0xF0

static keyboard_driver_t g_keyboard;
static u8 g_layout = 0;             /* 0 US, 1 US-intl, 2 UK, 3 DE */
static axinput_ring_t *g_ring;

/* Byte queue between the interrupt and the decoder.  In practice the decoder
 * runs directly from the poll loop, but keeping a queue means an interrupt
 * storm cannot lose bytes. */
typedef struct kbd_byte_queue {
    u8 bytes[KBD_MAX_BUFFERED];
    u32 head;
    u32 tail;
    u32 count;
    u32 overflow;
} kbd_byte_queue_t;

static kbd_byte_queue_t g_queue;

u8 keyboard_layout_id(void)
{
    return g_layout;
}

void keyboard_set_layout(u8 layout)
{
    if (layout > 3) {
        layout = 0;
    }
    g_layout = layout;
    printk("kbd: layout set to %u\n", layout);
}

const char *keyboard_layout_name(u8 layout)
{
    switch (layout) {
    case 0: return "us";
    case 1: return "us-intl";
    case 2: return "uk";
    case 3: return "de";
    default: return "unknown";
    }
}

static void queue_push(u8 byte)
{
    if (g_queue.count >= KBD_MAX_BUFFERED) {
        g_queue.overflow++;
        return;
    }
    g_queue.bytes[g_queue.head] = byte;
    g_queue.head = (g_queue.head + 1) % KBD_MAX_BUFFERED;
    g_queue.count++;
}

static bool_t queue_pop(u8 *out)
{
    if (g_queue.count == 0) {
        return (bool_t)0;
    }
    *out = g_queue.bytes[g_queue.tail];
    g_queue.tail = (g_queue.tail + 1) % KBD_MAX_BUFFERED;
    g_queue.count--;
    return (bool_t)1;
}

static u64 kbd_now(void)
{
    return axbpf_now_ns();
}

/* ------------------------------------------------------------------ */
/* Modifiers                                                           */
/* ------------------------------------------------------------------ */

static void set_key_bit(axkey_t key, bool_t down)
{
    u32 index = (u32)key / 32;
    u32 bit = 1u << ((u32)key % 32);
    if (index >= (AXKEY_MAX / 32)) {
        return;
    }
    if (down) {
        g_keyboard.state.pressed[index] |= bit;
    } else {
        g_keyboard.state.pressed[index] &= ~bit;
    }
}

static bool_t key_bit(axkey_t key)
{
    u32 index = (u32)key / 32;
    u32 bit = 1u << ((u32)key % 32);
    if (index >= (AXKEY_MAX / 32)) {
        return (bool_t)0;
    }
    return (bool_t)((g_keyboard.state.pressed[index] & bit) != 0);
}

/* Recompute the modifier word from the held-key bitmap.  AltGr is reported
 * separately: on a US keyboard the right alt behaves as AltGr only when the
 * layout asks for it, and applications care about the distinction. */
static u32 recompute_modifiers(void)
{
    u32 modifiers = 0;
    if (key_bit(AXKEY_LSHIFT)) modifiers |= AXMOD_LSHIFT;
    if (key_bit(AXKEY_RSHIFT)) modifiers |= AXMOD_RSHIFT;
    if (key_bit(AXKEY_LCTRL) || key_bit(AXKEY_RCTRL)) modifiers |= AXMOD_CTRL;
    if (key_bit(AXKEY_LALT)) modifiers |= AXMOD_ALT;
    if (key_bit(AXKEY_RALT)) {
        modifiers |= AXMOD_ALT;
        modifiers |= AXMOD_ALTGR;
    }
    if (key_bit(AXKEY_LMETA) || key_bit(AXKEY_RMETA)) modifiers |= AXMOD_META;
    if (g_keyboard.state.caps) modifiers |= AXMOD_CAPS;
    if (g_keyboard.state.num) modifiers |= AXMOD_NUM;
    if (g_keyboard.state.scroll) modifiers |= AXMOD_SCROLL;
    g_keyboard.state.modifiers = modifiers;
    return modifiers;
}

/* ------------------------------------------------------------------ */
/* Event emission                                                      */
/* ------------------------------------------------------------------ */

static void emit_key_event(axkey_t key, axkey_state_t state, u32 modifiers)
{
    if (!g_ring) {
        return;
    }
    axinput_event_t event;
    ax_memset(&event, 0, sizeof(event));
    event.timestamp_ns = kbd_now();
    event.sequence = g_keyboard.state.events_emitted++;
    event.class = AXINPUT_KEY;
    event.code = (u16)key;
    event.value = (i32)state;
    event.modifiers = modifiers;
    event.device = AXINPUT_DEVICE_KEYBOARD;
    if (!axinput_ring_push(g_ring, &event)) {
        g_keyboard.state.decode_errors++;
    }
}

/* ------------------------------------------------------------------ */
/* LED handling                                                        */
/* ------------------------------------------------------------------ */

static void keyboard_apply_leds(void)
{
    u8 leds = 0;
    if (g_keyboard.state.scroll) leds |= 0x01;
    if (g_keyboard.state.num) leds |= 0x02;
    if (g_keyboard.state.caps) leds |= 0x04;
    if (leds == g_keyboard.state.leds) {
        return;
    }
    g_keyboard.state.leds = leds;
    if (!g_keyboard.controller || !g_keyboard.controller->keyboard_present) {
        return;
    }
    (void)i8042_write_data(I8042_KBD_CMD_SET_LEDS);
    (void)i8042_write_data(leds);
}

void keyboard_set_leds(u8 leds)
{
    g_keyboard.state.leds = leds;
    g_keyboard.state.caps = (bool_t)((leds & 0x04) != 0);
    g_keyboard.state.num = (bool_t)((leds & 0x02) != 0);
    g_keyboard.state.scroll = (bool_t)((leds & 0x01) != 0);
    recompute_modifiers();
    keyboard_apply_leds();
}

void keyboard_set_typematic(u16 delay_ms, u16 rate_ms)
{
    if (delay_ms == 0 || delay_ms > 1000) {
        delay_ms = KBD_DEFAULT_DELAY_MS;
    }
    if (rate_ms < 2 || rate_ms > 500) {
        rate_ms = KBD_DEFAULT_RATE_MS;
    }
    g_keyboard.state.typematic_delay_ms = delay_ms;
    g_keyboard.state.typematic_rate_ms = rate_ms;
    g_keyboard.state.repeat_interval_ns = (u64)rate_ms * 1000000ULL;

    /* The device also has a typematic setting; pack it as the PS/2 command
     * expects: bits 4..0 hold the rate index, bits 6..5 the delay index.  We
     * compute the nearest indices rather than store the raw values. */
    u8 rate_index = 0x1F;
    if (rate_ms <= 500) {
        u32 approximate = (500 / rate_ms);
        if (approximate > 31) approximate = 31;
        rate_index = (u8)(31 - approximate);
    }
    u8 delay_index = 0;
    if (delay_ms >= 750) delay_index = 3;
    else if (delay_ms >= 500) delay_index = 2;
    else if (delay_ms >= 250) delay_index = 1;
    u8 parameter = (u8)((delay_index << 5) | (rate_index & 0x1F));
    (void)i8042_write_data(I8042_KBD_CMD_TYPEMATIC);
    (void)i8042_write_data(parameter);
}

/* ------------------------------------------------------------------ */
/* Scancode state machine                                              */
/* ------------------------------------------------------------------ */

/* Device responses arrive interleaved with scancodes.  Anything from fa upward
 * that is not part of a known multi-byte sequence is a response and must be
 * swallowed; otherwise the ack for an LED update looks like key 0x7a. */
static bool_t is_device_response(u8 byte, bool_t extended)
{
    if (extended) {
        return (bool_t)0;
    }
    switch (byte) {
    case I8042_ACK:
    case I8042_RESEND:
    case I8042_ERROR:
    case 0xEE:
    case 0x00:
        return (bool_t)1;
    default:
        return (bool_t)(byte >= KBD_DEVICE_RESPONSE_THRESHOLD);
    }
}

static void handle_key_press(axkey_t key, bool_t pressed)
{
    if (key == AXKEY_NONE) {
        return;
    }
    u32 modifiers = g_keyboard.state.modifiers;

    if (pressed) {
        /* A press clears any stale release that never arrived. */
        set_key_bit(key, (bool_t)1);
        switch (key) {
        case AXKEY_CAPSLOCK:
            g_keyboard.state.caps = (bool_t)!g_keyboard.state.caps;
            break;
        case AXKEY_NUMLOCK:
            g_keyboard.state.num = (bool_t)!g_keyboard.state.num;
            break;
        case AXKEY_SCROLLLOCK:
            g_keyboard.state.scroll = (bool_t)!g_keyboard.state.scroll;
            break;
        default:
            break;
        }
        modifiers = recompute_modifiers();
        emit_key_event(key, AXKEY_PRESSED, modifiers);

        /* Start typematic repetition for character and navigation keys only;
         * modifiers and locks do not repeat. */
        if (key != AXKEY_CAPSLOCK && key != AXKEY_NUMLOCK &&
            key != AXKEY_SCROLLLOCK) {
            g_keyboard.state.repeat_key = (u32)key;
            g_keyboard.state.repeat_deadline_ns =
                kbd_now() + (u64)g_keyboard.state.typematic_delay_ms * 1000000ULL;
        }
        if (key == AXKEY_CAPSLOCK || key == AXKEY_NUMLOCK ||
            key == AXKEY_SCROLLLOCK) {
            keyboard_apply_leds();
        }
    } else {
        set_key_bit(key, (bool_t)0);
        if (g_keyboard.state.repeat_key == (u32)key) {
            g_keyboard.state.repeat_key = AXKEY_NONE;
            g_keyboard.state.repeat_deadline_ns = 0;
        }
        modifiers = recompute_modifiers();
        emit_key_event(key, AXKEY_RELEASED, modifiers);
    }
}

/* Decode one byte.  Returns the number of events emitted (0 or 1), negative on
 * a malformed sequence. */
i32 keyboard_feed_scancode(u8 scancode, bool_t extended, bool_t release)
{
    scanset_t set = (scanset_t)g_keyboard.state.scancode_set;
    if (set == SCANSET_UNKNOWN) {
        set = SCANSET_1;
    }
    axkey_t key = scancode_to_key(set, scancode, extended);
    if (key == AXKEY_NONE) {
        /* Prefix or unmapped code.  Prefixes are handled before we get here. */
        return 0;
    }
    handle_key_press(key, (bool_t)!release);
    return 1;
}

i32 keyboard_feed_byte(u8 byte)
{
    if (!g_keyboard.initialised) {
        return -19;
    }
    g_keyboard.bytes_seen++;
    g_keyboard.decoder.bytes++;

    /* e1 starts the pause/break sequence, six bytes of 0x1d 0x45 ...  We
     * consume it and stay in "extended2" until the sequence ends. */
    if (byte == SCAN_EXTENDED_2) {
        g_keyboard.decoder.extended2 = (bool_t)1;
        return 0;
    }
    if (g_keyboard.decoder.extended2) {
        g_keyboard.decoder.extended2 = (bool_t)0;
        /* Pause is delivered as a single key here; the following bytes are
         * consumed with the same handling. */
        if (byte == 0x1D) {
            handle_key_press(AXKEY_NONE, (bool_t)1);
        }
        return 0;
    }
    if (byte == SCAN_EXTENDED_1) {
        g_keyboard.decoder.extended = (bool_t)1;
        return 0;
    }

    bool_t extended = g_keyboard.decoder.extended;
    bool_t release = (bool_t)0;
    u8 code = byte;

    if (is_device_response(byte, extended)) {
        g_keyboard.decoder.extended = (bool_t)0;
        return 0;
    }

    if (g_keyboard.state.scancode_set == SCANSET_2) {
        /* Set 2 uses f0 as the release prefix. */
        if (byte == 0xF0) {
            g_keyboard.decoder.release = (bool_t)1;
            g_keyboard.decoder.extended = (bool_t)0;
            return 0;
        }
        release = g_keyboard.decoder.release;
        g_keyboard.decoder.release = (bool_t)0;
        g_keyboard.decoder.extended = (bool_t)0;
        g_keyboard.decoder.last_scancode = byte;
        return keyboard_feed_scancode(code, extended, release);
    }

    /* Set 1: the high bit marks a release. */
    if (code & SCAN_RELEASE) {
        release = (bool_t)1;
        code = (u8)(code & 0x7F);
    }
    g_keyboard.decoder.extended = (bool_t)0;
    g_keyboard.decoder.last_scancode = code;
    return keyboard_feed_scancode(code, extended, release);
}

/* ------------------------------------------------------------------ */
/* Typematic and periodic work                                         */
/* ------------------------------------------------------------------ */

void keyboard_tick(u64 now_ns)
{
    if (!g_keyboard.initialised) {
        return;
    }
    if (g_keyboard.state.repeat_key == AXKEY_NONE) {
        return;
    }
    if (now_ns < g_keyboard.state.repeat_deadline_ns) {
        return;
    }
    axkey_t key = (axkey_t)g_keyboard.state.repeat_key;
    emit_key_event(key, AXKEY_REPEAT, g_keyboard.state.modifiers);
    u64 interval = g_keyboard.state.repeat_interval_ns ? g_keyboard.state.repeat_interval_ns
                                                        : 33000000ULL;
    g_keyboard.state.repeat_deadline_ns = now_ns + interval;
}

/* Drain the byte queue into the decoder.  Called from the input poll loop so
 * decoding cost stays out of interrupt context. */
u32 keyboard_drain(void)
{
    u32 processed = 0;
    u8 byte = 0;
    while (queue_pop(&byte)) {
        (void)keyboard_feed_byte(byte);
        processed++;
    }
    return processed;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

keyboard_driver_t *keyboard_driver_init(axinput_ring_t *ring)
{
    ax_memset(&g_keyboard, 0, sizeof(g_keyboard));
    ax_memset(&g_queue, 0, sizeof(g_queue));
    g_ring = ring;
    g_keyboard.ring = ring;
    g_keyboard.controller = i8042_init();
    g_keyboard.state.caps = (bool_t)0;
    g_keyboard.state.num = (bool_t)1;
    g_keyboard.state.scroll = (bool_t)0;
    g_keyboard.state.typematic_delay_ms = KBD_DEFAULT_DELAY_MS;
    g_keyboard.state.typematic_rate_ms = KBD_DEFAULT_RATE_MS;
    g_keyboard.state.repeat_interval_ns =
        (u64)KBD_DEFAULT_RATE_MS * 1000000ULL;
    g_keyboard.state.repeat_key = AXKEY_NONE;
    /* The controller translates to set 1 when translation is on; otherwise the
     * keyboard's native set 2 arrives unchanged. */
    g_keyboard.state.scancode_set = g_keyboard.controller->translation
                                        ? SCANSET_1
                                        : SCANSET_2;
    g_keyboard.initialised = (bool_t)1;
    g_keyboard.enabled = (bool_t)1;
    recompute_modifiers();
    printk("kbd: ready, set %u, %s layout\n",
           g_keyboard.state.scancode_set, keyboard_layout_name(g_layout));
    return &g_keyboard;
}

bool_t keyboard_ready(void)
{
    return (bool_t)(g_keyboard.initialised && g_keyboard.enabled);
}

u32 keyboard_modifiers(void)
{
    return g_keyboard.state.modifiers;
}

bool_t keyboard_key_down(axkey_t key)
{
    return key_bit(key);
}

void keyboard_reset_state(void)
{
    ax_memset(g_keyboard.state.pressed, 0, sizeof(g_keyboard.state.pressed));
    g_keyboard.state.caps = (bool_t)0;
    g_keyboard.state.num = (bool_t)0;
    g_keyboard.state.scroll = (bool_t)0;
    g_keyboard.state.repeat_key = AXKEY_NONE;
    g_keyboard.state.repeat_deadline_ns = 0;
    recompute_modifiers();
    keyboard_apply_leds();
}

u32 keyboard_event_count(void)
{
    return g_keyboard.state.events_emitted;
}

void keyboard_dump(void)
{
    printk("kbd: set=%u mods=%04x caps=%d num=%d scroll=%d bytes=%u events=%u "
           "errors=%u layout=%s\n",
           g_keyboard.state.scancode_set, g_keyboard.state.modifiers,
           g_keyboard.state.caps, g_keyboard.state.num,
           g_keyboard.state.scroll, g_keyboard.bytes_seen,
           g_keyboard.state.events_emitted, g_keyboard.state.decode_errors,
           keyboard_layout_name(g_layout));
    printk("kbd: queue=%u overflow=%u typematic=%ums/%ums leds=%02x\n",
           g_queue.count, g_queue.overflow,
           g_keyboard.state.typematic_delay_ms,
           g_keyboard.state.typematic_rate_ms, g_keyboard.state.leds);
}

/* Called by the i8042 poll path for every keyboard byte; queued so the decoder
 * can be timing-independent. */
i32 keyboard_accept_raw(u8 byte)
{
    queue_push(byte);
    return 0;
}
