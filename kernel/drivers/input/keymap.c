/*
 * AxiomOS - key mapping tables.
 *
 * Two separate concerns live here because they fail in different ways:
 *
 *   scancode -> key code   is a hardware fact; a wrong entry means a key does
 *                           the wrong thing on that keyboard;
 *   key code -> character  is a layout fact; a wrong entry means the key is
 *                           labelled correctly but types the wrong glyph.
 *
 * Keeping them apart means a new layout (AZERTY, Dvorak, a national variant)
 * only touches the second table set, and a keyboard with an odd scancode only
 * touches the first.
 *
 * The tables are sparse-by-design: 128 entries each for sets 1 and 2, with gaps
 * filled by AXKEY_NONE so an out-of-range scancode cannot index a wrong key by
 * accident.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axinput.h>

/* ------------------------------------------------------------------ */
/* Scancode set 1                                                      */
/* ------------------------------------------------------------------ */

static const u16 g_set1[192] = {
    AXKEY_NONE,      AXKEY_ESC,       AXKEY_1,         AXKEY_2,
    AXKEY_3,         AXKEY_4,         AXKEY_5,         AXKEY_6,
    AXKEY_7,         AXKEY_8,         AXKEY_9,         AXKEY_0,
    AXKEY_MINUS,     AXKEY_EQUAL,     AXKEY_BACKSPACE, AXKEY_TAB,
    AXKEY_Q,         AXKEY_W,         AXKEY_E,         AXKEY_R,
    AXKEY_T,         AXKEY_Y,         AXKEY_U,         AXKEY_I,
    AXKEY_O,         AXKEY_P,         AXKEY_LBRACKET,  AXKEY_RBRACKET,
    AXKEY_ENTER,     AXKEY_LCTRL,     AXKEY_A,         AXKEY_S,
    AXKEY_D,         AXKEY_F,         AXKEY_G,         AXKEY_H,
    AXKEY_J,         AXKEY_K,         AXKEY_L,         AXKEY_SEMICOLON,
    AXKEY_QUOTE,     AXKEY_GRAVE,     AXKEY_LSHIFT,    AXKEY_BACKSLASH,
    AXKEY_Z,         AXKEY_X,         AXKEY_C,         AXKEY_V,
    AXKEY_B,         AXKEY_N,         AXKEY_M,         AXKEY_COMMA,
    AXKEY_DOT,       AXKEY_SLASH,     AXKEY_RSHIFT,    AXKEY_KP_STAR,
    AXKEY_LALT,      AXKEY_SPACE,     AXKEY_CAPSLOCK,  AXKEY_F1,
    AXKEY_F2,        AXKEY_F3,        AXKEY_F4,        AXKEY_F5,
    AXKEY_F6,        AXKEY_F7,        AXKEY_F8,        AXKEY_F9,
    AXKEY_F10,       AXKEY_NUMLOCK,   AXKEY_SCROLLLOCK, AXKEY_KP_7,
    AXKEY_KP_8,      AXKEY_KP_9,      AXKEY_KP_MINUS,  AXKEY_KP_4,
    AXKEY_KP_5,      AXKEY_KP_6,      AXKEY_KP_PLUS,   AXKEY_KP_1,
    AXKEY_KP_2,      AXKEY_KP_3,      AXKEY_KP_0,      AXKEY_KP_DOT,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_F11,
    AXKEY_F12,       AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
};

/* Extended set 1 codes, e0-prefixed. */
static const u16 g_set1_ext[192] = {
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_KP_ENTER,  AXKEY_RCTRL,     AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_KP_SLASH,
    AXKEY_NONE,      AXKEY_PRINTSCREEN, AXKEY_RALT,    AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_HOME,
    AXKEY_UP,        AXKEY_PAGEUP,    AXKEY_NONE,      AXKEY_LEFT,
    AXKEY_NONE,      AXKEY_RIGHT,     AXKEY_NONE,      AXKEY_END,
    AXKEY_DOWN,      AXKEY_PAGEDOWN,  AXKEY_INSERT,    AXKEY_DELETE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_LMETA,
    AXKEY_RMETA,     AXKEY_MENU,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
};

/* ------------------------------------------------------------------ */
/* Scancode set 2                                                      */
/* ------------------------------------------------------------------ */

static const u16 g_set2[192] = {
    AXKEY_NONE,      AXKEY_ESC,       AXKEY_1,         AXKEY_2,
    AXKEY_3,         AXKEY_4,         AXKEY_5,         AXKEY_6,
    AXKEY_7,         AXKEY_8,         AXKEY_9,         AXKEY_0,
    AXKEY_MINUS,     AXKEY_EQUAL,     AXKEY_BACKSPACE, AXKEY_TAB,
    AXKEY_Q,         AXKEY_W,         AXKEY_E,         AXKEY_R,
    AXKEY_T,         AXKEY_Y,         AXKEY_U,         AXKEY_I,
    AXKEY_O,         AXKEY_P,         AXKEY_LBRACKET,  AXKEY_RBRACKET,
    AXKEY_ENTER,     AXKEY_LCTRL,     AXKEY_A,         AXKEY_S,
    AXKEY_D,         AXKEY_F,         AXKEY_G,         AXKEY_H,
    AXKEY_J,         AXKEY_K,         AXKEY_L,         AXKEY_SEMICOLON,
    AXKEY_QUOTE,     AXKEY_GRAVE,     AXKEY_LSHIFT,    AXKEY_BACKSLASH,
    AXKEY_Z,         AXKEY_X,         AXKEY_C,         AXKEY_V,
    AXKEY_B,         AXKEY_N,         AXKEY_M,         AXKEY_COMMA,
    AXKEY_DOT,       AXKEY_SLASH,     AXKEY_RSHIFT,    AXKEY_KP_STAR,
    AXKEY_LALT,      AXKEY_SPACE,     AXKEY_CAPSLOCK,  AXKEY_F1,
    AXKEY_F2,        AXKEY_F3,        AXKEY_F4,        AXKEY_F5,
    AXKEY_F6,        AXKEY_F7,        AXKEY_F8,        AXKEY_F9,
    AXKEY_F10,       AXKEY_NUMLOCK,   AXKEY_SCROLLLOCK, AXKEY_KP_7,
    AXKEY_KP_8,      AXKEY_KP_9,      AXKEY_KP_MINUS,  AXKEY_KP_4,
    AXKEY_KP_5,      AXKEY_KP_6,      AXKEY_KP_PLUS,   AXKEY_KP_1,
    AXKEY_KP_2,      AXKEY_KP_3,      AXKEY_KP_0,      AXKEY_KP_DOT,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_F11,
    AXKEY_F12,       AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
};

static const u16 g_set2_ext[192] = {
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_KP_ENTER,  AXKEY_RCTRL,     AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_KP_SLASH,
    AXKEY_NONE,      AXKEY_PRINTSCREEN, AXKEY_RALT,    AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_HOME,
    AXKEY_UP,        AXKEY_PAGEUP,    AXKEY_NONE,      AXKEY_LEFT,
    AXKEY_NONE,      AXKEY_RIGHT,     AXKEY_NONE,      AXKEY_END,
    AXKEY_DOWN,      AXKEY_PAGEDOWN,  AXKEY_INSERT,    AXKEY_DELETE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_LMETA,
    AXKEY_RMETA,     AXKEY_MENU,      AXKEY_POWER,     AXKEY_SLEEP,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
    AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,      AXKEY_NONE,
};

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

axkey_t scancode_to_key(scanset_t set, u8 scancode, bool_t extended)
{
    u32 index = (u32)scancode & 0x7F;
    if (set == SCANSET_1) {
        return extended ? (axkey_t)g_set1_ext[index] : (axkey_t)g_set1[index];
    }
    if (set == SCANSET_2) {
        return extended ? (axkey_t)g_set2_ext[index] : (axkey_t)g_set2[index];
    }
    return AXKEY_NONE;
}

/* Raw lookup without the release bit stripped, used by the decoder for
 * validation. */
bool_t scancode_known(scanset_t set, u8 scancode, bool_t extended)
{
    u8 base = (u8)(scancode & 0x7F);
    return (bool_t)(scancode_to_key(set, base, extended) != AXKEY_NONE);
}

/* ------------------------------------------------------------------ */
/* Key names                                                           */
/* ------------------------------------------------------------------ */

typedef struct key_name_entry {
    u16 key;
    const char *name;
} key_name_entry_t;

static const key_name_entry_t g_key_names[] = {
    {AXKEY_ESC, "esc"}, {AXKEY_1, "1"}, {AXKEY_2, "2"}, {AXKEY_3, "3"},
    {AXKEY_4, "4"}, {AXKEY_5, "5"}, {AXKEY_6, "6"}, {AXKEY_7, "7"},
    {AXKEY_8, "8"}, {AXKEY_9, "9"}, {AXKEY_0, "0"},
    {AXKEY_MINUS, "minus"}, {AXKEY_EQUAL, "equal"},
    {AXKEY_BACKSPACE, "backspace"}, {AXKEY_TAB, "tab"},
    {AXKEY_Q, "q"}, {AXKEY_W, "w"}, {AXKEY_E, "e"}, {AXKEY_R, "r"},
    {AXKEY_T, "t"}, {AXKEY_Y, "y"}, {AXKEY_U, "u"}, {AXKEY_I, "i"},
    {AXKEY_O, "o"}, {AXKEY_P, "p"},
    {AXKEY_LBRACKET, "leftbracket"}, {AXKEY_RBRACKET, "rightbracket"},
    {AXKEY_ENTER, "enter"}, {AXKEY_LCTRL, "leftctrl"},
    {AXKEY_A, "a"}, {AXKEY_S, "s"}, {AXKEY_D, "d"}, {AXKEY_F, "f"},
    {AXKEY_G, "g"}, {AXKEY_H, "h"}, {AXKEY_J, "j"}, {AXKEY_K, "k"},
    {AXKEY_L, "l"}, {AXKEY_SEMICOLON, "semicolon"}, {AXKEY_QUOTE, "quote"},
    {AXKEY_GRAVE, "grave"}, {AXKEY_LSHIFT, "leftshift"},
    {AXKEY_BACKSLASH, "backslash"},
    {AXKEY_Z, "z"}, {AXKEY_X, "x"}, {AXKEY_C, "c"}, {AXKEY_V, "v"},
    {AXKEY_B, "b"}, {AXKEY_N, "n"}, {AXKEY_M, "m"},
    {AXKEY_COMMA, "comma"}, {AXKEY_DOT, "dot"}, {AXKEY_SLASH, "slash"},
    {AXKEY_RSHIFT, "rightshift"}, {AXKEY_KP_STAR, "kpstar"},
    {AXKEY_LALT, "leftalt"}, {AXKEY_SPACE, "space"},
    {AXKEY_CAPSLOCK, "capslock"}, {AXKEY_F1, "f1"}, {AXKEY_F2, "f2"},
    {AXKEY_F3, "f3"}, {AXKEY_F4, "f4"}, {AXKEY_F5, "f5"}, {AXKEY_F6, "f6"},
    {AXKEY_F7, "f7"}, {AXKEY_F8, "f8"}, {AXKEY_F9, "f9"}, {AXKEY_F10, "f10"},
    {AXKEY_F11, "f11"}, {AXKEY_F12, "f12"},
    {AXKEY_NUMLOCK, "numlock"}, {AXKEY_SCROLLLOCK, "scrolllock"},
    {AXKEY_KP_0, "kp0"}, {AXKEY_KP_1, "kp1"}, {AXKEY_KP_2, "kp2"},
    {AXKEY_KP_3, "kp3"}, {AXKEY_KP_4, "kp4"}, {AXKEY_KP_5, "kp5"},
    {AXKEY_KP_6, "kp6"}, {AXKEY_KP_7, "kp7"}, {AXKEY_KP_8, "kp8"},
    {AXKEY_KP_9, "kp9"}, {AXKEY_KP_DOT, "kpdot"},
    {AXKEY_KP_PLUS, "kpplus"}, {AXKEY_KP_MINUS, "kpminus"},
    {AXKEY_KP_ENTER, "kpenter"}, {AXKEY_KP_SLASH, "kpslash"},
    {AXKEY_RCTRL, "rightctrl"}, {AXKEY_RALT, "rightalt"},
    {AXKEY_PRINTSCREEN, "printscreen"}, {AXKEY_HOME, "home"},
    {AXKEY_UP, "up"}, {AXKEY_PAGEUP, "pageup"}, {AXKEY_LEFT, "left"},
    {AXKEY_RIGHT, "right"}, {AXKEY_END, "end"}, {AXKEY_DOWN, "down"},
    {AXKEY_PAGEDOWN, "pagedown"}, {AXKEY_INSERT, "insert"},
    {AXKEY_DELETE, "delete"},
    {AXKEY_LMETA, "leftmeta"}, {AXKEY_RMETA, "rightmeta"},
    {AXKEY_MENU, "menu"},
    {AXKEY_MEDIA_MUTE, "mute"}, {AXKEY_MEDIA_VOLUMEDOWN, "volumedown"},
    {AXKEY_MEDIA_VOLUMEUP, "volumeup"},
    {AXKEY_POWER, "power"}, {AXKEY_SLEEP, "sleep"}, {AXKEY_WAKE, "wake"},
};

const char *key_name(axkey_t key)
{
    for (u32 i = 0; i < sizeof(g_key_names) / sizeof(g_key_names[0]); i++) {
        if (g_key_names[i].key == (u16)key) {
            return g_key_names[i].name;
        }
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */
/* US layout, unshifted and shifted                                    */
/* ------------------------------------------------------------------ */

/* Index by key code; values are ASCII.  Keypad digits honour NumLock in
 * key_to_ascii, so they are listed as their numeric characters here. */
static const char g_us_plain[AXKEY_MAX] = {
    [AXKEY_A] = 'a', [AXKEY_B] = 'b', [AXKEY_C] = 'c', [AXKEY_D] = 'd',
    [AXKEY_E] = 'e', [AXKEY_F] = 'f', [AXKEY_G] = 'g', [AXKEY_H] = 'h',
    [AXKEY_I] = 'i', [AXKEY_J] = 'j', [AXKEY_K] = 'k', [AXKEY_L] = 'l',
    [AXKEY_M] = 'm', [AXKEY_N] = 'n', [AXKEY_O] = 'o', [AXKEY_P] = 'p',
    [AXKEY_Q] = 'q', [AXKEY_R] = 'r', [AXKEY_S] = 's', [AXKEY_T] = 't',
    [AXKEY_U] = 'u', [AXKEY_V] = 'v', [AXKEY_W] = 'w', [AXKEY_X] = 'x',
    [AXKEY_Y] = 'y', [AXKEY_Z] = 'z',
    [AXKEY_1] = '1', [AXKEY_2] = '2', [AXKEY_3] = '3', [AXKEY_4] = '4',
    [AXKEY_5] = '5', [AXKEY_6] = '6', [AXKEY_7] = '7', [AXKEY_8] = '8',
    [AXKEY_9] = '9', [AXKEY_0] = '0',
    [AXKEY_MINUS] = '-', [AXKEY_EQUAL] = '=',
    [AXKEY_LBRACKET] = '[', [AXKEY_RBRACKET] = ']',
    [AXKEY_SEMICOLON] = ';', [AXKEY_QUOTE] = '\'',
    [AXKEY_GRAVE] = '`', [AXKEY_BACKSLASH] = '\\',
    [AXKEY_COMMA] = ',', [AXKEY_DOT] = '.', [AXKEY_SLASH] = '/',
    [AXKEY_SPACE] = ' ', [AXKEY_TAB] = '\t', [AXKEY_ENTER] = '\n',
    [AXKEY_KP_0] = '0', [AXKEY_KP_1] = '1', [AXKEY_KP_2] = '2',
    [AXKEY_KP_3] = '3', [AXKEY_KP_4] = '4', [AXKEY_KP_5] = '5',
    [AXKEY_KP_6] = '6', [AXKEY_KP_7] = '7', [AXKEY_KP_8] = '8',
    [AXKEY_KP_9] = '9', [AXKEY_KP_DOT] = '.', [AXKEY_KP_SLASH] = '/',
    [AXKEY_KP_STAR] = '*', [AXKEY_KP_MINUS] = '-', [AXKEY_KP_PLUS] = '+',
    [AXKEY_KP_ENTER] = '\n',
};

static const char g_us_shift[AXKEY_MAX] = {
    [AXKEY_A] = 'A', [AXKEY_B] = 'B', [AXKEY_C] = 'C', [AXKEY_D] = 'D',
    [AXKEY_E] = 'E', [AXKEY_F] = 'F', [AXKEY_G] = 'G', [AXKEY_H] = 'H',
    [AXKEY_I] = 'I', [AXKEY_J] = 'J', [AXKEY_K] = 'K', [AXKEY_L] = 'L',
    [AXKEY_M] = 'M', [AXKEY_N] = 'N', [AXKEY_O] = 'O', [AXKEY_P] = 'P',
    [AXKEY_Q] = 'Q', [AXKEY_R] = 'R', [AXKEY_S] = 'S', [AXKEY_T] = 'T',
    [AXKEY_U] = 'U', [AXKEY_V] = 'V', [AXKEY_W] = 'W', [AXKEY_X] = 'X',
    [AXKEY_Y] = 'Y', [AXKEY_Z] = 'Z',
    [AXKEY_1] = '!', [AXKEY_2] = '@', [AXKEY_3] = '#', [AXKEY_4] = '$',
    [AXKEY_5] = '%', [AXKEY_6] = '^', [AXKEY_7] = '&', [AXKEY_8] = '*',
    [AXKEY_9] = '(', [AXKEY_0] = ')',
    [AXKEY_MINUS] = '_', [AXKEY_EQUAL] = '+',
    [AXKEY_LBRACKET] = '{', [AXKEY_RBRACKET] = '}',
    [AXKEY_SEMICOLON] = ':', [AXKEY_QUOTE] = '"',
    [AXKEY_GRAVE] = '~', [AXKEY_BACKSLASH] = '|',
    [AXKEY_COMMA] = '<', [AXKEY_DOT] = '>', [AXKEY_SLASH] = '?',
    [AXKEY_SPACE] = ' ',
};

/* UK layout differs in a handful of positions: quote becomes 2, 2 becomes
 * quote, 3 becomes pound, and backslash moves to the hash key. */
static const char g_uk_plain[AXKEY_MAX] = {
    [AXKEY_2] = '\'', [AXKEY_QUOTE] = '2', [AXKEY_3] = 0,
    [AXKEY_GRAVE] = '`', [AXKEY_BACKSLASH] = '#',
    [AXKEY_A] = 'a', [AXKEY_B] = 'b', [AXKEY_C] = 'c', [AXKEY_D] = 'd',
    [AXKEY_E] = 'e', [AXKEY_F] = 'f', [AXKEY_G] = 'g', [AXKEY_H] = 'h',
    [AXKEY_I] = 'i', [AXKEY_J] = 'j', [AXKEY_K] = 'k', [AXKEY_L] = 'l',
    [AXKEY_M] = 'm', [AXKEY_N] = 'n', [AXKEY_O] = 'o', [AXKEY_P] = 'p',
    [AXKEY_Q] = 'q', [AXKEY_R] = 'r', [AXKEY_S] = 's', [AXKEY_T] = 't',
    [AXKEY_U] = 'u', [AXKEY_V] = 'v', [AXKEY_W] = 'w', [AXKEY_X] = 'x',
    [AXKEY_Y] = 'y', [AXKEY_Z] = 'z',
    [AXKEY_SEMICOLON] = ';', [AXKEY_COMMA] = ',', [AXKEY_DOT] = '.',
    [AXKEY_SLASH] = '/', [AXKEY_SPACE] = ' ', [AXKEY_ENTER] = '\n',
    [AXKEY_TAB] = '\t', [AXKEY_MINUS] = '-', [AXKEY_EQUAL] = '=',
};

/* German layout, the one non-ASCII case we handle: y and z swap, and the
 * umlauts sit on the bracket keys.  Non-ASCII keys return 0 here and are
 * reachable through key_to_unicode. */
static const char g_de_plain[AXKEY_MAX] = {
    [AXKEY_Y] = 'z', [AXKEY_Z] = 'y',
    [AXKEY_A] = 'a', [AXKEY_B] = 'b', [AXKEY_C] = 'c', [AXKEY_D] = 'd',
    [AXKEY_E] = 'e', [AXKEY_F] = 'f', [AXKEY_G] = 'g', [AXKEY_H] = 'h',
    [AXKEY_I] = 'i', [AXKEY_J] = 'j', [AXKEY_K] = 'k', [AXKEY_L] = 'l',
    [AXKEY_M] = 'm', [AXKEY_N] = 'n', [AXKEY_O] = 'o', [AXKEY_P] = 'p',
    [AXKEY_Q] = 'q', [AXKEY_R] = 'r', [AXKEY_S] = 's', [AXKEY_T] = 't',
    [AXKEY_U] = 'u', [AXKEY_V] = 'v', [AXKEY_W] = 'w', [AXKEY_X] = 'x',
    [AXKEY_SEMICOLON] = ';', [AXKEY_COMMA] = ',', [AXKEY_DOT] = '.',
    [AXKEY_SLASH] = '/', [AXKEY_SPACE] = ' ', [AXKEY_ENTER] = '\n',
    [AXKEY_MINUS] = '-', [AXKEY_EQUAL] = '=',
    [AXKEY_LBRACKET] = 0, [AXKEY_QUOTE] = 0, [AXKEY_GRAVE] = 0,
};

char key_to_ascii_layout(axkey_t key, u32 modifiers, u8 layout)
{
    u32 index = (u32)key;
    if (index >= AXKEY_MAX) {
        return 0;
    }
    bool_t shift = (modifiers & AXMOD_ANY_SHIFT) != 0;

    if (layout == 2) {
        /* UK: shifted table is the US one with the moved keys corrected. */
        if (shift) {
            switch (key) {
            case AXKEY_2: return '"';
            case AXKEY_3: return 0;
            case AXKEY_QUOTE: return '@';
            case AXKEY_GRAVE: return '~';
            case AXKEY_BACKSLASH: return '~';
            default: break;
            }
            return g_us_shift[index];
        }
        return g_uk_plain[index];
    }
    if (layout == 3) {
        char base = g_de_plain[index];
        if (base >= 'a' && base <= 'z' && shift) {
            return (char)(base - 'a' + 'A');
        }
        if (shift) {
            switch (key) {
            case AXKEY_1: return '!';
            case AXKEY_2: return '"';
            case AXKEY_3: return 0;      /* section sign */
            case AXKEY_4: return '$';
            case AXKEY_5: return '%';
            case AXKEY_6: return '&';
            case AXKEY_7: return '/';
            case AXKEY_8: return '(';
            case AXKEY_9: return ')';
            case AXKEY_0: return '=';
            default: break;
            }
        }
        return base;
    }
    /* US. */
    if (key >= AXKEY_A && key <= AXKEY_Z) {
        char base = g_us_plain[index];
        if (shift != ((modifiers & AXMOD_CAPS) != 0 && !(modifiers & AXMOD_CTRL))) {
            return (char)(base - 'a' + 'A');
        }
        return base;
    }
    if (shift) {
        return g_us_shift[index];
    }
    return g_us_plain[index];
}

char key_to_ascii(axkey_t key, u32 modifiers)
{
    /* The active layout is published by the keyboard layer. */
    extern u8 keyboard_layout_id(void);
    return key_to_ascii_layout(key, modifiers, keyboard_layout_id());
}

/* Symbolic Unicode for the keys that ASCII cannot express.  This is what the
 * GUI uses for key legends and what a text field stores for non-US input. */
u32 key_to_unicode(axkey_t key, u32 modifiers)
{
    u32 base = (u32)key;
    bool_t shift = (modifiers & AXMOD_ANY_SHIFT) != 0;
    if (base >= AXKEY_MAX) {
        return 0;
    }
    char ascii = key_to_ascii(key, modifiers);
    if (ascii) {
        return (u32)ascii;
    }
    switch (key) {
    case AXKEY_LBRACKET: return shift ? 0x00DC : 0x00FC;   /* ue / UE */
    case AXKEY_SEMICOLON: return shift ? 0x00D6 : 0x00F6;  /* oe / OE */
    case AXKEY_QUOTE: return shift ? 0x00C4 : 0x00E4;      /* ae / AE */
    case AXKEY_GRAVE: return shift ? 0x00B0 : 0x00DF;      /* degree / sharp s */
    case AXKEY_3: return 0x00A7;                            /* section */
    case AXKEY_EQUAL: return shift ? 0x00B4 : 0x00B4;
    case AXKEY_MINUS: return shift ? 0x00DF : 0x00DF;
    case AXKEY_BACKSPACE: return 0x0008;
    case AXKEY_TAB: return 0x0009;
    case AXKEY_ENTER: return 0x000A;
    case AXKEY_ESC: return 0x001B;
    default:
        return 0;
    }
}

/* Directional keys and function keys map to their names for display purposes;
 * keeping this here rather than in the GUI means both the terminal and the
 * on-screen keyboard agree. */
const char *key_legend(axkey_t key, u32 modifiers)
{
    static char buffer[8];
    if (modifiers & AXMOD_CTRL) {
        switch (key) {
        case AXKEY_A: return "^A";
        case AXKEY_B: return "^B";
        case AXKEY_C: return "^C";
        case AXKEY_D: return "^D";
        case AXKEY_E: return "^E";
        case AXKEY_F: return "^F";
        case AXKEY_G: return "^G";
        case AXKEY_H: return "^H";
        case AXKEY_I: return "^I";
        case AXKEY_J: return "^J";
        case AXKEY_K: return "^K";
        case AXKEY_L: return "^L";
        case AXKEY_M: return "^M";
        case AXKEY_N: return "^N";
        case AXKEY_O: return "^O";
        case AXKEY_P: return "^P";
        case AXKEY_Q: return "^Q";
        case AXKEY_R: return "^R";
        case AXKEY_S: return "^S";
        case AXKEY_T: return "^T";
        case AXKEY_U: return "^U";
        case AXKEY_V: return "^V";
        case AXKEY_W: return "^W";
        case AXKEY_X: return "^X";
        case AXKEY_Y: return "^Y";
        case AXKEY_Z: return "^Z";
        case AXKEY_LBRACKET: return "^[";
        case AXKEY_BACKSLASH: return "^\\";
        case AXKEY_RBRACKET: return "^]";
        case AXKEY_GRAVE: return "^`";
        default: break;
        }
    }
    char c = key_to_ascii(key, modifiers);
    if (c >= 0x20 && c < 0x7F) {
        buffer[0] = c;
        buffer[1] = 0;
        return buffer;
    }
    return key_name(key);
}

/* Table sizes, exported so selftests and tooling can validate coverage rather
 * than guessing at constants. */
u32 keymap_scancode_table_size(void)
{
    return 128;
}

u32 keymap_keycode_count(void)
{
    return AXKEY_MAX;
}
