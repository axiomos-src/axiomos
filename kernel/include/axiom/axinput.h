/*
 * AxiomOS - input subsystem: PS/2 controller, keyboard and mouse.
 *
 * Three layers, each in its own translation unit:
 *
 *   i8042   the controller: ports 0x60/0x64, command and data queues,
 *           self-test, configuration byte, IRQ routing;
 *   keyboard  scancode sets 1 and 2, modifier state, typematic repeat, LEDs;
 *   mouse  packet assembly, scaling, acceleration, button state.
 *
 * Everything below produces `axinput_event_t` records into a single ring, which
 * the GUI reads once per frame.  Nothing in the drivers blocks: the interrupt
 * handler pushes bytes into a small queue and the decode runs either inline
 * (when the queue is short) or in the polling path the GUI already calls.
 *
 * Scancode conventions: set 1 is what the firmware leaves a PS/2 keyboard in on
 * this platform; set 2 is the native set, translated by the controller when
 * configured that way.  We support both explicitly because QEMU and VMware
 * differ in what they hand over.
 */

#ifndef AXIOM_AXINPUT_H
#define AXIOM_AXINPUT_H

#include <axiom/types.h>

/* ------------------------------------------------------------------ */
/* Key codes                                                           */
/* ------------------------------------------------------------------ */

/* A stable, layout-independent code per physical key.  Values follow the Linux
 * input event codes in the range we use so tooling can compare. */
typedef enum axkey {
    AXKEY_NONE = 0,
    AXKEY_ESC = 1,
    AXKEY_1 = 2, AXKEY_2, AXKEY_3, AXKEY_4, AXKEY_5,
    AXKEY_6, AXKEY_7, AXKEY_8, AXKEY_9, AXKEY_0,
    AXKEY_MINUS, AXKEY_EQUAL, AXKEY_BACKSPACE, AXKEY_TAB,
    AXKEY_Q = 16, AXKEY_W, AXKEY_E, AXKEY_R, AXKEY_T, AXKEY_Y,
    AXKEY_U, AXKEY_I, AXKEY_O, AXKEY_P, AXKEY_LBRACKET, AXKEY_RBRACKET,
    AXKEY_ENTER = 28, AXKEY_LCTRL,
    AXKEY_A = 30, AXKEY_S, AXKEY_D, AXKEY_F, AXKEY_G, AXKEY_H,
    AXKEY_I2, AXKEY_J, AXKEY_K, AXKEY_L, AXKEY_SEMICOLON, AXKEY_QUOTE,
    AXKEY_GRAVE, AXKEY_LSHIFT, AXKEY_BACKSLASH,
    AXKEY_Z = 45, AXKEY_X, AXKEY_C, AXKEY_V, AXKEY_B, AXKEY_N, AXKEY_M,
    AXKEY_COMMA, AXKEY_DOT, AXKEY_SLASH, AXKEY_RSHIFT,
    AXKEY_KP_STAR = 55, AXKEY_LALT, AXKEY_SPACE, AXKEY_CAPSLOCK,
    AXKEY_F1 = 59, AXKEY_F2, AXKEY_F3, AXKEY_F4, AXKEY_F5, AXKEY_F6,
    AXKEY_F7, AXKEY_F8, AXKEY_F9, AXKEY_F10,
    AXKEY_NUMLOCK = 69, AXKEY_SCROLLLOCK,
    AXKEY_KP_7 = 71, AXKEY_KP_8, AXKEY_KP_9, AXKEY_KP_MINUS,
    AXKEY_KP_4, AXKEY_KP_5, AXKEY_KP_6, AXKEY_KP_PLUS,
    AXKEY_KP_1, AXKEY_KP_2, AXKEY_KP_3, AXKEY_KP_0, AXKEY_KP_DOT,
    AXKEY_F11 = 87, AXKEY_F12,
    AXKEY_KP_ENTER = 96, AXKEY_RCTRL, AXKEY_KP_SLASH, AXKEY_PRINTSCREEN,
    AXKEY_RALT = 100, AXKEY_HOME = 102, AXKEY_UP, AXKEY_PAGEUP,
    AXKEY_LEFT, AXKEY_RIGHT, AXKEY_END, AXKEY_DOWN, AXKEY_PAGEDOWN,
    AXKEY_INSERT, AXKEY_DELETE,
    AXKEY_LMETA = 125, AXKEY_RMETA, AXKEY_MENU,
    AXKEY_MEDIA_MUTE = 113, AXKEY_MEDIA_VOLUMEDOWN, AXKEY_MEDIA_VOLUMEUP,
    AXKEY_POWER = 116, AXKEY_SLEEP, AXKEY_WAKE,
    AXKEY_BRIGHTNESS_DOWN = 224, AXKEY_BRIGHTNESS_UP,
    AXKEY_MAX = 256
} axkey_t;

typedef enum axkey_state {
    AXKEY_RELEASED = 0,
    AXKEY_PRESSED = 1,
    AXKEY_REPEAT = 2
} axkey_state_t;

/* Modifier bits, OR-able. */
#define AXMOD_LSHIFT   0x0001
#define AXMOD_RSHIFT   0x0002
#define AXMOD_CTRL     0x0004
#define AXMOD_ALT      0x0008
#define AXMOD_META     0x0010
#define AXMOD_CAPS     0x0020
#define AXMOD_NUM      0x0040
#define AXMOD_SCROLL   0x0080
#define AXMOD_ALTGR    0x0100
#define AXMOD_ANY_SHIFT (AXMOD_LSHIFT | AXMOD_RSHIFT)

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

typedef enum axinput_class {
    AXINPUT_KEY = 1,
    AXINPUT_MOUSE_MOVE,
    AXINPUT_MOUSE_BUTTON,
    AXINPUT_MOUSE_WHEEL,
    AXINPUT_HOTKEY,
    AXINPUT_SYNC
} axinput_class_t;

typedef enum axmouse_button {
    AXMOUSE_LEFT = 0x01,
    AXMOUSE_RIGHT = 0x02,
    AXMOUSE_MIDDLE = 0x04,
    AXMOUSE_BUTTON4 = 0x08,
    AXMOUSE_BUTTON5 = 0x10
} axmouse_button_t;

typedef struct axinput_event {
    u64  timestamp_ns;
    u32  sequence;
    u16  class;
    u16  code;              /* key code or button mask */
    i32  value;             /* 1/0/repeat, wheel delta */
    i32  x;
    i32  y;
    u32  modifiers;
    u8   device;
} axinput_event_t;

#define AXINPUT_DEVICE_KEYBOARD 1
#define AXINPUT_DEVICE_MOUSE    2

/* ------------------------------------------------------------------ */
/* Ring buffer                                                         */
/* ------------------------------------------------------------------ */

#define AXINPUT_RING_SIZE 256

typedef struct axinput_ring {
    axinput_event_t events[AXINPUT_RING_SIZE];
    u32 head;               /* producer index */
    u32 tail;               /* consumer index */
    u32 dropped;
    u32 high_water;
    u64 pushed;
    u64 popped;
} axinput_ring_t;

void axinput_ring_init(axinput_ring_t *ring);
bool_t axinput_ring_push(axinput_ring_t *ring, const axinput_event_t *event);
bool_t axinput_ring_pop(axinput_ring_t *ring, axinput_event_t *out);
bool_t axinput_ring_peek(const axinput_ring_t *ring, axinput_event_t *out);
u32 axinput_ring_count(const axinput_ring_t *ring);
void axinput_ring_flush(axinput_ring_t *ring);
void axinput_ring_stats(const axinput_ring_t *ring, char *buffer, usize cap);
const char *axinput_class_name(u16 event_class);
void axinput_modifier_string(u32 modifiers, char *buffer, usize cap);

/* ------------------------------------------------------------------ */
/* i8042 controller                                                    */
/* ------------------------------------------------------------------ */

#define I8042_DATA_PORT   0x60
#define I8042_STATUS_PORT 0x64
#define I8042_COMMAND_PORT 0x64

#define I8042_STATUS_OUTPUT_FULL 0x01
#define I8042_STATUS_INPUT_FULL  0x02
#define I8042_STATUS_AUX_DATA    0x20
#define I8042_STATUS_TIMEOUT     0x40
#define I8042_STATUS_PARITY      0x80

#define I8042_CMD_READ_CONFIG     0x20
#define I8042_CMD_WRITE_CONFIG    0x60
#define I8042_CMD_DISABLE_AUX     0xA7
#define I8042_CMD_ENABLE_AUX      0xA8
#define I8042_CMD_TEST_AUX        0xA9
#define I8042_CMD_SELF_TEST       0xAA
#define I8042_CMD_TEST_FIRST      0xAB
#define I8042_CMD_DISABLE_FIRST   0xAD
#define I8042_CMD_ENABLE_FIRST    0xAE
#define I8042_CMD_READ_INPUT      0xC1
#define I8042_CMD_READ_OUTPUT     0xD0
#define I8042_CMD_WRITE_OUTPUT    0xD1
#define I8042_CMD_WRITE_AUX       0xD4

#define I8042_CONFIG_FIRST_IRQ    0x01
#define I8042_CONFIG_AUX_IRQ      0x02
#define I8042_CONFIG_TRANSLATE    0x40
#define I8042_CONFIG_CLOCK1       0x10
#define I8042_CONFIG_CLOCK2       0x20

#define I8042_SELF_TEST_OK        0x55
#define I8042_AUX_TEST_OK         0x00

#define I8042_KBD_CMD_SET_LEDS    0xED
#define I8042_KBD_CMD_ECHO        0xEE
#define I8042_KBD_CMD_SET_SCAN    0xF0
#define I8042_KBD_CMD_IDENTIFY    0xF2
#define I8042_KBD_CMD_TYPEMATIC   0xF3
#define I8042_KBD_CMD_ENABLE      0xF4
#define I8042_KBD_CMD_DISABLE     0xF5
#define I8042_KBD_CMD_RESET       0xFF

#define I8042_MOUSE_CMD_SET_SCALE11 0xE6
#define I8042_MOUSE_CMD_SET_SCALE21 0xE7
#define I8042_MOUSE_CMD_SET_RES     0xE8
#define I8042_MOUSE_CMD_GET_DEVICE  0xF2
#define I8042_MOUSE_CMD_SET_SAMPLE  0xF3
#define I8042_MOUSE_CMD_ENABLE_DATA 0xF4
#define I8042_MOUSE_CMD_RESET       0xFF

#define I8042_ACK  0xFA
#define I8042_RESEND 0xFE
#define I8042_ERROR 0xFC

typedef enum i8042_state {
    I8042_STATE_UNINITIALISED = 0,
    I8042_STATE_READY,
    I8042_STATE_DEGRADED,
    I8042_STATE_ABSENT
} i8042_state_t;

typedef struct i8042_stats {
    u32 commands;
    u32 reads;
    u32 writes;
    u32 timeouts;
    u32 resends;
    u32 keyboard_bytes;
    u32 mouse_bytes;
    u32 spurious;
} i8042_stats_t;

typedef struct i8042_controller {
    u8  config;
    u8  status;
    u8  keyboard_id[2];
    u8  mouse_id;
    bool_t keyboard_present;
    bool_t mouse_present;
    bool_t mouse_wheel;
    bool_t mouse_5button;
    bool_t translation;
    u32  poll_interval_us;
    i8042_state_t state;
    i8042_stats_t stats;
} i8042_controller_t;

i8042_controller_t *i8042_init(void);
bool_t i8042_ready(void);
i8042_state_t i8042_state(void);
i32  i8042_wait_write(void);
i32  i8042_wait_read(void);
i32  i8042_write_command(u8 command);
i32  i8042_write_data(u8 data);
i32  i8042_read_data(u8 *out);
i32  i8042_read_status(u8 *out);
i32  i8042_command(u8 command, u8 *parameter);
i32  i8042_write_aux(u8 data);
i32  i8042_read_aux(u8 *out);
bool_t i8042_has_output(void);
bool_t i8042_has_input(void);
u32  i8042_poll(u32 budget);
void i8042_dump(void);
void i8042_reset_stats(void);

/* ------------------------------------------------------------------ */
/* Scancodes                                                           */
/* ------------------------------------------------------------------ */

#define SCAN_EXTENDED_1 0xE0
#define SCAN_EXTENDED_2 0xE1
#define SCAN_RELEASE    0x80

typedef enum scanset {
    SCANSET_1 = 0,
    SCANSET_2,
    SCANSET_UNKNOWN
} scanset_t;

/* The keyboard driver keeps a small state machine per byte stream. */
typedef struct kbd_decoder {
    u8  buffer[8];
    u32 buffer_len;
    bool_t extended;
    bool_t extended2;
    bool_t release;
    u32 dropped;
    u32 bytes;
    u32 events;
    u8  last_scancode;
} kbd_decoder_t;

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

#define AXXLATE_KBD_STATE_IDLE 0
#define AXXLATE_KBD_STATE_LED  1
#define AXXLATE_KBD_STATE_SCAN 2

typedef struct keyboard_state {
    u32 modifiers;
    bool_t caps;
    bool_t num;
    bool_t scroll;
    u16  typematic_delay_ms;
    u16  typematic_rate_ms;
    u32  repeat_key;
    u64  repeat_deadline_ns;
    u64  repeat_interval_ns;
    u8   leds;
    u32  pressed[AXKEY_MAX / 32];
    u32  scancode_set;
    u32  decode_errors;
    u32  events_emitted;
} keyboard_state_t;

typedef struct keyboard_driver {
    i8042_controller_t *controller;
    keyboard_state_t state;
    kbd_decoder_t decoder;
    axinput_ring_t *ring;
    bool_t initialised;
    bool_t enabled;
    u32 irq_count;
    u32 bytes_seen;
} keyboard_driver_t;

keyboard_driver_t *keyboard_driver_init(axinput_ring_t *ring);
bool_t keyboard_ready(void);
i32 keyboard_feed_byte(u8 byte);
i32 keyboard_feed_scancode(u8 scancode, bool_t extended, bool_t release);
void keyboard_tick(u64 now_ns);
u32 keyboard_modifiers(void);
bool_t keyboard_key_down(axkey_t key);
void keyboard_set_leds(u8 leds);
void keyboard_set_typematic(u16 delay_ms, u16 rate_ms);
void keyboard_reset_state(void);
void keyboard_dump(void);
u32 keyboard_event_count(void);

/* Translate one scancode into a key code.  Returns AXKEY_NONE for prefixes and
 * unknown bytes. */
axkey_t scancode_to_key(scanset_t set, u8 scancode, bool_t extended);
bool_t scancode_known(scanset_t set, u8 scancode, bool_t extended);
const char *key_name(axkey_t key);
const char *key_legend(axkey_t key, u32 modifiers);
u32 keymap_scancode_table_size(void);
u32 keymap_keycode_count(void);
u32 keyboard_drain(void);
i32 keyboard_accept_raw(u8 byte);
void keyboard_set_layout(u8 layout);
const char *keyboard_layout_name(u8 layout);

/* Layout translation: key plus modifiers to a character.  Returns 0 when the
 * key produces no character (function keys, modifiers). */
char key_to_ascii(axkey_t key, u32 modifiers);
char key_to_ascii_layout(axkey_t key, u32 modifiers, u8 layout);
u32  key_to_unicode(axkey_t key, u32 modifiers);

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

#define AXMOUSE_MAX_X 4096
#define AXMOUSE_MAX_Y 4096
#define AXMOUSE_PACKET_SIZE 4

typedef enum mouse_protocol {
    MOUSE_PROTO_PS2 = 1,
    MOUSE_PROTO_IMPS2,
    MOUSE_PROTO_IMPS2_5BUTTON,
    MOUSE_PROTO_UNKNOWN
} mouse_protocol_t;

typedef struct mouse_state {
    i32 x;
    i32 y;
    i32 max_x;
    i32 max_y;
    u32 buttons;
    i32 wheel_accum;
    i32 movement_accum_x;
    i32 movement_accum_y;
    u32 packets;
    u32 resyncs;
    u32 bad_bytes;
    u8  packet[AXMOUSE_PACKET_SIZE];
    u32 packet_len;
    u32 expected_len;
    mouse_protocol_t protocol;
    u8  sample_rate;
    u8  resolution;
    bool_t scaling;
    bool_t enabled;
    u32 led_state;
} mouse_state_t;

typedef struct mouse_driver {
    i8042_controller_t *controller;
    mouse_state_t state;
    axinput_ring_t *ring;
    bool_t initialised;
    u32 irq_count;
    u32 bytes_seen;
} mouse_driver_t;

mouse_driver_t *mouse_init(axinput_ring_t *ring);
bool_t mouse_ready(void);
i32 mouse_feed_byte(u8 byte);
i32 mouse_process_packet(const u8 *packet, u32 length, mouse_protocol_t protocol);
void mouse_set_bounds(i32 width, i32 height);
void mouse_get_position(i32 *x, i32 *y);
u32 axmouse_buttons(void);
void mouse_set_speed(u8 sample_rate, u8 resolution);
void mouse_reset(void);
void mouse_dump(void);
void mouse_emulate_move(i32 dx, i32 dy);
i32 mouse_accelerate(i32 delta, u32 speed_factor);

/* ------------------------------------------------------------------ */
/* Hotkeys                                                             */
/* ------------------------------------------------------------------ */

#define AXHOTKEY_MAX 16

typedef void (*axhotkey_fn)(void *context);

typedef struct axhotkey {
    u32  key;
    u32  required_mods;
    u32  forbidden_mods;
    axhotkey_fn handler;
    void *context;
    char name[24];
    u32  hits;
    bool_t active;
} axhotkey_t;

typedef struct axhotkey_table {
    axhotkey_t entries[AXHOTKEY_MAX];
    u32 count;
    u32 matches;
} axhotkey_table_t;

void axhotkey_init(axhotkey_table_t *table);
i32  axhotkey_register(axhotkey_table_t *table, const char *name, u32 key,
                       u32 required, u32 forbidden, axhotkey_fn handler,
                       void *context);
bool_t axhotkey_dispatch(axhotkey_table_t *table, const axinput_event_t *event);
i32  axhotkey_unregister(axhotkey_table_t *table, const char *name);
i32  axhotkey_set_active(axhotkey_table_t *table, const char *name, bool_t active);
void axhotkey_dump(const axhotkey_table_t *table);

/* ------------------------------------------------------------------ */
/* Subsystem entry points                                              */
/* ------------------------------------------------------------------ */

void axinput_init(void);
bool_t axinput_ready(void);
axinput_ring_t *axinput_events(void);
void axinput_drain(void);
void axinput_dump_state(void);
i32  axinput_selftest(void);

/* Key injection for tests and for the shell's `input inject` command. */
i32 axinput_inject_key(axkey_t key, bool_t pressed);
i32 axinput_inject_mouse(i32 dx, i32 dy, u32 buttons, i32 wheel);

/* Text composition (kbd_text.c). */
void kbdtext_init(void);
void kbdtext_reset(void);
u32  kbdtext_key(axkey_t key, u32 modifiers, u32 *out, u32 out_cap, u64 now_ns);
u32  kbdtext_compose_key(u32 code);
u32  utf8_encode(u32 code, u8 *out, u32 cap);
u32  utf8_decode(const u8 *data, u32 length, u32 *code);
void kbdtext_dump(void);

/* Gestures (mouse_gestures.c).  Opaque structs are mirrored there; callers use
 * the accessors rather than the layout. */
void mouse_gestures_init(void);
void mouse_gestures_dump(void);
bool_t mouse_gestures_dragging(void);
u32  mouse_gesture_count(void);
u32  mouse_gestures_drag_distance(void);

/* History and macros (input_history.c). */
void input_history_init(void);
void input_history_record(const axinput_event_t *event);
u32  input_history_count(void);
void input_history_dump(u32 limit);
void input_history_stats(void);
void input_macro_list(void);

/* Shell entry point (input_diag.c). */
i32 input_command(u32 argc, const char **argv);
u32 input_pending_class(u16 event_class);

/* Extended tables (scancode_tables.c). */
axkey_t scancode_set3_lookup(u8 code, bool_t *is_break);
const char *scanset_name(scanset_t set);
u32 scancode_table_coverage(scanset_t set);
u16 typematic_rate_ms(u8 index);
u16 typematic_delay_for(u8 index);
u8  typematic_index_for(u16 rate_ms);
u8  typematic_delay_index_for(u16 delay_ms);
u8  typematic_encode(u8 rate_index, u8 delay_index);
void typematic_decode(u8 parameter, u8 *rate_index, u8 *delay_index);
u32 led_pattern_count(void);
const char *led_pattern_name(u32 index);
u8  led_pattern_mask(u32 index);
void scancode_tables_dump(void);

#endif /* AXIOM_AXINPUT_H */
