/*
 * AxiomOS - keyboard text composition.
 *
 * Between a key event and a character that lands in a text field sit three
 * concerns that the terminal and the GUI both need:
 *
 *   dead keys   an accent key postpones output until the next key arrives;
 *   compose     AltGr and multi-key sequences produce characters ASCII lacks;
 *   editing     the small set of operations a text field performs on a buffer.
 *
 * Keeping them in one module means the terminal and the GUI produce identical
 * text for identical keystrokes, which matters when a user pastes a command
 * from the terminal into a dialog.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axinput.h>

#define TEXT_BUFFER_MAX 512

typedef enum dead_key {
    DEAD_NONE = 0,
    DEAD_ACUTE,
    DEAD_GRAVE,
    DEAD_CIRCUMFLEX,
    DEAD_TILDE,
    DEAD_DIAERESIS,
    DEAD_CEDILLA,
    DEAD_RING,
    DEAD_STROKE
} dead_key_t;

typedef struct compose_state {
    dead_key_t pending;
    u32 pending_since_ns;
    u32 compose_keys[4];
    u32 compose_len;
    u32 dead_applied;
    u32 dead_dropped;
    u32 utf8_fallbacks;
} compose_state_t;

static compose_state_t g_compose;

/* ------------------------------------------------------------------ */
/* Dead key mapping                                                    */
/* ------------------------------------------------------------------ */

/* Base letter for each accent.  A zero entry means the accent has no precomposed
 * form for that letter and the caller must emit the accent plus the base letter
 * separately.  The table is indexed by lower-case letter position a..z. */
typedef struct accent_row {
    dead_key_t key;
    u32 upper[32];
    u32 lower[32];
} accent_row_t;

static const accent_row_t g_accents[] = {
    {DEAD_ACUTE,
     {0x00C0, 0, 0x0106, 0, 0x00C9, 0, 0x01F4, 0, 0x00CD, 0, 0, 0,
      0, 0x0143, 0x00D3, 0x1E4C, 0, 0x0154, 0x015E, 0x0164, 0x00DA, 0, 0x1E82,
      0, 0x00DD, 0x0179},
     {0x00E0, 0, 0x0107, 0, 0x00E9, 0, 0x01F5, 0, 0x00ED, 0, 0, 0,
      0, 0x0144, 0x00F3, 0x1E4D, 0, 0x0155, 0x015F, 0x0165, 0x00FA, 0, 0x1E83,
      0, 0x00FD, 0x017A}},
    {DEAD_GRAVE,
     {0x00C0, 0, 0, 0, 0x00C8, 0, 0, 0, 0x00CC, 0, 0, 0,
      0, 0, 0x00D2, 0, 0, 0, 0, 0, 0x00D9, 0, 0, 0,
      0, 0},
     {0x00E0, 0, 0, 0, 0x00E8, 0, 0, 0, 0x00EC, 0, 0, 0,
      0, 0, 0x00F2, 0, 0, 0, 0, 0, 0x00F9, 0, 0, 0,
      0, 0}},
    {DEAD_CIRCUMFLEX,
     {0x00C2, 0, 0x0108, 0, 0x00CA, 0, 0x011C, 0x0124, 0x00CE, 0x0134, 0, 0,
      0, 0, 0x00D4, 0, 0, 0, 0x015C, 0, 0x00DB, 0, 0x0174, 0,
      0x0176, 0x1E90},
     {0x00E2, 0, 0x0109, 0, 0x00EA, 0, 0x011D, 0x0125, 0x00EE, 0x0135, 0, 0,
      0, 0, 0x00F4, 0, 0, 0, 0x015D, 0, 0x00FB, 0, 0x0175, 0,
      0x0177, 0x1E91}},
    {DEAD_TILDE,
     {0x00C0, 0, 0, 0, 0x1EBC, 0, 0, 0, 0x0128, 0, 0, 0,
      0, 0x00D5, 0x00D4, 0, 0, 0, 0, 0, 0x0168, 0, 0, 0,
      0, 0},
     {0x00E0, 0, 0, 0, 0x1EBD, 0, 0, 0, 0x0129, 0, 0, 0,
      0, 0x00F5, 0x00F4, 0, 0, 0, 0, 0, 0x0169, 0, 0, 0,
      0, 0}},
    {DEAD_DIAERESIS,
     {0x00C4, 0, 0, 0, 0x00CB, 0, 0, 0, 0x00CF, 0, 0, 0,
      0, 0, 0x00D6, 0, 0, 0, 0, 0, 0x00DC, 0, 0, 0,
      0x0178, 0},
     {0x00E4, 0, 0, 0, 0x00EB, 0, 0, 0, 0x00EF, 0, 0, 0,
      0, 0, 0x00F6, 0, 0, 0, 0, 0, 0x00FC, 0, 0, 0,
      0x00FF, 0}},
    {DEAD_CEDILLA,
     {0, 0, 0x00C7, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0x015E, 0, 0, 0, 0, 0,
      0, 0},
     {0, 0, 0x00E7, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0x015F, 0, 0, 0, 0, 0,
      0, 0}},
    {DEAD_RING,
     {0x00C5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0x016E, 0, 0, 0,
      0, 0},
     {0x00E5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0x016F, 0, 0, 0,
      0, 0}},
    {DEAD_STROKE,
     {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0x00D8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0},
     {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0x00F8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0}},
};

static const accent_row_t *find_accent(dead_key_t key)
{
    for (u32 i = 0; i < sizeof(g_accents) / sizeof(g_accents[0]); i++) {
        if (g_accents[i].key == key) {
            return &g_accents[i];
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Composition                                                         */
/* ------------------------------------------------------------------ */

void kbdtext_init(void)
{
    ax_memset(&g_compose, 0, sizeof(g_compose));
}

/* Translate a letter key to the accented form for the pending dead key.
 * Returns 0 when there is no precomposed character. */
static u32 apply_accent(dead_key_t accent, axkey_t key, u32 modifiers)
{
    const accent_row_t *row = find_accent(accent);
    if (!row) {
        return 0;
    }
    char base = key_to_ascii(key, modifiers);
    if (base == 0) {
        return 0;
    }
    bool_t upper = (bool_t)(base >= 'A' && base <= 'Z');
    char lower = upper ? (char)(base - 'A' + 'a') : base;
    if (lower < 'a' || lower > 'z') {
        return 0;
    }
    u32 index = (u32)(lower - 'a');
    return upper ? row->upper[index] : row->lower[index];
}

/* Feed one key press and get zero, one or two code points back.  The caller
 * appends them to a UTF-8 buffer.  `out_count` is the number of valid entries
 * in `out`. */
u32 kbdtext_key(axkey_t key, u32 modifiers, u32 *out, u32 out_cap,
                u64 now_ns)
{
    if (!out || out_cap == 0) {
        return 0;
    }
    u32 count = 0;

    /* A dead key followed by a non-letter emits the accent's spacing form and
     * then the key's own character, which is what users expect when they press
     * accent then space. */
    if (g_compose.pending != DEAD_NONE) {
        dead_key_t accent = g_compose.pending;
        g_compose.pending = DEAD_NONE;
        if (key == AXKEY_SPACE) {
            static const u32 spacing[] = {
                0x00B4, 0x0060, 0x02C6, 0x02DC, 0x00A8,
                0x00B8, 0x02DA, 0x00AF,
            };
            u32 index = (u32)accent - 1;
            if (index < sizeof(spacing) / sizeof(spacing[0])) {
                out[0] = spacing[index];
                g_compose.dead_applied++;
                return 1;
            }
        }
        u32 composed = apply_accent(accent, key, modifiers);
        if (composed) {
            out[0] = composed;
            g_compose.dead_applied++;
            return 1;
        }
        g_compose.dead_dropped++;
    }

    /* AltGr on the layouts we ship turns some keys into symbols; the mapping is
     * per-layout and deliberately small. */
    if (modifiers & AXMOD_ALTGR) {
        switch (key) {
        case AXKEY_Q: out[count++] = '@'; return count;
        case AXKEY_E: out[count++] = 0x20AC; return count;  /* euro */
        case AXKEY_7: out[count++] = '{'; return count;
        case AXKEY_8: out[count++] = '['; return count;
        case AXKEY_9: out[count++] = ']'; return count;
        case AXKEY_0: out[count++] = '}'; return count;
        case AXKEY_MINUS: out[count++] = '\\'; return count;
        default: break;
        }
    }

    switch (key) {
    case AXKEY_GRAVE:
        if (modifiers & AXMOD_ALTGR) {
            g_compose.pending = DEAD_GRAVE;
            g_compose.pending_since_ns = (u32)now_ns;
            return 0;
        }
        break;
    case AXKEY_QUOTE:
        if (modifiers & AXMOD_ALTGR) {
            g_compose.pending = DEAD_ACUTE;
            g_compose.pending_since_ns = (u32)now_ns;
            return 0;
        }
        break;
    case AXKEY_6:
        if (modifiers & AXMOD_ALTGR) {
            g_compose.pending = DEAD_CIRCUMFLEX;
            g_compose.pending_since_ns = (u32)now_ns;
            return 0;
        }
        break;
    default:
        break;
    }

    u32 code = key_to_unicode(key, modifiers);
    if (code == 0) {
        return 0;
    }
    out[count++] = code;
    (void)now_ns;
    return count;
}

/* Record a multi-key compose sequence (for example Ctrl-K then a vowel on some
 * terminal layouts).  Returns 1 when the sequence completed and produced a
 * code point. */
u32 kbdtext_compose_key(u32 code)
{
    if (g_compose.compose_len >= 4) {
        g_compose.compose_len = 0;
    }
    g_compose.compose_keys[g_compose.compose_len++] = code;
    if (g_compose.compose_len == 2) {
        u32 first = g_compose.compose_keys[0];
        u32 second = g_compose.compose_keys[1];
        g_compose.compose_len = 0;
        /* The classic two-key sequences that do not have dead-key forms. */
        if (first == 'a' && second == 'e') return 0x00E6;
        if (first == 'A' && second == 'E') return 0x00C6;
        if (first == 'o' && second == 'e') return 0x0153;
        if (first == 'O' && second == 'E') return 0x0152;
        if (first == 's' && second == 's') return 0x00DF;
        if (first == 'i' && second == 'j') return 0x0133;
        return 0;
    }
    return 0;
}

void kbdtext_reset(void)
{
    g_compose.pending = DEAD_NONE;
    g_compose.compose_len = 0;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

/* Encode one code point.  Returns the number of bytes written (1..4) or 0 when
 * the buffer is too small.  Surrogates and out-of-range values are rejected,
 * because a text field that stores them will fail to render later. */
u32 utf8_encode(u32 code, u8 *out, u32 cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    if (code <= 0x7F) {
        out[0] = (u8)code;
        return 1;
    }
    if (code <= 0x7FF) {
        if (cap < 2) return 0;
        out[0] = (u8)(0xC0 | (code >> 6));
        out[1] = (u8)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code >= 0xD800 && code <= 0xDFFF) {
        return 0;
    }
    if (code <= 0xFFFF) {
        if (cap < 3) return 0;
        out[0] = (u8)(0xE0 | (code >> 12));
        out[1] = (u8)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (code & 0x3F));
        return 3;
    }
    if (code <= 0x10FFFF) {
        if (cap < 4) return 0;
        out[0] = (u8)(0xF0 | (code >> 18));
        out[1] = (u8)(0x80 | ((code >> 12) & 0x3F));
        out[2] = (u8)(0x80 | ((code >> 6) & 0x3F));
        out[3] = (u8)(0x80 | (code & 0x3F));
        return 4;
    }
    return 0;
}

u32 utf8_decode(const u8 *data, u32 length, u32 *code)
{
    if (!data || !length || !code) {
        return 0;
    }
    u8 first = data[0];
    if (first < 0x80) {
        *code = first;
        return 1;
    }
    u32 need = 0;
    u32 value = 0;
    if ((first & 0xE0) == 0xC0) {
        need = 2;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        need = 3;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        need = 4;
        value = first & 0x07;
    } else {
        return 0;
    }
    if (length < need) {
        return 0;
    }
    for (u32 i = 1; i < need; i++) {
        if ((data[i] & 0xC0) != 0x80) {
            return 0;
        }
        value = (value << 6) | (data[i] & 0x3F);
    }
    /* Reject overlong encodings: a 3-byte form of 'a' is a protocol error. */
    if (need == 2 && value < 0x80) return 0;
    if (need == 3 && value < 0x800) return 0;
    if (need == 4 && value < 0x10000) return 0;
    if (value > 0x10FFFF) return 0;
    *code = value;
    return need;
}

/* ------------------------------------------------------------------ */
/* Text buffer editing                                                 */
/* ------------------------------------------------------------------ */

typedef struct text_buffer {
    char bytes[TEXT_BUFFER_MAX];
    u32 length;
    u32 cursor;
    u32 selection_start;
    u32 selection_end;
    u32 edits;
} text_buffer_t;

void text_buffer_init(text_buffer_t *buffer)
{
    if (!buffer) return;
    ax_memset(buffer, 0, sizeof(*buffer));
}

bool_t text_buffer_insert(text_buffer_t *buffer, char c)
{
    if (!buffer || buffer->length >= TEXT_BUFFER_MAX - 1) {
        return (bool_t)0;
    }
    if (buffer->cursor > buffer->length) {
        buffer->cursor = buffer->length;
    }
    for (u32 i = buffer->length; i > buffer->cursor; i--) {
        buffer->bytes[i] = buffer->bytes[i - 1];
    }
    buffer->bytes[buffer->cursor] = c;
    buffer->cursor++;
    buffer->length++;
    buffer->bytes[buffer->length] = 0;
    buffer->edits++;
    return (bool_t)1;
}

bool_t text_buffer_backspace(text_buffer_t *buffer)
{
    if (!buffer || buffer->cursor == 0) {
        return (bool_t)0;
    }
    for (u32 i = buffer->cursor - 1; i < buffer->length; i++) {
        buffer->bytes[i] = buffer->bytes[i + 1];
    }
    buffer->cursor--;
    buffer->length--;
    buffer->edits++;
    return (bool_t)1;
}

bool_t text_buffer_delete(text_buffer_t *buffer)
{
    if (!buffer || buffer->cursor >= buffer->length) {
        return (bool_t)0;
    }
    for (u32 i = buffer->cursor; i < buffer->length; i++) {
        buffer->bytes[i] = buffer->bytes[i + 1];
    }
    buffer->length--;
    buffer->edits++;
    return (bool_t)1;
}

void text_buffer_move(text_buffer_t *buffer, i32 delta)
{
    if (!buffer) return;
    i32 target = (i32)buffer->cursor + delta;
    if (target < 0) target = 0;
    if (target > (i32)buffer->length) target = (i32)buffer->length;
    buffer->cursor = (u32)target;
}

bool_t text_buffer_insert_code(text_buffer_t *buffer, u32 code)
{
    u8 bytes[4];
    u32 count = utf8_encode(code, bytes, 4);
    if (count == 0) {
        return (bool_t)0;
    }
    for (u32 i = 0; i < count; i++) {
        if (!text_buffer_insert(buffer, (char)bytes[i])) {
            return (bool_t)0;
        }
    }
    return (bool_t)1;
}

u32 text_buffer_length(const text_buffer_t *buffer)
{
    return buffer ? buffer->length : 0;
}

u32 text_buffer_cursor(const text_buffer_t *buffer)
{
    return buffer ? buffer->cursor : 0;
}

void text_buffer_clear(text_buffer_t *buffer)
{
    if (!buffer) return;
    buffer->length = 0;
    buffer->cursor = 0;
    buffer->bytes[0] = 0;
}

const char *text_buffer_data(const text_buffer_t *buffer)
{
    return buffer ? buffer->bytes : "";
}

void text_buffer_stats(const text_buffer_t *buffer, char *out, u32 cap)
{
    if (!buffer || !out || !cap) return;
    ax_snprintf(out, cap, "text: %u/%u bytes, cursor %u, %u edits",
                buffer->length, (u32)TEXT_BUFFER_MAX, buffer->cursor,
                buffer->edits);
}

void kbdtext_dump(void)
{
    printk("compose: dead=%u applied=%u dropped=%u compose_len=%u fallbacks=%u\n",
           (u32)g_compose.pending, g_compose.dead_applied,
           g_compose.dead_dropped, g_compose.compose_len,
           g_compose.utf8_fallbacks);
}
