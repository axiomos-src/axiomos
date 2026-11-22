/*
 * AxiomOS - input diagnostics commands.
 *
 * The shell hands us an argument vector; these handlers print state and, where
 * it is safe, change it.  Nothing here allocates, and every command is
 * idempotent except the macro recorder, which is explicit about starting and
 * stopping.
 *
 * Command surface:
 *   input              one-line summary
 *   input dump         full driver state
 *   input keys         held keys and modifier word
 *   input scancodes    table coverage and typematic options
 *   input history [n]  last n events
 *   input stats        event histogram
 *   input layout X     us | uk | de
 *   input macro ...    list | begin NAME | end | play NAME [n]
 *   mouse              pointer position and gesture configuration
 *   mouse gestures     gesture counters
 *   input selftest     run the built-in checks
 *   hotkey             list registered chords
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axinput.h>

/* Declared in their own translation units; the header only carries the subset
 * the GUI and drivers share. */
void keyboard_set_layout(u8 layout);
const char *keyboard_layout_name(u8 layout);
const char *scanset_name(scanset_t set);
u32 scancode_table_coverage(scanset_t set);
void scancode_tables_dump(void);
u16 typematic_rate_ms(u8 index);
u16 typematic_delay_for(u8 index);
u8 typematic_index_for(u16 rate_ms);
u8 typematic_delay_index_for(u16 delay_ms);
void mouse_gestures_dump(void);
bool_t mouse_gestures_dragging(void);
u32 mouse_gesture_count(void);
u32 mouse_gestures_drag_distance(void);
void input_history_dump(u32 limit);
void input_history_stats(void);
u32 input_history_count(void);
void input_macro_list(void);
i32 input_macro_begin(const char *name);
i32 input_macro_end(void);
i32 input_macro_play(const char *name, u32 repeats);
void kbdtext_dump(void);
void mouse_emulate_move(i32 dx, i32 dy);

static void cmd_summary(void)
{
    printk("input: keyboard=%d mouse=%d %s\n", keyboard_ready(),
           mouse_ready(), axinput_ready() ? "ready" : "not initialised");
    printk("input: modifiers=%04x ring=%u\n", keyboard_modifiers(),
           axinput_ring_count(axinput_events()));
}

static void cmd_keys(void)
{
    printk("keys: modifiers=%04x caps=%d num=%d scroll=%d\n",
           keyboard_modifiers(),
           (keyboard_modifiers() & AXMOD_CAPS) != 0,
           (keyboard_modifiers() & AXMOD_NUM) != 0,
           (keyboard_modifiers() & AXMOD_SCROLL) != 0);
    u32 held = 0;
    char line[128];
    usize used = 0;
    line[0] = 0;
    for (u32 key = 1; key < AXKEY_MAX; key++) {
        if (!keyboard_key_down((axkey_t)key)) {
            continue;
        }
        held++;
        const char *name = key_name((axkey_t)key);
        while (*name && used + 2 < sizeof(line)) {
            line[used++] = *name++;
        }
        if (used + 1 < sizeof(line)) {
            line[used++] = ' ';
            line[used] = 0;
        }
        if (used > sizeof(line) - 16) {
            printk("keys: held %s\n", line);
            used = 0;
            line[0] = 0;
        }
    }
    if (used) {
        printk("keys: held %s\n", line);
    }
    printk("keys: %u held\n", held);
}

static void cmd_scancodes(void)
{
    scancode_tables_dump();
    printk("scancodes: live set %s, typematic now %ums delay %ums\n",
           scanset_name((scanset_t)(keyboard_modifiers() ? 1 : 1)),
           typematic_rate_ms(0), typematic_delay_for(0));
}

static void cmd_layout(const char *name)
{
    if (!name) {
        printk("layout: %s\n", keyboard_layout_name(0));
        return;
    }
    if (ax_strcmp(name, "us") == 0) {
        keyboard_set_layout(0);
    } else if (ax_strcmp(name, "us-intl") == 0) {
        keyboard_set_layout(1);
    } else if (ax_strcmp(name, "uk") == 0) {
        keyboard_set_layout(2);
    } else if (ax_strcmp(name, "de") == 0) {
        keyboard_set_layout(3);
    } else {
        printk("layout: unknown '%s' (us, us-intl, uk, de)\n", name);
        return;
    }
    printk("layout: now %s\n", keyboard_layout_name(0));
}

static void cmd_macro(u32 argc, const char **argv)
{
    if (argc < 2 || ax_strcmp(argv[1], "list") == 0) {
        input_macro_list();
        return;
    }
    if (ax_strcmp(argv[1], "begin") == 0 && argc >= 3) {
        i32 index = input_macro_begin(argv[2]);
        printk("macro: begin %s -> %d\n", argv[2], index);
        return;
    }
    if (ax_strcmp(argv[1], "end") == 0) {
        printk("macro: end, %d entries\n", input_macro_end());
        return;
    }
    if (ax_strcmp(argv[1], "play") == 0 && argc >= 3) {
        u32 repeats = (argc >= 4) ? (u32)ax_atoi(argv[3]) : 1;
        printk("macro: play %s x%u -> %d\n", argv[2], repeats,
               input_macro_play(argv[2], repeats));
        return;
    }
    printk("macro: usage: input macro list|begin NAME|end|play NAME [n]\n");
}

static void cmd_mouse(u32 argc, const char **argv)
{
    if (argc >= 2 && ax_strcmp(argv[1], "gestures") == 0) {
        mouse_gestures_dump();
        return;
    }
    i32 x = 0;
    i32 y = 0;
    mouse_get_position(&x, &y);
    printk("mouse: %d,%d buttons=%02x gesture_count=%u dragging=%d\n", x, y,
           axmouse_buttons(), mouse_gesture_count(), mouse_gestures_dragging());
    printk("mouse: drag distance=%u\n", mouse_gestures_drag_distance());
    if (argc >= 3) {
        i32 dx = (i32)ax_atoi(argv[1]);
        i32 dy = (i32)ax_atoi(argv[2]);
        mouse_emulate_move(dx, dy);
        mouse_get_position(&x, &y);
        printk("mouse: moved to %d,%d\n", x, y);
    }
}

/* Entry point used by the shell.  `argv` is null-terminated and `argv[0]` is
 * the word "input" or "mouse"; returns 0 on success, -2 for an unknown verb. */
i32 input_command(u32 argc, const char **argv)
{
    if (!argv || argc == 0) {
        return -22;
    }
    const char *verb = argv[0];
    if (ax_strcmp(verb, "mouse") == 0) {
        cmd_mouse(argc, argv);
        return 0;
    }
    if (ax_strcmp(verb, "hotkey") == 0) {
        extern void axhotkey_table_dump_public(void);
        axhotkey_table_dump_public();
        return 0;
    }
    if (ax_strcmp(verb, "input") != 0) {
        return -2;
    }
    if (argc == 1) {
        cmd_summary();
        return 0;
    }
    const char *sub = argv[1];
    if (ax_strcmp(sub, "dump") == 0) {
        axinput_dump_state();
    } else if (ax_strcmp(sub, "keys") == 0) {
        cmd_keys();
    } else if (ax_strcmp(sub, "scancodes") == 0) {
        cmd_scancodes();
    } else if (ax_strcmp(sub, "history") == 0) {
        u32 limit = (argc >= 3) ? (u32)ax_atoi(argv[2]) : 16;
        input_history_dump(limit);
    } else if (ax_strcmp(sub, "stats") == 0) {
        input_history_stats();
    } else if (ax_strcmp(sub, "layout") == 0) {
        cmd_layout(argc >= 3 ? argv[2] : 0);
    } else if (ax_strcmp(sub, "macro") == 0) {
        cmd_macro(argc, argv);
    } else if (ax_strcmp(sub, "selftest") == 0) {
        i32 rc = axinput_selftest();
        printk("input: selftest %s\n", rc == 0 ? "passed" : "failed");
    } else if (ax_strcmp(sub, "compose") == 0) {
        kbdtext_dump();
    } else {
        printk("input: unknown subcommand '%s'\n", sub);
        printk("input: dump keys scancodes history stats layout macro compose "
               "selftest\n");
        return -2;
    }
    return 0;
}

/* Public wrapper so axinput.c can expose the hotkey table without pulling the
 * whole diagnostics module into its translation unit. */
static axhotkey_table_t g_public_hotkeys;

void axhotkey_table_bind(axhotkey_table_t *table)
{
    if (table) {
        g_public_hotkeys = *table;
    }
}

void axhotkey_table_dump_public(void)
{
    axhotkey_dump(&g_public_hotkeys);
}

/* Counter helper for the GUI's debug overlay: how many events of a class are
 * waiting.  Cheap enough to call every frame. */
u32 input_pending_class(u16 event_class)
{
    axinput_ring_t *ring = axinput_events();
    if (!ring) return 0;
    u32 count = axinput_ring_count(ring);
    u32 matches = 0;
    for (u32 i = 0; i < count; i++) {
        u32 index = (ring->tail + i) % AXINPUT_RING_SIZE;
        if (ring->events[index].class == event_class) {
            matches++;
        }
    }
    return matches;
}
