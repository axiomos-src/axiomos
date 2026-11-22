/*
 * AxiomOS - input history.
 *
 * Every event the GUI consumes is also appended here, in a bounded ring that
 * survives longer than the input queue.  Two consumers use it: the diagnostics
 * page, which shows the last N events so a stuck key is visible as a key that
 * never released, and the macro recorder, which captures a sequence and replays
 * it for automated tests.
 *
 * Replay deliberately goes through `axinput_inject_key`/`axinput_inject_mouse`
 * rather than into the ring directly, so replay is indistinguishable from user
 * input for every downstream consumer.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axinput.h>

#define HISTORY_MAX 512
#define MACRO_MAX_ENTRIES 256
#define MACRO_MAX 8

typedef struct history_entry {
    axinput_event_t event;
    u32 index;
} history_entry_t;

typedef struct input_history {
    history_entry_t entries[HISTORY_MAX];
    u32 head;
    u32 count;
    u32 wrapped;
    u64 first_ns;
    u64 last_ns;
} input_history_t;

typedef struct macro_entry {
    axinput_event_t event;
    u32 delay_ms;
} macro_entry_t;

typedef struct input_macro {
    char name[24];
    macro_entry_t entries[MACRO_MAX_ENTRIES];
    u32 count;
    bool_t recording;
    bool_t playing;
    u64 started_ns;
    u32 replays;
} input_macro_t;

static input_history_t g_history;
static input_macro_t g_macros[MACRO_MAX];
static u32 g_macro_count;
static input_macro_t *g_recording;
static u32 g_history_total;

void input_history_init(void)
{
    ax_memset(&g_history, 0, sizeof(g_history));
    ax_memset(g_macros, 0, sizeof(g_macros));
    g_macro_count = 0;
    g_recording = 0;
    g_history_total = 0;
}

void input_history_record(const axinput_event_t *event)
{
    if (!event) return;
    history_entry_t *slot = &g_history.entries[g_history.head];
    slot->event = *event;
    slot->index = g_history_total++;
    g_history.head = (g_history.head + 1) % HISTORY_MAX;
    if (g_history.count < HISTORY_MAX) {
        g_history.count++;
    } else {
        g_history.wrapped++;
    }
    if (g_history.first_ns == 0) {
        g_history.first_ns = event->timestamp_ns;
    }
    g_history.last_ns = event->timestamp_ns;

    if (g_recording && g_recording->count < MACRO_MAX_ENTRIES) {
        macro_entry_t *entry = &g_recording->entries[g_recording->count++];
        entry->event = *event;
        entry->delay_ms = 0;
    }
}

/* Walk entries oldest first, invoking `fn` on each.  Callback style avoids
 * copying the ring or exposing its index arithmetic to callers. */
typedef void (*history_visit_fn)(const history_entry_t *entry, void *context);

void input_history_walk(history_visit_fn fn, void *context)
{
    if (!fn) return;
    u32 start = (g_history.head + HISTORY_MAX - g_history.count) % HISTORY_MAX;
    for (u32 i = 0; i < g_history.count; i++) {
        u32 index = (start + i) % HISTORY_MAX;
        fn(&g_history.entries[index], context);
    }
}

typedef struct dump_context {
    u32 shown;
    u32 limit;
} dump_context_t;

static void dump_one(const history_entry_t *entry, void *context)
{
    dump_context_t *dump = (dump_context_t *)context;
    if (dump->shown >= dump->limit) {
        return;
    }
    dump->shown++;
    char mods[16];
    axinput_modifier_string(entry->event.modifiers, mods, sizeof(mods));
    if (entry->event.class == AXINPUT_KEY) {
        printk("input[%u] key %s %s %s\n", entry->index,
               key_name((axkey_t)entry->event.code),
               entry->event.value ? "down" : "up", mods);
    } else if (entry->event.class == AXINPUT_MOUSE_MOVE ||
               entry->event.class == AXINPUT_MOUSE_BUTTON) {
        printk("input[%u] mouse %s at %d,%d code=%u value=%d\n", entry->index,
               axinput_class_name(entry->event.class), entry->event.x,
               entry->event.y, entry->event.code, entry->event.value);
    } else if (entry->event.class == AXINPUT_MOUSE_WHEEL) {
        printk("input[%u] wheel %d\n", entry->index, entry->event.value);
    } else {
        printk("input[%u] %s code=%u value=%d\n", entry->index,
               axinput_class_name(entry->event.class), entry->event.code,
               entry->event.value);
    }
}

void input_history_dump(u32 limit)
{
    dump_context_t context;
    context.shown = 0;
    context.limit = limit ? limit : 16;
    printk("history: %u entries, %u wrapped, span %llu..%llu ns\n",
           g_history.count, g_history.wrapped,
           (unsigned long long)g_history.first_ns,
           (unsigned long long)g_history.last_ns);
    input_history_walk(dump_one, &context);
}

u32 input_history_count(void)
{
    return g_history.count;
}

u32 input_history_wrapped(void)
{
    return g_history.wrapped;
}

bool_t input_history_latest(axinput_event_t *out, u32 offset)
{
    if (!out || offset >= g_history.count) {
        return (bool_t)0;
    }
    u32 index = (g_history.head + HISTORY_MAX - 1 - offset) % HISTORY_MAX;
    *out = g_history.entries[index].event;
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Macros                                                              */
/* ------------------------------------------------------------------ */

i32 input_macro_begin(const char *name)
{
    if (!name || g_macro_count >= MACRO_MAX) {
        return -28;
    }
    input_macro_t *macro = &g_macros[g_macro_count];
    ax_memset(macro, 0, sizeof(*macro));
    usize length = ax_strlen(name);
    if (length >= sizeof(macro->name)) {
        length = sizeof(macro->name) - 1;
    }
    for (usize i = 0; i < length; i++) {
        macro->name[i] = name[i];
    }
    macro->name[length] = 0;
    macro->recording = (bool_t)1;
    macro->started_ns = 0;
    g_recording = macro;
    g_macro_count++;
    return (i32)(g_macro_count - 1);
}

i32 input_macro_end(void)
{
    if (!g_recording) {
        return -2;
    }
    g_recording->recording = (bool_t)0;
    i32 count = (i32)g_recording->count;
    g_recording = 0;
    return count;
}

i32 input_macro_play(const char *name, u32 repeats)
{
    if (!name) return -22;
    input_macro_t *macro = 0;
    for (u32 i = 0; i < g_macro_count; i++) {
        if (ax_strcmp(g_macros[i].name, name) == 0) {
            macro = &g_macros[i];
            break;
        }
    }
    if (!macro || macro->recording) {
        return -2;
    }
    if (repeats == 0) repeats = 1;
    for (u32 r = 0; r < repeats; r++) {
        for (u32 i = 0; i < macro->count; i++) {
            const axinput_event_t *event = &macro->entries[i].event;
            if (event->class == AXINPUT_KEY) {
                (void)axinput_inject_key((axkey_t)event->code,
                                         (bool_t)(event->value != 0));
            } else if (event->class == AXINPUT_MOUSE_MOVE) {
                (void)axinput_inject_mouse(event->x, event->y, event->modifiers,
                                           0);
            } else if (event->class == AXINPUT_MOUSE_WHEEL) {
                (void)axinput_inject_mouse(0, 0, 0, event->value);
            }
        }
    }
    macro->replays++;
    return (i32)macro->count;
}

void input_macro_list(void)
{
    printk("macros: %u recorded\n", g_macro_count);
    for (u32 i = 0; i < g_macro_count; i++) {
        printk("macros: [%u] %s entries=%u recording=%d replays=%u\n", i,
               g_macros[i].name, g_macros[i].count, g_macros[i].recording,
               g_macros[i].replays);
    }
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

typedef struct history_stats {
    u32 keys;
    u32 presses;
    u32 releases;
    u32 repeats;
    u32 moves;
    u32 buttons;
    u32 wheels;
    u32 ctrl_uses;
    u32 alt_uses;
    u32 shift_uses;
} history_stats_t;

static void count_one(const history_entry_t *entry, void *context)
{
    history_stats_t *stats = (history_stats_t *)context;
    switch (entry->event.class) {
    case AXINPUT_KEY:
        stats->keys++;
        if (entry->event.value == AXKEY_PRESSED) stats->presses++;
        else if (entry->event.value == AXKEY_RELEASED) stats->releases++;
        else stats->repeats++;
        if (entry->event.modifiers & AXMOD_CTRL) stats->ctrl_uses++;
        if (entry->event.modifiers & AXMOD_ALT) stats->alt_uses++;
        if (entry->event.modifiers & AXMOD_ANY_SHIFT) stats->shift_uses++;
        break;
    case AXINPUT_MOUSE_MOVE: stats->moves++; break;
    case AXINPUT_MOUSE_BUTTON: stats->buttons++; break;
    case AXINPUT_MOUSE_WHEEL: stats->wheels++; break;
    default: break;
    }
}

void input_history_stats(void)
{
    history_stats_t stats;
    ax_memset(&stats, 0, sizeof(stats));
    input_history_walk(count_one, &stats);
    printk("history: keys=%u (%u down, %u up, %u repeat) moves=%u "
           "buttons=%u wheels=%u\n",
           stats.keys, stats.presses, stats.releases, stats.repeats,
           stats.moves, stats.buttons, stats.wheels);
    printk("history: ctrl=%u alt=%u shift=%u\n", stats.ctrl_uses,
           stats.alt_uses, stats.shift_uses);
}
