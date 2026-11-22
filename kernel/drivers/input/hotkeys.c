/*
 * AxiomOS - hotkey registry.
 *
 * Hotkeys are registered by name with a key code, a required modifier mask and
 * a forbidden one.  Forbidden modifiers exist because the interesting chords
 * overlap: Ctrl-Alt-Del and Ctrl-Shift-Esc must not both fire when the user
 * presses Ctrl-Alt-Shift-Del, and a shell that binds Ctrl-C must not steal
 * Ctrl-Alt-C from the window manager.
 *
 * Dispatch is first-match-wins in registration order, which makes the ordering
 * visible in `axhotkey_dump` instead of implicit in a hash.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axinput.h>

void axhotkey_init(axhotkey_table_t *table)
{
    if (!table) {
        return;
    }
    ax_memset(table, 0, sizeof(*table));
}

i32 axhotkey_register(axhotkey_table_t *table, const char *name, u32 key,
                      u32 required, u32 forbidden, axhotkey_fn handler,
                      void *context)
{
    if (!table || !handler || key == 0) {
        return -22;
    }
    if (table->count >= AXHOTKEY_MAX) {
        return -28;
    }
    axhotkey_t *entry = &table->entries[table->count];
    ax_memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->required_mods = required;
    entry->forbidden_mods = forbidden;
    entry->handler = handler;
    entry->context = context;
    entry->active = (bool_t)1;
    usize length = ax_strlen(name ? name : "");
    if (length >= sizeof(entry->name)) {
        length = sizeof(entry->name) - 1;
    }
    for (usize i = 0; i < length; i++) {
        entry->name[i] = name[i];
    }
    entry->name[length] = 0;
    table->count++;
    return (i32)(table->count - 1);
}

static bool_t hotkey_matches(const axhotkey_t *entry,
                             const axinput_event_t *event)
{
    if (!entry->active) {
        return (bool_t)0;
    }
    if (entry->key != event->code) {
        return (bool_t)0;
    }
    u32 relevant = event->modifiers & ~(AXMOD_CAPS | AXMOD_NUM | AXMOD_SCROLL);
    if ((relevant & entry->required_mods) != entry->required_mods) {
        return (bool_t)0;
    }
    if (entry->forbidden_mods && (relevant & entry->forbidden_mods)) {
        return (bool_t)0;
    }
    return (bool_t)1;
}

bool_t axhotkey_dispatch(axhotkey_table_t *table, const axinput_event_t *event)
{
    if (!table || !event) {
        return (bool_t)0;
    }
    for (u32 i = 0; i < table->count; i++) {
        axhotkey_t *entry = &table->entries[i];
        if (!hotkey_matches(entry, event)) {
            continue;
        }
        entry->hits++;
        table->matches++;
        entry->handler(entry->context);
        return (bool_t)1;
    }
    return (bool_t)0;
}

/* Remove a registration by name.  Returns 0 on success, -2 when absent. */
i32 axhotkey_unregister(axhotkey_table_t *table, const char *name)
{
    if (!table || !name) {
        return -22;
    }
    for (u32 i = 0; i < table->count; i++) {
        if (ax_strcmp(table->entries[i].name, name) != 0) {
            continue;
        }
        for (u32 j = i; j + 1 < table->count; j++) {
            table->entries[j] = table->entries[j + 1];
        }
        table->count--;
        return 0;
    }
    return -2;
}

/* Enable or disable a registration without removing it, so a shell command can
 * suspend a binding temporarily. */
i32 axhotkey_set_active(axhotkey_table_t *table, const char *name, bool_t active)
{
    if (!table || !name) {
        return -22;
    }
    for (u32 i = 0; i < table->count; i++) {
        if (ax_strcmp(table->entries[i].name, name) == 0) {
            table->entries[i].active = active;
            return 0;
        }
    }
    return -2;
}

void axhotkey_dump(const axhotkey_table_t *table)
{
    if (!table) {
        return;
    }
    printk("hotkeys: %u registered, %u matches\n", table->count, table->matches);
    for (u32 i = 0; i < table->count; i++) {
        const axhotkey_t *entry = &table->entries[i];
        char mods[16];
        axinput_modifier_string(entry->required_mods, mods, sizeof(mods));
        printk("hotkeys: [%u] %s = %s%s%s active=%d hits=%u\n", i, entry->name,
               mods, mods[0] ? "+" : "", key_name((axkey_t)entry->key),
               entry->active, entry->hits);
    }
}
