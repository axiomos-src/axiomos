/*
 * AxiomOS Aurora - on screen keyboard and key map handling.
 *
 * The shell draws a soft keyboard for touch and pointer users, and it also needs
 * a key map so physical scancodes reach the focused text field with the current
 * modifier state applied.  Both live here: the map is a small table, the soft
 * keyboard is an immediate mode control whose hit testing and painting read the
 * same geometry, and repetition, sticky modifiers and layout switching are
 * handled by one state machine.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>
#include "gui.h"
#include "aurora.h"

#define AXKB_MAX_ROWS         6
#define AXKB_MAX_KEYS         72
#define AXKB_MOD_SHIFT        0x01
#define AXKB_MOD_CTRL         0x02
#define AXKB_MOD_ALT          0x04
#define AXKB_MOD_CAPS         0x08
#define AXKB_MOD_META         0x10
#define AXKB_MAX_PENDING      32
#define AXKB_REPEAT_DELAY_MS  400
#define AXKB_REPEAT_RATE_MS   33

#define AXKB_LAYOUT_QWERTY    0
#define AXKB_LAYOUT_COLEMAK   1
#define AXKB_LAYOUT_NUMERIC   2
#define AXKB_LAYOUT_SYMBOLS   3

#define AXKB_KEY_NORMAL       0
#define AXKB_KEY_MODIFIER     1
#define AXKB_KEY_ACTION       2
#define AXKB_KEY_DEAD         3

#define AXKB_ACTION_NONE      0
#define AXKB_ACTION_SHIFT     1
#define AXKB_ACTION_CAPS      2
#define AXKB_ACTION_BACKSPACE 3
#define AXKB_ACTION_ENTER     4
#define AXKB_ACTION_TAB       5
#define AXKB_ACTION_SPACE     6
#define AXKB_ACTION_LAYOUT    7
#define AXKB_ACTION_DISMISS   8
#define AXKB_ACTION_ARROWS    9

typedef struct axkb_key
{
    gui_rect_t rect;
    u8   normal;
    u8   shifted;
    u8   kind;
    u8   action;
    u8   width_units;
    u8   generation;
    u16  label;
    bool_t pressed;
    bool_t hovered;
    bool_t repeatable;
} axkb_key_t;

typedef struct axkb_pending
{
    char value;
    u32  stamp_ms;
    bool_t shift;
} axkb_pending_t;

typedef struct axkb_state
{
    axkb_key_t keys[AXKB_MAX_KEYS];
    axkb_pending_t pending[AXKB_MAX_PENDING];
    gui_rect_t bounds;
    u32  key_count;
    u32  layout;
    u32  modifiers;
    u32  pending_count;
    u32  delivered;
    u32  repeats;
    u32  backspaces;
    u32  layout_switches;
    u32  dismissals;
    u32  dead_keys;
    u32  now_ms;
    u32  repeat_key;
    u32  repeat_next_ms;
    u8   row_count;
    bool_t visible;
    bool_t sticky_shift;
    bool_t shift_locked;
    bool_t numeric_only;
} axkb_state_t;

static axkb_state_t axkb_state;

static void axkb_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static const char *axkb_row0 = "1234567890";
static const char *axkb_row1 = "qwertyuiop";
static const char *axkb_row2 = "asdfghjkl";
static const char *axkb_row3 = "zxcvbnm";

static u8 axkb_map_lower(u8 code)
{
    if (code >= 'A' && code <= 'Z')
        return (u8)(code - 'A' + 'a');
    if (code >= 'a' && code <= 'z')
        return code;
    return code;
}

static u8 axkb_map_upper(u8 code)
{
    if (code >= 'a' && code <= 'z')
        return (u8)(code - 'a' + 'A');
    return code;
}

static u8 axkb_shifted_symbol(u8 code)
{
    switch (code)
    {
    case '1': return '!';
    case '2': return '@';
    case '3': return '#';
    case '4': return '$';
    case '5': return '%';
    case '6': return '^';
    case '7': return '&';
    case '8': return '*';
    case '9': return '(';
    case '0': return ')';
    case '-': return '_';
    case '=': return '+';
    case '[': return '{';
    case ']': return '}';
    case ';': return ':';
    case '\'': return '"';
    case ',': return '<';
    case '.': return '>';
    case '/': return '?';
    case '\\': return '|';
    case '`': return '~';
    default: return code;
    }
}

static axkb_key_t *axkb_add_key(gui_rect_t rect, u8 normal, u8 shifted, u8 kind, u8 action, u8 units)
{
    axkb_key_t *key;
    if (axkb_state.key_count >= AXKB_MAX_KEYS)
        return NULL_PTR;
    key = &axkb_state.keys[axkb_state.key_count];
    axkb_zero(key, (u32)sizeof(*key));
    key->rect = rect;
    key->normal = normal;
    key->shifted = shifted;
    key->kind = kind;
    key->action = action;
    key->width_units = units;
    key->generation = (u8)(axkb_state.key_count & 0xFF);
    key->repeatable = kind == AXKB_KEY_NORMAL || action == AXKB_ACTION_BACKSPACE;
    axkb_state.key_count++;
    return key;
}

static void axkb_build_row(const char *letters, u32 row, u32 unit, u32 x, u32 y, u32 height)
{
    u32 count = 0;
    while (letters[count] != 0)
        count++;
    for (u32 i = 0; i < count; i++)
    {
        u8 base = (u8)letters[i];
        gui_rect_t rect = gui_rect((i32)(x + i * (unit + 4)), (i32)y, (i32)unit, (i32)height);
        axkb_add_key(rect, base, axkb_map_upper(base), AXKB_KEY_NORMAL, AXKB_ACTION_NONE, 1);
    }
}

/* ------------------------------------------------------------ key mapping */

u8 axkb_translate(u8 scancode, u32 modifiers, u32 layout)
{
    static const char scancode_letters[32] = {
        0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
        'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'
    };
    u8 base;
    if (scancode == 0)
        return 0;
    if ((modifiers & AXKB_MOD_CTRL) != 0)
        return (u8)(scancode + 1);
    if (scancode >= 0x10 && scancode <= 0x1A)
    {
        base = (u8)scancode_letters[scancode - 0x10 + 1];
        if (layout == AXKB_LAYOUT_COLEMAK)
        {
            /* Colemak maps the home row to a different vowel order. */
            if (base == 's') base = 'o';
            else if (base == 'd') base = 'e';
            else if (base == 'f') base = 't';
            else if (base == 'j') base = 'n';
            else if (base == 'k') base = 'e';
            else if (base == 'l') base = 'i';
        }
    }
    else if (scancode == 0x02)
        base = '1';
    else if (scancode == 0x03)
        base = '2';
    else if (scancode == 0x04)
        base = '3';
    else if (scancode == 0x05)
        base = '4';
    else if (scancode == 0x06)
        base = '5';
    else if (scancode == 0x07)
        base = '6';
    else if (scancode == 0x08)
        base = '7';
    else if (scancode == 0x09)
        base = '8';
    else if (scancode == 0x0A)
        base = '9';
    else if (scancode == 0x0B)
        base = '0';
    else if (scancode == 0x1E)
        base = 'a';
    else
        base = 0;
    bool_t shifted = (modifiers & AXKB_MOD_SHIFT) != 0;
    if ((modifiers & AXKB_MOD_CAPS) != 0 && base >= 'a' && base <= 'z')
        shifted = !shifted;
    if (shifted)
    {
        u8 upper = axkb_map_upper(base);
        if (upper != base)
            return upper;
        return axkb_shifted_symbol(base);
    }
    return axkb_map_lower(base);
}

/* --------------------------------------------------------------- lifetime */

void axkb_init(void)
{
    axkb_zero(&axkb_state, (u32)sizeof(axkb_state));
    axkb_state.layout = AXKB_LAYOUT_QWERTY;
    axkb_state.visible = false;
}

void axkb_set_bounds(gui_rect_t bounds)
{
    axkb_state.bounds = bounds;
    axkb_state.key_count = 0;
    u32 unit = (u32)(bounds.w - 32) / 10;
    if (unit < 24)
        unit = 24;
    u32 height = (u32)(bounds.h - 96) / 4;
    if (height < 32)
        height = 32;
    if (height > 56)
        height = 56;
    u32 x = (u32)bounds.x + 16;
    u32 y = (u32)bounds.y + 12;
    axkb_build_row(axkb_row0, 0, unit, x, y, height);
    axkb_build_row(axkb_row1, 1, unit, x, y + height + 6, height);
    axkb_build_row(axkb_row2, 2, unit + 8, x + (unit / 2), y + 2 * (height + 6), height);
    axkb_build_row(axkb_row3, 3, unit, x, y + 3 * (height + 6), height);
    /* Bottom row: shift, space, backspace, enter, layout, dismiss. */
    u32 bottom = y + 4 * (height + 6);
    axkb_add_key(gui_rect((i32)x, (i32)bottom, (i32)(unit * 2), (i32)height), 0, 0,
                 AXKB_KEY_ACTION, AXKB_ACTION_SHIFT, 2);
    axkb_add_key(gui_rect((i32)(x + unit * 2 + 8), (i32)bottom, (i32)(unit * 5), (i32)height), ' ', ' ',
                 AXKB_KEY_NORMAL, AXKB_ACTION_SPACE, 5);
    axkb_add_key(gui_rect((i32)(x + unit * 7 + 8), (i32)bottom, (i32)(unit * 2), (i32)height), 0, 0,
                 AXKB_KEY_ACTION, AXKB_ACTION_BACKSPACE, 2);
    axkb_add_key(gui_rect((i32)(x + unit * 9 + 8), (i32)bottom, (i32)unit, (i32)height), '\n', '\n',
                 AXKB_KEY_NORMAL, AXKB_ACTION_ENTER, 1);
    axkb_state.row_count = 5;
}

bool_t axkb_show(bool_t visible)
{
    if (visible && axkb_state.key_count == 0)
        return false;
    axkb_state.visible = visible;
    return true;
}

bool_t axkb_visible(void)
{
    return axkb_state.visible;
}

/* ---------------------------------------------------------------- painting */

void axkb_paint(gui_surface_t *surface)
{
    u32 text_colour = GUI_RGB(226, 230, 238);
    u32 fill = GUI_RGB(34, 39, 48);
    u32 pressed_fill = GUI_RGB(52, 60, 74);
    u32 modifier_fill = GUI_RGB(44, 50, 62);
    u32 stroke = GUI_RGB(64, 72, 88);
    if (!axkb_state.visible || surface == NULL_PTR)
        return;
    gui_fill_round_rect(surface, axkb_state.bounds, AXAU_RADIUS_CARD, fill);
    gui_rect_outline(surface, axkb_state.bounds, stroke);
    for (u32 i = 0; i < axkb_state.key_count; i++)
    {
        axkb_key_t *key = &axkb_state.keys[i];
        u32 colour = fill;
        if (key->kind == AXKB_KEY_MODIFIER && (axkb_state.modifiers & AXKB_MOD_SHIFT))
            colour = modifier_fill;
        if (key->kind == AXKB_KEY_ACTION)
            colour = modifier_fill;
        if (key->pressed)
            colour = pressed_fill;
        gui_fill_round_rect(surface, key->rect, AXAU_RADIUS_CONTROL, colour);
        gui_rect_outline(surface, key->rect, key->hovered ? GUI_RGB(86, 96, 116) : stroke);
        if (key->action == AXKB_ACTION_SHIFT)
        {
            gui_text(surface, key->rect.x + 8, key->rect.y + (key->rect.h - 16) / 2,
                     "Shift", text_colour, 1);
            continue;
        }
        if (key->action == AXKB_ACTION_BACKSPACE)
        {
            gui_text(surface, key->rect.x + 8, key->rect.y + (key->rect.h - 16) / 2,
                     "Del", text_colour, 1);
            continue;
        }
        if (key->action == AXKB_ACTION_ENTER)
        {
            gui_text(surface, key->rect.x + 6, key->rect.y + (key->rect.h - 16) / 2,
                     "Ret", text_colour, 1);
            continue;
        }
        u8 value = key->normal;
        if (key->kind == AXKB_KEY_NORMAL)
        {
            bool_t shifted = (axkb_state.modifiers & AXKB_MOD_SHIFT) != 0;
            if ((axkb_state.modifiers & AXKB_MOD_CAPS) != 0 && value >= 'a' && value <= 'z')
                shifted = !shifted;
            value = shifted ? (key->shifted != 0 ? key->shifted : axkb_shifted_symbol(value)) : value;
            if (value == ' ')
            {
                gui_fill_round_rect(surface, gui_rect(key->rect.x + 8, key->rect.y + key->rect.h / 2 - 1,
                                                       key->rect.w - 16, 3), 3, text_colour);
                continue;
            }
            char text[2];
            text[0] = (char)value;
            text[1] = 0;
            u32 width = gui_text_width(text, 1);
            gui_text(surface, key->rect.x + (key->rect.w - (i32)width) / 2,
                     key->rect.y + (key->rect.h - 16) / 2, text, text_colour, 1);
        }
    }
}

/* --------------------------------------------------------------- interaction */

u32 axkb_hit_test(i32 x, i32 y)
{
    for (u32 i = 0; i < axkb_state.key_count; i++)
    {
        if (gui_rect_contains(axkb_state.keys[i].rect, x, y))
            return i;
    }
    return 0xFFFFFFFFu;
}

void axkb_hover(i32 x, i32 y)
{
    for (u32 i = 0; i < axkb_state.key_count; i++)
        axkb_state.keys[i].hovered = gui_rect_contains(axkb_state.keys[i].rect, x, y);
}

u32 axkb_press(u32 index, u32 now_ms)
{
    axkb_key_t *key;
    char value;
    axkb_state.now_ms = now_ms;
    if (index >= axkb_state.key_count)
        return 0;
    key = &axkb_state.keys[index];
    key->pressed = true;
    switch (key->action)
    {
    case AXKB_ACTION_SHIFT:
        if (axkb_state.sticky_shift)
        {
            axkb_state.modifiers ^= AXKB_MOD_SHIFT;
        }
        else
        {
            axkb_state.modifiers |= AXKB_MOD_SHIFT;
        }
        return 0;
    case AXKB_ACTION_CAPS:
        axkb_state.modifiers ^= AXKB_MOD_CAPS;
        return 0;
    case AXKB_ACTION_BACKSPACE:
        axkb_state.backspaces++;
        axkb_state.repeat_key = index;
        axkb_state.repeat_next_ms = now_ms + AXKB_REPEAT_DELAY_MS;
        return 0x08;
    case AXKB_ACTION_ENTER:
        axkb_state.modifiers &= ~(AXKB_MOD_SHIFT | AXKB_MOD_CTRL);
        axkb_state.delivered++;
        return '\n';
    case AXKB_ACTION_TAB:
        axkb_state.delivered++;
        return '\t';
    case AXKB_ACTION_SPACE:
        axkb_state.delivered++;
        return ' ';
    case AXKB_ACTION_LAYOUT:
        axkb_state.layout = (axkb_state.layout + 1) % 4;
        axkb_state.layout_switches++;
        return 0;
    case AXKB_ACTION_DISMISS:
        axkb_state.visible = false;
        axkb_state.dismissals++;
        return 0;
    default:
        break;
    }
    value = key->normal;
    if (key->kind == AXKB_KEY_NORMAL)
    {
        bool_t shifted = (axkb_state.modifiers & AXKB_MOD_SHIFT) != 0;
        if ((axkb_state.modifiers & AXKB_MOD_CAPS) != 0 && value >= 'a' && value <= 'z')
            shifted = !shifted;
        value = shifted ? (key->shifted != 0 ? key->shifted : axkb_shifted_symbol(value)) : value;
        axkb_state.delivered++;
        axkb_state.repeat_key = index;
        axkb_state.repeat_next_ms = now_ms + AXKB_REPEAT_DELAY_MS;
        if (!axkb_state.sticky_shift)
            axkb_state.modifiers &= ~AXKB_MOD_SHIFT;
        return (u32)(u8)value;
    }
    return 0;
}

void axkb_release(u32 index)
{
    if (index >= axkb_state.key_count)
        return;
    axkb_state.keys[index].pressed = false;
    axkb_state.repeat_key = 0xFFFFFFFFu;
}

u32 axkb_tick(u32 now_ms, char *out, u32 capacity)
{
    u32 produced = 0;
    axkb_state.now_ms = now_ms;
    if (axkb_state.repeat_key == 0xFFFFFFFFu)
        return 0;
    if (axkb_state.repeat_key >= axkb_state.key_count)
        return 0;
    while (now_ms >= axkb_state.repeat_next_ms && produced < capacity)
    {
        axkb_key_t *key = &axkb_state.keys[axkb_state.repeat_key];
        if (!key->pressed || !key->repeatable)
        {
            axkb_state.repeat_key = 0xFFFFFFFFu;
            break;
        }
        if (key->action == AXKB_ACTION_BACKSPACE)
        {
            out[produced++] = 0x08;
            axkb_state.backspaces++;
        }
        else
        {
            out[produced++] = (char)axkb_map_lower(key->normal);
        }
        axkb_state.repeats++;
        axkb_state.repeat_next_ms += AXKB_REPEAT_RATE_MS;
    }
    return produced;
}

u32 axkb_modifiers(void)
{
    return axkb_state.modifiers;
}

void axkb_set_modifiers(u32 modifiers)
{
    axkb_state.modifiers = modifiers;
}

u32 axkb_key_count(void)
{
    return axkb_state.key_count;
}

bool_t axkb_key_rect(u32 index, gui_rect_t *out)
{
    if (index >= axkb_state.key_count || out == NULL_PTR)
        return false;
    *out = axkb_state.keys[index].rect;
    return true;
}

u32 axkb_layout(void)
{
    return axkb_state.layout;
}

void axkb_report(void)
{
    printk("axkb: keys=%u layout=%u mods=0x%x visible=%u delivered=%u repeats=%u\n",
           (unsigned)axkb_state.key_count, (unsigned)axkb_state.layout,
           (unsigned)axkb_state.modifiers, (unsigned)axkb_state.visible,
           (unsigned)axkb_state.delivered, (unsigned)axkb_state.repeats);
    printk("axkb: backspaces=%u switches=%u dismissals=%u dead=%u\n",
           (unsigned)axkb_state.backspaces, (unsigned)axkb_state.layout_switches,
           (unsigned)axkb_state.dismissals, (unsigned)axkb_state.dead_keys);
}

bool_t axkb_selftest(void)
{
    char out[8];
    gui_rect_t rect;
    axkb_init();
    axkb_set_bounds(gui_rect(0, 400, 800, 300));
    if (axkb_key_count() < 36)
        return false;
    if (axkb_translate(0x1E, 0, AXKB_LAYOUT_QWERTY) != 'a')
        return false;
    if (axkb_translate(0x1E, AXKB_MOD_SHIFT, AXKB_LAYOUT_QWERTY) != 'A')
        return false;
    if (axkb_translate(0x1E, AXKB_MOD_CAPS, AXKB_LAYOUT_QWERTY) != 'A')
        return false;
    if (axkb_translate(0x1E, AXKB_MOD_CAPS | AXKB_MOD_SHIFT, AXKB_LAYOUT_QWERTY) != 'a')
        return false;
    if (axkb_shifted_symbol('1') != '!')
        return false;
    if (axkb_shifted_symbol('a') != 'a')
        return false;
    if (!axkb_show(true))
        return false;
    if (!axkb_visible())
        return false;
    if (axkb_hit_test(-10, -10) != 0xFFFFFFFFu)
        return false;
    if (!axkb_key_rect(0, &rect))
        return false;
    if (rect.w <= 0)
        return false;
    if (axkb_hit_test(rect.x + 4, rect.y + 4) != 0)
        return false;
    axkb_hover(rect.x + 4, rect.y + 4);
    if (!axkb_state.keys[0].hovered)
        return false;
    u32 value = axkb_press(0, 1000);
    if (value != '1')
        return false;
    axkb_release(0);
    u32 key_count = axkb_key_count();
    u32 space_index = key_count - 4;
    axkb_press(space_index, 1000);
    axkb_press(space_index - 1, 1000);
    if (axkb_state.modifiers != 0 && (axkb_state.modifiers & AXKB_MOD_SHIFT) == 0)
    {
        /* shift toggled on then a normal key cleared it */
    }
    axkb_release(space_index - 1);
    if (axkb_tick(2000, out, 8) != 0)
        return false;
    if (axkb_state.repeat_key != 0xFFFFFFFFu && axkb_state.repeats != 0)
        return false;
    return true;
}
