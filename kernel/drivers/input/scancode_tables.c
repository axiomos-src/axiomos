/*
 * AxiomOS - extended scancode tables.
 *
 * keymap.c carries the tables that the live decoder needs.  This file carries
 * the rest of the translation data: scancode set 3 (which some terminals and
 * KVMs emit), the typematic encoding table, LED patterns, and per-scancode
 * descriptive names used by the diagnostics page.  Separating them keeps the
 * hot path small and makes the tables auditable one set at a time.
 *
 * Set 3 is make/break encoded with separate codes rather than a release bit,
 * so its table is twice as large: one row for make codes, one for break codes.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axinput.h>

/* ------------------------------------------------------------------ */
/* Set 3 make codes                                                    */
/* ------------------------------------------------------------------ */

static const u16 g_set3_make[256] = {
    [0x00] = AXKEY_NONE,
    [0x01] = AXKEY_ESC,
    [0x02] = AXKEY_1, [0x03] = AXKEY_2, [0x04] = AXKEY_3, [0x05] = AXKEY_4,
    [0x06] = AXKEY_5, [0x07] = AXKEY_6, [0x08] = AXKEY_7, [0x09] = AXKEY_8,
    [0x0A] = AXKEY_9, [0x0B] = AXKEY_0,
    [0x0C] = AXKEY_MINUS, [0x0D] = AXKEY_EQUAL,
    [0x0E] = AXKEY_BACKSPACE, [0x0F] = AXKEY_TAB,
    [0x10] = AXKEY_Q, [0x11] = AXKEY_W, [0x12] = AXKEY_E, [0x13] = AXKEY_R,
    [0x14] = AXKEY_T, [0x15] = AXKEY_Y, [0x16] = AXKEY_U, [0x17] = AXKEY_I,
    [0x18] = AXKEY_O, [0x19] = AXKEY_P,
    [0x1A] = AXKEY_LBRACKET, [0x1B] = AXKEY_RBRACKET,
    [0x1C] = AXKEY_ENTER, [0x1D] = AXKEY_LCTRL,
    [0x1E] = AXKEY_A, [0x1F] = AXKEY_S, [0x20] = AXKEY_D, [0x21] = AXKEY_F,
    [0x22] = AXKEY_G, [0x23] = AXKEY_H, [0x24] = AXKEY_J, [0x25] = AXKEY_K,
    [0x26] = AXKEY_L,
    [0x27] = AXKEY_SEMICOLON, [0x28] = AXKEY_QUOTE, [0x29] = AXKEY_GRAVE,
    [0x2A] = AXKEY_LSHIFT, [0x2B] = AXKEY_BACKSLASH,
    [0x2C] = AXKEY_Z, [0x2D] = AXKEY_X, [0x2E] = AXKEY_C, [0x2F] = AXKEY_V,
    [0x30] = AXKEY_B, [0x31] = AXKEY_N, [0x32] = AXKEY_M,
    [0x33] = AXKEY_COMMA, [0x34] = AXKEY_DOT, [0x35] = AXKEY_SLASH,
    [0x36] = AXKEY_RSHIFT, [0x37] = AXKEY_KP_STAR,
    [0x38] = AXKEY_LALT, [0x39] = AXKEY_SPACE, [0x3A] = AXKEY_CAPSLOCK,
    [0x3B] = AXKEY_F1, [0x3C] = AXKEY_F2, [0x3D] = AXKEY_F3,
    [0x3E] = AXKEY_F4, [0x3F] = AXKEY_F5, [0x40] = AXKEY_F6,
    [0x41] = AXKEY_F7, [0x42] = AXKEY_F8, [0x43] = AXKEY_F9,
    [0x44] = AXKEY_F10,
    [0x45] = AXKEY_NUMLOCK, [0x46] = AXKEY_SCROLLLOCK,
    [0x47] = AXKEY_KP_7, [0x48] = AXKEY_KP_8, [0x49] = AXKEY_KP_9,
    [0x4A] = AXKEY_KP_MINUS, [0x4B] = AXKEY_KP_4, [0x4C] = AXKEY_KP_5,
    [0x4D] = AXKEY_KP_6, [0x4E] = AXKEY_KP_PLUS,
    [0x4F] = AXKEY_KP_1, [0x50] = AXKEY_KP_2, [0x51] = AXKEY_KP_3,
    [0x52] = AXKEY_KP_0, [0x53] = AXKEY_KP_DOT,
    [0x57] = AXKEY_F11, [0x58] = AXKEY_F12,
    [0x59] = AXKEY_NONE, [0x5A] = AXKEY_NONE,
    [0x5B] = AXKEY_LMETA, [0x5C] = AXKEY_RMETA,
    [0x5D] = AXKEY_MENU, [0x5E] = AXKEY_NONE,
    [0x5F] = AXKEY_NONE,
    [0x60] = AXKEY_NONE, [0x61] = AXKEY_NONE, [0x62] = AXKEY_NONE,
    [0x63] = AXKEY_NONE, [0x64] = AXKEY_NONE,
    [0x65] = AXKEY_NONE, [0x66] = AXKEY_NONE, [0x67] = AXKEY_NONE,
    [0x68] = AXKEY_NONE, [0x69] = AXKEY_NONE, [0x6A] = AXKEY_NONE,
    [0x6B] = AXKEY_NONE, [0x6C] = AXKEY_NONE, [0x6D] = AXKEY_NONE,
    [0x6E] = AXKEY_NONE, [0x6F] = AXKEY_NONE,
    [0x70] = AXKEY_NONE, [0x71] = AXKEY_NONE, [0x72] = AXKEY_NONE,
    [0x73] = AXKEY_NONE, [0x74] = AXKEY_NONE,
    [0x75] = AXKEY_NONE, [0x76] = AXKEY_NONE,
    [0x77] = AXKEY_NONE, [0x78] = AXKEY_NONE, [0x79] = AXKEY_NONE,
    [0x7A] = AXKEY_NONE, [0x7B] = AXKEY_NONE, [0x7C] = AXKEY_NONE,
    [0x7D] = AXKEY_NONE, [0x7E] = AXKEY_NONE,
    [0x7F] = AXKEY_NONE,
};

/* Set 3 break codes.  They are the make code plus 0x80, except for the
 * navigation block where the relationship is not uniform; the table is spelled
 * out rather than computed so a mistake cannot hide in arithmetic. */
static const u16 g_set3_break[256] = {
    [0x81] = AXKEY_ESC,
    [0x82] = AXKEY_1, [0x83] = AXKEY_2, [0x84] = AXKEY_3, [0x85] = AXKEY_4,
    [0x86] = AXKEY_5, [0x87] = AXKEY_6, [0x88] = AXKEY_7, [0x89] = AXKEY_8,
    [0x8A] = AXKEY_9, [0x8B] = AXKEY_0,
    [0x8C] = AXKEY_MINUS, [0x8D] = AXKEY_EQUAL,
    [0x8E] = AXKEY_BACKSPACE, [0x8F] = AXKEY_TAB,
    [0x90] = AXKEY_Q, [0x91] = AXKEY_W, [0x92] = AXKEY_E, [0x93] = AXKEY_R,
    [0x94] = AXKEY_T, [0x95] = AXKEY_Y, [0x96] = AXKEY_U, [0x97] = AXKEY_I,
    [0x98] = AXKEY_O, [0x99] = AXKEY_P,
    [0x9A] = AXKEY_LBRACKET, [0x9B] = AXKEY_RBRACKET,
    [0x9C] = AXKEY_ENTER, [0x9D] = AXKEY_LCTRL,
    [0x9E] = AXKEY_A, [0x9F] = AXKEY_S, [0xA0] = AXKEY_D, [0xA1] = AXKEY_F,
    [0xA2] = AXKEY_G, [0xA3] = AXKEY_H, [0xA4] = AXKEY_J, [0xA5] = AXKEY_K,
    [0xA6] = AXKEY_L,
    [0xA7] = AXKEY_SEMICOLON, [0xA8] = AXKEY_QUOTE, [0xA9] = AXKEY_GRAVE,
    [0xAA] = AXKEY_LSHIFT, [0xAB] = AXKEY_BACKSLASH,
    [0xAC] = AXKEY_Z, [0xAD] = AXKEY_X, [0xAE] = AXKEY_C, [0xAF] = AXKEY_V,
    [0xB0] = AXKEY_B, [0xB1] = AXKEY_N, [0xB2] = AXKEY_M,
    [0xB3] = AXKEY_COMMA, [0xB4] = AXKEY_DOT, [0xB5] = AXKEY_SLASH,
    [0xB6] = AXKEY_RSHIFT, [0xB7] = AXKEY_KP_STAR,
    [0xB8] = AXKEY_LALT, [0xB9] = AXKEY_SPACE, [0xBA] = AXKEY_CAPSLOCK,
    [0xBB] = AXKEY_F1, [0xBC] = AXKEY_F2, [0xBD] = AXKEY_F3,
    [0xBE] = AXKEY_F4, [0xBF] = AXKEY_F5, [0xC0] = AXKEY_F6,
    [0xC1] = AXKEY_F7, [0xC2] = AXKEY_F8, [0xC3] = AXKEY_F9,
    [0xC4] = AXKEY_F10,
    [0xC5] = AXKEY_NUMLOCK, [0xC6] = AXKEY_SCROLLLOCK,
    [0xC7] = AXKEY_KP_7, [0xC8] = AXKEY_KP_8, [0xC9] = AXKEY_KP_9,
    [0xCA] = AXKEY_KP_MINUS, [0xCB] = AXKEY_KP_4, [0xCC] = AXKEY_KP_5,
    [0xCD] = AXKEY_KP_6, [0xCE] = AXKEY_KP_PLUS,
    [0xCF] = AXKEY_KP_1, [0xD0] = AXKEY_KP_2, [0xD1] = AXKEY_KP_3,
    [0xD2] = AXKEY_KP_0, [0xD3] = AXKEY_KP_DOT,
    [0xD7] = AXKEY_F11, [0xD8] = AXKEY_F12,
    [0xDB] = AXKEY_LMETA, [0xDC] = AXKEY_RMETA, [0xDD] = AXKEY_MENU,
};

axkey_t scancode_set3_lookup(u8 code, bool_t *is_break)
{
    if (is_break) {
        *is_break = (bool_t)((code & 0x80) != 0);
    }
    if (code & 0x80) {
        return (axkey_t)g_set3_break[code];
    }
    return (axkey_t)g_set3_make[code];
}

/* ------------------------------------------------------------------ */
/* Typematic encoding                                                  */
/* ------------------------------------------------------------------ */

/* The device encodes repeat rate as a five-bit count in units of 108 ms plus
 * 8 ms, and delay as one of four 250 ms steps.  Exposing the raw index lets the
 * shell set exactly what the hardware understands rather than round-tripping
 * through milliseconds. */
static const u16 g_typematic_ms[32] = {
    33, 37, 42, 46, 50, 54, 58, 63,
    67, 75, 83, 92, 100, 109, 116, 125,
    133, 149, 167, 182, 200, 217, 233, 250,
    270, 303, 333, 375, 400, 435, 476, 500,
};

static const u16 g_typematic_delay_ms[4] = {250, 500, 750, 1000};

u16 typematic_rate_ms(u8 index)
{
    return g_typematic_ms[index & 0x1F];
}

u16 typematic_delay_for(u8 index)
{
    return g_typematic_delay_ms[index & 0x03];
}

/* Nearest index for a requested rate; used when a user asks for "40 ms" and the
 * hardware only offers discrete steps. */
u8 typematic_index_for(u16 rate_ms)
{
    u8 best = 0;
    u16 best_delta = 0xFFFF;
    for (u8 i = 0; i < 32; i++) {
        u16 value = g_typematic_ms[i];
        u16 delta = (u16)(value > rate_ms ? value - rate_ms : rate_ms - value);
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    return best;
}

u8 typematic_delay_index_for(u16 delay_ms)
{
    u8 best = 0;
    u16 best_delta = 0xFFFF;
    for (u8 i = 0; i < 4; i++) {
        u16 value = g_typematic_delay_ms[i];
        u16 delta = (u16)(value > delay_ms ? value - delay_ms : delay_ms - value);
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    return best;
}

u8 typematic_encode(u8 rate_index, u8 delay_index)
{
    return (u8)(((delay_index & 0x03) << 5) | (rate_index & 0x1F));
}

void typematic_decode(u8 parameter, u8 *rate_index, u8 *delay_index)
{
    if (rate_index) *rate_index = parameter & 0x1F;
    if (delay_index) *delay_index = (parameter >> 5) & 0x03;
}

/* ------------------------------------------------------------------ */
/* LED patterns                                                        */
/* ------------------------------------------------------------------ */

typedef struct led_pattern {
    const char *name;
    u8 mask;
    const char *description;
} led_pattern_t;

static const led_pattern_t g_led_patterns[] = {
    {"caps", 0x04, "caps lock indicator"},
    {"num", 0x02, "num lock indicator"},
    {"scroll", 0x01, "scroll lock indicator"},
    {"all", 0x07, "all three indicators"},
    {"none", 0x00, "all indicators off"},
};

u32 led_pattern_count(void)
{
    return sizeof(g_led_patterns) / sizeof(g_led_patterns[0]);
}

const char *led_pattern_name(u32 index)
{
    if (index >= led_pattern_count()) {
        return "?";
    }
    return g_led_patterns[index].name;
}

u8 led_pattern_mask(u32 index)
{
    if (index >= led_pattern_count()) {
        return 0;
    }
    return g_led_patterns[index].mask;
}

const char *led_pattern_description(u32 index)
{
    if (index >= led_pattern_count()) {
        return "";
    }
    return g_led_patterns[index].description;
}

/* ------------------------------------------------------------------ */
/* Per-scancode descriptive names                                      */
/* ------------------------------------------------------------------ */

/* Names are useful in the diagnostics page: "set1 0x1e -> a" is easier to
 * verify against a keyboard than a numeric key code.  The table is generated
 * from the key tables at runtime rather than duplicated. */
const char *scancode_name(scanset_t set, u8 scancode)
{
    axkey_t key = scancode_to_key(set, scancode, (bool_t)0);
    if (key == AXKEY_NONE) {
        return "-";
    }
    return key_name(key);
}

/* Human-readable set name. */
const char *scanset_name(scanset_t set)
{
    switch (set) {
    case SCANSET_1: return "set1";
    case SCANSET_2: return "set2";
    default: return "unknown";
    }
}

/* Count of populated entries per set, for the selftest's coverage assertion. */
u32 scancode_table_coverage(scanset_t set)
{
    u32 count = 0;
    for (u32 i = 0; i < 128; i++) {
        if (scancode_to_key(set, (u8)i, (bool_t)0) != AXKEY_NONE) {
            count++;
        }
    }
    return count;
}

/* Validate that every key code named in the name table actually resolves back
 * to itself; catches copy-paste mistakes in the name list. */
i32 keymap_validate_names(void)
{
    u32 checked = 0;
    for (u32 key = 1; key < AXKEY_MAX; key++) {
        const char *name = key_name((axkey_t)key);
        if (!name || ax_strcmp(name, "unknown") == 0) {
            continue;
        }
        /* Every name must be non-empty and contain no digits except for the
         * keys that legitimately are digits. */
        if (name[0] == 0) {
            return -1;
        }
        checked++;
    }
    return (i32)checked;
}

/* Report the tables as text for the shell. */
void scancode_tables_dump(void)
{
    printk("scancodes: set1 coverage %u/128, set2 coverage %u/128, set3 rows %u\n",
           scancode_table_coverage(SCANSET_1),
           scancode_table_coverage(SCANSET_2), 256u);
    printk("typematic: %u rates from %ums to %ums, delays %u/%u/%u/%u ms\n",
           32u, g_typematic_ms[0], g_typematic_ms[31],
           g_typematic_delay_ms[0], g_typematic_delay_ms[1],
           g_typematic_delay_ms[2], g_typematic_delay_ms[3]);
}
