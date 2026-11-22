/*
 * AxiomOS - input subsystem entry point.
 *
 * Ties the three drivers together: one ring, one controller, one hotkey table,
 * and a single `axinput_drain` that the GUI calls once per frame.  Keeping the
 * drain in one place means the ordering between keyboard, mouse and repeat
 * events is deterministic: bytes are consumed in controller order, then the
 * typematic tick runs, then hotkeys are considered for each queued key press.
 *
 * The selftest at the bottom is deliberately written against the public API
 * only.  It feeds synthetic scancodes through the same decoder the hardware
 * uses, so it catches table errors as well as state-machine errors -- a
 * hand-written test that poked internal state would pass even if the tables
 * were wrong.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>
#include <axiom/axinput.h>

static axinput_ring_t g_ring;
static axinput_ring_t g_hotkey_ring;   /* copy of key events for chord matching */
static axhotkey_table_t g_hotkeys;
static bool_t g_initialised;
static u32 g_frame;

/* ------------------------------------------------------------------ */
/* Built-in hotkeys                                                    */
/* ------------------------------------------------------------------ */

static void hotkey_ctrl_alt_del(void *context)
{
    (void)context;
    printk("hotkey: Ctrl-Alt-Del pressed\n");
    /* A real secure attention sequence would reset to the login surface; here
     * it clears the screen through the console hook if one is registered. */
    extern void console_clear(void) __attribute__((weak));
    if (console_clear) {
        console_clear();
    }
}

static void hotkey_sysrq(void *context)
{
    (void)context;
    printk("hotkey: SysRq -- dumping input state\n");
    axinput_dump_state();
}

static void hotkey_debug_overlay(void *context)
{
    (void)context;
    extern void gui_debug_overlay_toggle(void) __attribute__((weak));
    if (gui_debug_overlay_toggle) {
        gui_debug_overlay_toggle();
    } else {
        printk("hotkey: debug overlay (no GUI hook)\n");
    }
}

static void register_defaults(void)
{
    axhotkey_init(&g_hotkeys);
    (void)axhotkey_register(&g_hotkeys, "ctrl-alt-del", AXKEY_DELETE,
                            AXMOD_CTRL | AXMOD_ALT, AXMOD_ANY_SHIFT,
                            hotkey_ctrl_alt_del, 0);
    (void)axhotkey_register(&g_hotkeys, "sysrq", AXKEY_PRINTSCREEN, 0, 0,
                            hotkey_sysrq, 0);
    (void)axhotkey_register(&g_hotkeys, "debug-overlay", AXKEY_GRAVE,
                            AXMOD_CTRL | AXMOD_ALT, AXMOD_ANY_SHIFT,
                            hotkey_debug_overlay, 0);
}

/* ------------------------------------------------------------------ */
/* Public state                                                        */
/* ------------------------------------------------------------------ */

axinput_ring_t *axinput_events(void)
{
    return &g_ring;
}

bool_t axinput_ready(void)
{
    return g_initialised;
}

void axinput_init(void)
{
    axinput_ring_init(&g_ring);
    axinput_ring_init(&g_hotkey_ring);

    i8042_controller_t *controller = i8042_init();
    if (!controller) {
        g_initialised = (bool_t)0;
        return;
    }

    (void)keyboard_driver_init(&g_ring);
    (void)mouse_init(&g_ring);
    register_defaults();

    g_initialised = (bool_t)1;
    printk("input: initialised, controller state %u\n", (u32)i8042_state());
}

/* Consume pending hardware bytes and let time-based work run.  `budget` bounds
 * how many controller bytes one frame may consume so a stuck device cannot
 * starve the compositor. */
void axinput_drain(void)
{
    if (!g_initialised) {
        return;
    }
    u32 consumed = i8042_poll(32);
    (void)consumed;
    keyboard_tick(axbpf_now_ns());
    g_frame++;

    /* Replay key presses through the hotkey table without disturbing the ring
     * the GUI consumes: the copy is drained independently. */
    axinput_event_t event;
    while (axinput_ring_pop(&g_hotkey_ring, &event)) {
        if (event.class == AXINPUT_KEY && event.value != 0) {
            (void)axhotkey_dispatch(&g_hotkeys, &event);
        }
    }
}

void axinput_dump_state(void)
{
    printk("input: frame=%u ready=%d ring=%u\n", g_frame, g_initialised,
           axinput_ring_count(&g_ring));
    i8042_dump();
    keyboard_dump();
    mouse_dump();
    axhotkey_dump(&g_hotkeys);
}

/* ------------------------------------------------------------------ */
/* Injection                                                           */
/* ------------------------------------------------------------------ */

/* Queue a synthetic key event.  This is the path the on-screen keyboard and the
 * selftest use, and it deliberately bypasses the scancode decoder but not the
 * ring, so consumers cannot tell injected from physical events except by
 * timestamp. */
i32 axinput_inject_key(axkey_t key, bool_t pressed)
{
    axinput_event_t event;
    ax_memset(&event, 0, sizeof(event));
    event.timestamp_ns = axbpf_now_ns();
    event.sequence = g_frame;
    event.class = AXINPUT_KEY;
    event.code = (u16)key;
    event.value = pressed ? (i32)AXKEY_PRESSED : (i32)AXKEY_RELEASED;
    event.modifiers = keyboard_modifiers();
    event.device = AXINPUT_DEVICE_KEYBOARD;
    if (!axinput_ring_push(&g_ring, &event)) {
        return -28;
    }
    if (pressed) {
        (void)axinput_ring_push(&g_hotkey_ring, &event);
    }
    return 0;
}

i32 axinput_inject_mouse(i32 dx, i32 dy, u32 buttons, i32 wheel)
{
    extern void mouse_emulate_move(i32 dx, i32 dy);
    axinput_event_t event;
    ax_memset(&event, 0, sizeof(event));
    event.timestamp_ns = axbpf_now_ns();
    event.class = AXINPUT_MOUSE_MOVE;
    event.x = dx;
    event.y = dy;
    event.modifiers = buttons;
    event.device = AXINPUT_DEVICE_MOUSE;
    if (!axinput_ring_push(&g_ring, &event)) {
        return -28;
    }
    if (dx || dy) {
        mouse_emulate_move(dx, dy);
    }
    if (wheel) {
        ax_memset(&event, 0, sizeof(event));
        event.timestamp_ns = axbpf_now_ns();
        event.class = AXINPUT_MOUSE_WHEEL;
        event.value = wheel;
        event.device = AXINPUT_DEVICE_MOUSE;
        (void)axinput_ring_push(&g_ring, &event);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Selftest                                                            */
/* ------------------------------------------------------------------ */

static u32 g_selftest_failures;
static u32 g_selftest_checks;

static void check_true(const char *what, bool_t condition)
{
    g_selftest_checks++;
    if (!condition) {
        g_selftest_failures++;
        printk("input selftest: FAIL %s\n", what);
    }
}

static void check_key(const char *what, u8 scancode, bool_t extended, axkey_t want)
{
    axkey_t got = scancode_to_key(SCANSET_1, scancode, extended);
    g_selftest_checks++;
    if (got != want) {
        g_selftest_failures++;
        printk("input selftest: FAIL %s got %s want %s\n", what, key_name(got),
               key_name(want));
    }
}

/* Feed a scancode and check that exactly one key event lands in the ring with
 * the expected code and value. */
static void expect_key_event(const char *what, u8 scancode, bool_t extended,
                             bool_t release, axkey_t want, u32 want_mods)
{
    axinput_ring_flush(&g_ring);
    (void)keyboard_feed_scancode(scancode, extended, release);
    axinput_event_t event;
    g_selftest_checks++;
    if (!axinput_ring_pop(&g_ring, &event)) {
        g_selftest_failures++;
        printk("input selftest: FAIL %s (no event)\n", what);
        return;
    }
    if (event.code != (u16)want || event.value != (release ? 0 : 1)) {
        g_selftest_failures++;
        printk("input selftest: FAIL %s code=%u value=%d\n", what, event.code,
               event.value);
    }
    if (want_mods != 0xFFFFFFFFu && event.modifiers != want_mods) {
        g_selftest_failures++;
        printk("input selftest: FAIL %s mods=%04x want=%04x\n", what,
               event.modifiers, want_mods);
    }
}

static void test_tables(void)
{
    check_key("a", 0x1E, (bool_t)0, AXKEY_A);
    check_key("z", 0x2C, (bool_t)0, AXKEY_Z);
    check_key("enter", 0x1C, (bool_t)0, AXKEY_ENTER);
    check_key("kp-enter", 0x1C, (bool_t)1, AXKEY_KP_ENTER);
    check_key("left", 0x4B, (bool_t)1, AXKEY_LEFT);
    check_key("f12", 0x58, (bool_t)0, AXKEY_F12);
    check_key("set2-q", 0x10, (bool_t)0, AXKEY_Q);

    g_selftest_checks++;
    if (scancode_to_key(SCANSET_1, 0x10, (bool_t)0) != AXKEY_Q) {
        g_selftest_failures++;
        printk("input selftest: FAIL set1 q\n");
    }
    g_selftest_checks++;
    if (scancode_to_key(SCANSET_2, 0x10, (bool_t)0) != AXKEY_Q) {
        g_selftest_failures++;
        printk("input selftest: FAIL set2 q\n");
    }
}

static void test_ascii(void)
{
    check_true("ascii-a", (bool_t)(key_to_ascii_layout(AXKEY_A, 0, 0) == 'a'));
    check_true("ascii-shift-a",
               (bool_t)(key_to_ascii_layout(AXKEY_A, AXMOD_LSHIFT, 0) == 'A'));
    check_true("ascii-caps-a",
               (bool_t)(key_to_ascii_layout(AXKEY_A, AXMOD_CAPS, 0) == 'A'));
    check_true("ascii-shift-caps-a",
               (bool_t)(key_to_ascii_layout(AXKEY_A, AXMOD_CAPS | AXMOD_LSHIFT, 0) == 'a'));
    check_true("ascii-1", (bool_t)(key_to_ascii_layout(AXKEY_1, 0, 0) == '1'));
    check_true("ascii-shift-1",
               (bool_t)(key_to_ascii_layout(AXKEY_1, AXMOD_LSHIFT, 0) == '!'));
    check_true("ascii-slash",
               (bool_t)(key_to_ascii_layout(AXKEY_SLASH, 0, 0) == '/'));
    check_true("ascii-shift-slash",
               (bool_t)(key_to_ascii_layout(AXKEY_SLASH, AXMOD_LSHIFT, 0) == '?'));
    check_true("uk-quote", (bool_t)(key_to_ascii_layout(AXKEY_QUOTE, 0, 2) == '2'));
    check_true("de-y", (bool_t)(key_to_ascii_layout(AXKEY_Y, 0, 3) == 'z'));
    check_true("unicode-umlaut",
               (bool_t)(key_to_unicode(AXKEY_SEMICOLON, AXMOD_LSHIFT) == 0x00D6));
}

static void test_modifiers(void)
{
    expect_key_event("shift-down", 0x2A, (bool_t)0, (bool_t)0, AXKEY_LSHIFT,
                     AXMOD_LSHIFT);
    expect_key_event("a-with-shift", 0x1E, (bool_t)0, (bool_t)0, AXKEY_A,
                     AXMOD_LSHIFT);
    expect_key_event("shift-up", 0x2A, (bool_t)1, (bool_t)1, AXKEY_LSHIFT, 0);
    check_true("mods-clear", (bool_t)(keyboard_modifiers() == 0));
    check_true("no-shift-held", (bool_t)(keyboard_key_down(AXKEY_LSHIFT) == 0));
}

static void test_typematic(void)
{
    keyboard_set_typematic(250, 20);
    expect_key_event("repeat-start", 0x39, (bool_t)0, (bool_t)0, AXKEY_SPACE, 0);
    u64 now = axbpf_now_ns() + 300000000ULL;
    keyboard_tick(now);
    axinput_event_t event;
    g_selftest_checks++;
    if (!axinput_ring_pop(&g_ring, &event) || event.value != AXKEY_REPEAT) {
        g_selftest_failures++;
        printk("input selftest: FAIL typematic repeat\n");
    }
    /* Release so the repeat state does not leak into the next test. */
    (void)keyboard_feed_scancode(0x39, (bool_t)0, (bool_t)1);
}

static void test_mouse(void)
{
    g_selftest_checks++;
    if (mouse_accelerate(2, 1) != 2) {
        g_selftest_failures++;
        printk("input selftest: FAIL accel-slow\n");
    }
    g_selftest_checks++;
    if (mouse_accelerate(10, 1) != 15) {
        g_selftest_failures++;
        printk("input selftest: FAIL accel-fast\n");
    }

    u8 packet[4] = {0x08, 0x05, 0xFB, 0x00};
    g_selftest_checks++;
    if (mouse_process_packet(packet, 3, MOUSE_PROTO_PS2) != 0) {
        g_selftest_failures++;
        printk("input selftest: FAIL mouse packet rejected\n");
    }

    /* Negative delta with the sign bit: -3 means data 0xFD and bit 4 set. */
    u8 negative[4] = {0x18, 0xFD, 0x03, 0x00};
    g_selftest_checks++;
    if (mouse_process_packet(negative, 3, MOUSE_PROTO_PS2) != 0) {
        g_selftest_failures++;
        printk("input selftest: FAIL negative delta\n");
    }

    /* Wheel packet: fourth byte 0x01 means one notch down. */
    u8 wheel[4] = {0x08, 0x00, 0x00, 0x01};
    g_selftest_checks++;
    if (mouse_process_packet(wheel, 4, MOUSE_PROTO_IMPS2) != 0) {
        g_selftest_failures++;
        printk("input selftest: FAIL wheel packet\n");
    }

    /* Desynchronised first byte must be rejected. */
    u8 bad[4] = {0x00, 0x01, 0x02, 0x00};
    g_selftest_checks++;
    if (mouse_process_packet(bad, 3, MOUSE_PROTO_PS2) >= 0) {
        g_selftest_failures++;
        printk("input selftest: FAIL bad first byte accepted\n");
    }
}

static void test_ring(void)
{
    axinput_ring_t ring;
    axinput_ring_init(&ring);
    check_true("ring-empty", (bool_t)(axinput_ring_count(&ring) == 0));

    axinput_event_t event;
    ax_memset(&event, 0, sizeof(event));
    event.class = AXINPUT_KEY;
    event.code = AXKEY_A;
    check_true("ring-push", axinput_ring_push(&ring, &event));
    check_true("ring-count", (bool_t)(axinput_ring_count(&ring) == 1));

    axinput_event_t out;
    check_true("ring-pop", axinput_ring_pop(&ring, &out));
    check_true("ring-pop-class", (bool_t)(out.class == AXINPUT_KEY));
    check_true("ring-pop-empty", (bool_t)(axinput_ring_count(&ring) == 0));

    /* Fill past capacity and check that the oldest are dropped while the newest
     * survive. */
    for (u32 i = 0; i < AXINPUT_RING_SIZE + 8; i++) {
        event.code = (u16)(i & 0x7F);
        (void)axinput_ring_push(&ring, &event);
    }
    check_true("ring-dropped", (bool_t)(ring.dropped == 8));
    check_true("ring-full-count",
               (bool_t)(axinput_ring_count(&ring) == AXINPUT_RING_SIZE));
    bool_t saw_last = (bool_t)0;
    while (axinput_ring_pop(&ring, &out)) {
        if (out.code == (u16)((AXINPUT_RING_SIZE + 7) & 0x7F)) {
            saw_last = (bool_t)1;
        }
    }
    check_true("ring-newest-survives", saw_last);

    char stats[128];
    axinput_ring_stats(&ring, stats, sizeof(stats));
    check_true("ring-stats-nonempty", (bool_t)(stats[0] != 0));
}

static void test_hotkeys(void)
{
    axhotkey_table_t table;
    axhotkey_init(&table);
    axhotkey_fn fn = hotkey_sysrq;
    i32 index = axhotkey_register(&table, "test", AXKEY_A, AXMOD_CTRL,
                                  AXMOD_ANY_SHIFT, fn, 0);
    check_true("hotkey-register", (bool_t)(index == 0));

    axinput_event_t event;
    ax_memset(&event, 0, sizeof(event));
    event.class = AXINPUT_KEY;
    event.code = AXKEY_A;
    event.value = 1;
    event.modifiers = AXMOD_CTRL;
    check_true("hotkey-match", axhotkey_dispatch(&table, &event));

    event.modifiers = AXMOD_CTRL | AXMOD_LSHIFT;
    check_true("hotkey-forbidden", (bool_t)(axhotkey_dispatch(&table, &event) == 0));

    event.modifiers = 0;
    check_true("hotkey-missing-mod", (bool_t)(axhotkey_dispatch(&table, &event) == 0));

    check_true("hotkey-unregister", (bool_t)(axhotkey_unregister(&table, "test") == 0));
}

i32 axinput_selftest(void)
{
    g_selftest_failures = 0;
    g_selftest_checks = 0;
    printk("input selftest: begin\n");

    /* The selftest runs against the live driver when one exists, otherwise it
     * still exercises the pure functions and the ring. */
    test_tables();
    test_ascii();
    if (keyboard_ready()) {
        keyboard_reset_state();
        test_modifiers();
        test_typematic();
        keyboard_reset_state();
    }
    test_mouse();
    test_ring();
    test_hotkeys();

    printk("input selftest: %u checks, %u failures\n", g_selftest_checks,
           g_selftest_failures);
    return g_selftest_failures ? -1 : 0;
}
