/* AxiomOS terminal - escape sequence parser.
 *
 * A byte-fed state machine in the spirit of Paul Williams' DEC ANSI parser,
 * simplified to the states AxiomOS actually needs.  Every byte goes through
 * axterm_feed(); the ground state prints, the CSI states accumulate parameters
 * and dispatch on the final byte, and OSC strings terminate on BEL or ST.
 *
 * Supported:
 *
 *   C0    BEL BS HT LF VT FF CR SO SI ESC DEL
 *   ESC   D E H M N O 7 8 c = > ( ) * + # % 6 5
 *   CSI   @ A B C D E F G H I J K L M P S T X Z ` a b c d e f g h l m n r s
 *         t u  (plus the DEC private ? forms)
 *   OSC   0 1 2 4 7 8 10 11 12 104 110 111 112
 *
 * Anything unrecognised is consumed and counted, never printed, so a stray
 * sequence cannot corrupt the grid.
 */

#include <axiom/lib.h>
#include "term.h"

/* ------------------------------------------------------------------ */
/* Parameter helpers                                                   */
/* ------------------------------------------------------------------ */

static i32 param(const term_t *term, i32 index, i32 fallback)
{
    if (index >= term->csi_param_count) {
        return fallback;
    }
    i32 value = term->csi_params[index];
    return value == 0 ? fallback : value;
}

/* A zero parameter means "one" for every cursor motion command. */
static i32 param1(const term_t *term, i32 index)
{
    return param(term, index, 1);
}

static void param_reset(term_t *term)
{
    term->csi_param_count = 0;
    for (i32 i = 0; i < TERM_MAX_PARAMS; i++) {
        term->csi_params[i] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* SGR                                                                 */
/* ------------------------------------------------------------------ */

/* Parse 38;5;n / 48;5;n / 38;2;r;g;b / 48;2;r;g;b starting at `index`.
 * Returns the number of parameters consumed (0 if the form was malformed). */
static i32 sgr_extended(term_t *term, i32 index, bool_t foreground)
{
    if (index >= term->csi_param_count) {
        return 0;
    }
    i32 kind = term->csi_params[index];
    if (kind == 5) {
        if (index + 1 >= term->csi_param_count) {
            return 0;
        }
        i32 entry = term->csi_params[index + 1];
        u32 colour = axterm_xterm256(entry);
        if (foreground) {
            term->cursor.fg = colour;
        } else {
            term->cursor.bg = colour;
        }
        return 2;
    }
    if (kind == 2) {
        if (index + 3 >= term->csi_param_count) {
            return 0;
        }
        u8 r = (u8)axui_clamp_i32(term->csi_params[index + 1], 0, 255);
        u8 g = (u8)axui_clamp_i32(term->csi_params[index + 2], 0, 255);
        u8 b = (u8)axui_clamp_i32(term->csi_params[index + 3], 0, 255);
        u32 colour = axui_rgba(r, g, b, 255);
        if (foreground) {
            term->cursor.fg = colour;
        } else {
            term->cursor.bg = colour;
        }
        return 4;
    }
    return 0;
}

void axterm_reset_attributes(term_t *term)
{
    if (!term) {
        return;
    }
    term->cursor.fg = term->default_fg;
    term->cursor.bg = term->default_bg;
    term->cursor.flags = term->default_flags;
}

/* Dispatch one SGR parameter list.  Defaults (no parameters, or 0) reset. */
static void do_sgr(term_t *term)
{
    if (term->csi_param_count == 0) {
        axterm_reset_attributes(term);
        return;
    }

    bool_t have_foreground = (bool_t)0;
    bool_t have_background = (bool_t)0;

    for (i32 i = 0; i < term->csi_param_count; i++) {
        i32 code = term->csi_params[i];
        switch (code) {
        case 0:
            axterm_reset_attributes(term);
            have_foreground = (bool_t)0;
            have_background = (bool_t)0;
            break;
        case 1:
            term->cursor.flags |= TERM_A_BOLD;
            break;
        case 2:
            term->cursor.flags |= TERM_A_DIM;
            break;
        case 3:
            term->cursor.flags |= TERM_A_ITALIC;
            break;
        case 4:
            term->cursor.flags |= TERM_A_UNDERLINE;
            break;
        case 5:
        case 6:
            term->cursor.flags |= TERM_A_BLINK;
            break;
        case 7:
            term->cursor.flags |= TERM_A_REVERSE;
            break;
        case 8:
            term->cursor.flags |= TERM_A_HIDDEN;
            break;
        case 9:
            term->cursor.flags |= TERM_A_STRIKE;
            break;
        case 21:
        case 22:
            term->cursor.flags &= (u8)~(TERM_A_BOLD | TERM_A_DIM);
            break;
        case 23:
            term->cursor.flags &= (u8)~TERM_A_ITALIC;
            break;
        case 24:
            term->cursor.flags &= (u8)~TERM_A_UNDERLINE;
            break;
        case 25:
            term->cursor.flags &= (u8)~TERM_A_BLINK;
            break;
        case 27:
            term->cursor.flags &= (u8)~TERM_A_REVERSE;
            break;
        case 28:
            term->cursor.flags &= (u8)~TERM_A_HIDDEN;
            break;
        case 29:
            term->cursor.flags &= (u8)~TERM_A_STRIKE;
            break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            term->cursor.fg = axterm_default_palette(code - 30);
            have_foreground = (bool_t)1;
            break;
        case 39:
            term->cursor.fg = term->default_fg;
            have_foreground = (bool_t)0;
            break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            term->cursor.bg = axterm_default_palette(code - 40);
            have_background = (bool_t)1;
            break;
        case 49:
            term->cursor.bg = term->default_bg;
            have_background = (bool_t)0;
            break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            term->cursor.fg = axterm_bright_palette(code - 90);
            have_foreground = (bool_t)1;
            break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            term->cursor.bg = axterm_bright_palette(code - 100);
            have_background = (bool_t)1;
            break;
        case 38: {
            i32 consumed = sgr_extended(term, i + 1, (bool_t)1);
            if (consumed) {
                have_foreground = (bool_t)1;
                i += consumed;
            }
            break;
        }
        case 48: {
            i32 consumed = sgr_extended(term, i + 1, (bool_t)0);
            if (consumed) {
                have_background = (bool_t)1;
                i += consumed;
            }
            break;
        }
        case 58:
            /* Underline colour: accepted and ignored, the face has no
             * multi-colour underline. */
            (void)sgr_extended(term, i + 1, (bool_t)1);
            i += 2;
            break;
        case 59:
            break;
        default:
            break;
        }
    }
    (void)have_foreground;
    (void)have_background;
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Mode setting                                                        */
/* ------------------------------------------------------------------ */

static void set_mode(term_t *term, i32 mode, bool_t enable)
{
    switch (mode) {
    case 1:
        if (enable) {
            term->modes |= TERM_MODE_CURSOR_KEYS;
        } else {
            term->modes &= ~TERM_MODE_CURSOR_KEYS;
        }
        break;
    case 6:
        if (enable) {
            term->modes |= TERM_MODE_ORIGIN;
            term->cursor.x = 0;
            term->cursor.y = term->scroll_top;
        } else {
            term->modes &= ~TERM_MODE_ORIGIN;
            term->cursor.x = 0;
            term->cursor.y = 0;
        }
        break;
    case 7:
        if (enable) {
            term->modes |= TERM_MODE_AUTOWRAP;
        } else {
            term->modes &= ~TERM_MODE_AUTOWRAP;
        }
        break;
    case 4:
        if (enable) {
            term->modes |= TERM_MODE_INSERT;
        } else {
            term->modes &= ~TERM_MODE_INSERT;
        }
        break;
    case 12:
        if (enable) {
            term->modes |= TERM_MODE_CURSOR_BLINK;
        } else {
            term->modes &= ~TERM_MODE_CURSOR_BLINK;
        }
        break;
    case 20:
        if (enable) {
            term->modes |= TERM_MODE_NEWLINE;
        } else {
            term->modes &= ~TERM_MODE_NEWLINE;
        }
        break;
    case 25:
        term->cursor.visible = enable;
        break;
    case 1000:
    case 1002:
    case 1003:
        if (enable) {
            term->modes |= TERM_MODE_MOUSE;
        } else {
            term->modes &= ~TERM_MODE_MOUSE;
        }
        break;
    case 1006:
        if (enable) {
            term->modes |= TERM_MODE_MOUSE_SGR;
        } else {
            term->modes &= ~TERM_MODE_MOUSE_SGR;
        }
        break;
    case 1004:
        if (enable) {
            term->modes |= TERM_MODE_FOCUS_EVENTS;
        } else {
            term->modes &= ~TERM_MODE_FOCUS_EVENTS;
        }
        break;
    case 1047:
        axterm_switch_screen(term, enable);
        break;
    case 1048:
        if (enable) {
            axterm_save_cursor(term);
        } else {
            axterm_restore_cursor(term);
        }
        break;
    case 1049:
        if (enable) {
            axterm_save_cursor(term);
            axterm_switch_screen(term, (bool_t)1);
            axterm_clear_screen(term, 2);
        } else {
            axterm_switch_screen(term, (bool_t)0);
            axterm_restore_cursor(term);
        }
        break;
    case 2004:
        if (enable) {
            term->modes |= TERM_MODE_BRACKET_PASTE;
        } else {
            term->modes &= ~TERM_MODE_BRACKET_PASTE;
        }
        break;
    case 5:
        if (enable) {
            term->modes |= TERM_MODE_REVERSE_VIDEO;
        } else {
            term->modes &= ~TERM_MODE_REVERSE_VIDEO;
        }
        break;
    default:
        break;
    }
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Device reports                                                      */
/* ------------------------------------------------------------------ */

/* Answer a device status report by printing the response, exactly as a real
 * terminal would.  The shell ignores it; a future application can read it. */
static void report_cursor(term_t *term)
{
    char buffer[24];
    (void)ax_snprintf(buffer, sizeof(buffer), "\x1b[%d;%dR", term->cursor.y + 1,
                      term->cursor.x + 1);
    axterm_write(term, buffer);
}

static void report_device(term_t *term)
{
    /* VT220 with ANSI colour and sixel absent. */
    axterm_write(term, "\x1b[?62;22c");
}

/* ------------------------------------------------------------------ */
/* CSI dispatch                                                        */
/* ------------------------------------------------------------------ */

static void do_csi(term_t *term, u8 final)
{
    term->escape_count++;

    /* DEC private modes arrive as CSI ? n h / l. */
    if (term->csi_private) {
        bool_t enable = (bool_t)(final == 'h');
        bool_t disable = (bool_t)(final == 'l');
        if (enable || disable) {
            for (i32 i = 0; i < term->csi_param_count; i++) {
                set_mode(term, term->csi_params[i], enable);
            }
            return;
        }
        if (final == 'n') {
            /* DECDSR: ?6n asks for the cursor position. */
            if (param(term, 0, 6) == 6) {
                report_cursor(term);
            }
            return;
        }
        if (final == 'c') {
            report_device(term);
            return;
        }
        if (final == 'J') {
            axterm_clear_screen(term, param(term, 0, 0));
            return;
        }
        if (final == 'K') {
            axterm_clear_line(term, param(term, 0, 0));
            return;
        }
        return;
    }

    switch (final) {
    case '@':
        axterm_insert_chars(term, param1(term, 0));
        break;
    case 'A':
        axterm_move_relative(term, 0, -param1(term, 0));
        break;
    case 'B':
    case 'e':
        axterm_move_relative(term, 0, param1(term, 0));
        break;
    case 'C':
    case 'a':
        axterm_move_relative(term, param1(term, 0), 0);
        break;
    case 'D':
        axterm_move_relative(term, -param1(term, 0), 0);
        break;
    case 'E':
        axterm_move_relative(term, 0, param1(term, 0));
        term->cursor.x = 0;
        break;
    case 'F':
        axterm_move_relative(term, 0, -param1(term, 0));
        term->cursor.x = 0;
        break;
    case 'G':
    case '`':
        axterm_set_cursor(term, param1(term, 0) - 1, term->cursor.y -
                          ((term->modes & TERM_MODE_ORIGIN) ? term->scroll_top : 0));
        break;
    case 'H':
    case 'f':
        axterm_set_cursor(term, param1(term, 1) - 1, param1(term, 0) - 1);
        break;
    case 'I':
        axterm_tab(term, param1(term, 0));
        break;
    case 'J':
        axterm_clear_screen(term, param(term, 0, 0));
        break;
    case 'K':
        axterm_clear_line(term, param(term, 0, 0));
        break;
    case 'L':
        axterm_insert_lines(term, param1(term, 0));
        break;
    case 'M':
        axterm_delete_lines(term, param1(term, 0));
        break;
    case 'P':
        axterm_delete_chars(term, param1(term, 0));
        break;
    case 'S':
        axterm_scroll_region_up(term, param1(term, 0), (bool_t)0);
        break;
    case 'T':
        axterm_scroll_region_down(term, param1(term, 0));
        break;
    case 'X':
        axterm_erase_chars(term, param1(term, 0));
        break;
    case 'Z':
        axterm_back_tab(term, param1(term, 0));
        break;
    case 'b':
        /* REP: repeat the last printed character.  The grid does not keep a
         * "last cell" pointer, so this is approximated by re-printing the
         * character to the left of the cursor. */
        if (term->cursor.x > 0) {
            term_cell_t *prev = &term->cells[(usize)term->cursor.y * TERM_MAX_COLS +
                                             (usize)(term->cursor.x - 1)];
            i32 repeat = param1(term, 0);
            for (i32 i = 0; i < repeat; i++) {
                axterm_put_cell(term, prev->ch, 0);
            }
        }
        break;
    case 'c':
        /* DA1 without the private prefix. */
        axterm_write(term, "\x1b[?62;22c");
        break;
    case 'd':
        axterm_set_cursor(term, term->cursor.x, param1(term, 0) - 1);
        break;
    case 'g':
        axterm_clear_tabs(term, (bool_t)(param(term, 0, 0) == 3));
        break;
    case 'h':
    case 'l': {
        bool_t enable = (bool_t)(final == 'h');
        i32 modes[] = { 4, 20 };
        for (usize i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            set_mode(term, modes[i], enable);
        }
        break;
    }
    case 'm':
        do_sgr(term);
        break;
    case 'n':
        if (param(term, 0, 0) == 6) {
            report_cursor(term);
        } else if (param(term, 0, 0) == 5) {
            axterm_write(term, "\x1b[0n");
        }
        break;
    case 'r':
        axterm_set_scroll_region(term, param(term, 0, 1), param(term, 1, term->rows));
        break;
    case 's':
        axterm_save_cursor(term);
        break;
    case 'u':
        axterm_restore_cursor(term);
        break;
    case 't':
        /* Window manipulation: only the text-area size report is meaningful. */
        if (param(term, 0, 0) == 18) {
            char buffer[24];
            (void)ax_snprintf(buffer, sizeof(buffer), "\x1b[8;%d;%dt", term->rows,
                              term->cols);
            axterm_write(term, buffer);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* OSC dispatch                                                        */
/* ------------------------------------------------------------------ */

/* The OSC buffer holds "code;payload".  Split it at the first semicolon. */
static void do_osc(term_t *term)
{
    term->osc_buffer[term->osc_length] = 0;
    if (term->osc_length == 0) {
        return;
    }

    const char *cursor = term->osc_buffer;
    i32 code = 0;
    while (ax_isdigit(*cursor)) {
        code = code * 10 + (*cursor - '0');
        cursor++;
    }
    if (*cursor == ';') {
        cursor++;
    }

    switch (code) {
    case 0:
    case 2:
        axterm_set_title(term, *cursor ? cursor : "Terminal");
        break;
    case 1:
        /* Icon name: no desktop icons here, so this is recorded as status. */
        axterm_set_status(term, cursor);
        break;
    case 4: {
        /* Palette override: "4;index;colour" or "4;index;rgb:rr/gg/bb". */
        i32 index = 0;
        const char *p = cursor;
        while (ax_isdigit(*p)) {
            index = index * 10 + (*p - '0');
            p++;
        }
        if (*p == ';') {
            p++;
        }
        u32 colour = 0;
        if (ax_startswith(p, "rgb:")) {
            /* xterm's "rgb:rr/gg/bb" form uses 1..4 hex digits per channel. */
            u32 channels[3] = { 0, 0, 0 };
            p += 4;
            for (i32 c = 0; c < 3 && *p; c++) {
                u32 value = 0;
                i32 digits = 0;
                while (ax_isxdigit(*p) && digits < 4) {
                    value = (value << 4) | (u32)ax_digit_value(*p);
                    p++;
                    digits++;
                }
                /* Scale a 1..2 digit value to 0..255. */
                channels[c] = digits <= 2 ? value : value >> 8;
                if (*p == '/') {
                    p++;
                }
            }
            colour = axui_rgba((u8)channels[0], (u8)channels[1], (u8)channels[2], 255);
        } else {
            colour = axui_parse_colour(p);
        }
        if (colour) {
            axterm_set_colour(term, index, colour);
        }
        break;
    }
    case 7:
        /* Current working directory: kept in the status line, the shell does
         * not have a filesystem to resolve it against. */
        axterm_set_status(term, cursor);
        break;
    case 10:
        term->default_fg = axui_parse_colour(cursor) ? axui_parse_colour(cursor)
                                                    : term->default_fg;
        break;
    case 11:
        term->default_bg = axui_parse_colour(cursor) ? axui_parse_colour(cursor)
                                                    : term->default_bg;
        break;
    case 12:
        term->cursor_colour = axui_parse_colour(cursor) ? axui_parse_colour(cursor)
                                                        : term->cursor_colour;
        break;
    case 104:
        /* Reset palette to the built-in defaults. */
        for (i32 i = 0; i < 256; i++) {
            axterm_set_colour(term, i, axterm_xterm256(i));
        }
        break;
    case 110:
        term->default_fg = axui_rgba(0xD8, 0xDE, 0xE9, 255);
        break;
    case 111:
        term->default_bg = axui_rgba(0x0A, 0x0D, 0x16, 255);
        break;
    case 112:
        term->cursor_colour = axui_rgba(0x9C, 0xD8, 0xFF, 255);
        break;
    default:
        break;
    }
    term->dirty = (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* ESC dispatch                                                        */
/* ------------------------------------------------------------------ */

static void do_esc(term_t *term, u8 final)
{
    term->escape_count++;
    switch (final) {
    case 'D':
        axterm_newline(term);
        break;
    case 'E':
        axterm_newline(term);
        break;
    case 'H':
        axterm_set_tab_stop(term);
        break;
    case 'M':
        axterm_reverse_index(term);
        break;
    case 'N':
        /* Single shift: switch to G2 for one character; unsupported, but the
         * sequence must be consumed. */
        break;
    case 'O':
        /* Single shift G3. */
        break;
    case '7':
        axterm_save_cursor(term);
        break;
    case '8':
        axterm_restore_cursor(term);
        break;
    case 'c':
        axterm_reset(term);
        break;
    case '=':
        term->modes |= TERM_MODE_APP_KEYPAD;
        break;
    case '>':
        term->modes &= ~TERM_MODE_APP_KEYPAD;
        break;
    case '(':
        term->g0_charset = term->csi_intermediate;
        break;
    case ')':
        term->g1_charset = term->csi_intermediate;
        break;
    case '*':
    case '+':
        break;
    case '#':
        /* DEC alignment test: fill the screen with 'E' characters, which is
         * the historical behaviour and useful for calibrating the face. */
        for (i32 y = 0; y < term->rows; y++) {
            for (i32 x = 0; x < term->cols; x++) {
                term_cell_t *cell = &term->cells[(usize)y * TERM_MAX_COLS + (usize)x];
                cell->ch = 'E';
                cell->fg = term->cursor.fg;
                cell->bg = term->cursor.bg;
                cell->flags = term->cursor.flags;
            }
        }
        term->dirty = (bool_t)1;
        break;
    case '%':
        break;
    case '6':
        /* DECBI: back index. */
        axterm_scroll_region_down(term, 1);
        break;
    case '5':
        /* DECSC index. */
        axterm_newline(term);
        break;
    case '\\':
        /* String terminator outside a string: ignore. */
        break;
    default:
        break;
    }
    term->csi_intermediate = 0;
}

/* ------------------------------------------------------------------ */
/* Feed                                                                */
/* ------------------------------------------------------------------ */

/* C0 controls are legal in every state and must be handled before the state
 * dispatch, exactly as the VT100 does. */
static bool_t handle_c0(term_t *term, u8 byte)
{
    switch (byte) {
    case 0x07:
        axterm_bell(term);
        return (bool_t)1;
    case 0x08:
        axterm_backspace(term);
        return (bool_t)1;
    case 0x09:
        axterm_tab(term, 1);
        return (bool_t)1;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        axterm_newline(term);
        if (term->modes & TERM_MODE_NEWLINE) {
            axterm_carriage_return(term);
        }
        return (bool_t)1;
    case 0x0D:
        axterm_carriage_return(term);
        return (bool_t)1;
    case 0x0E:
        term->active_charset = 1;
        return (bool_t)1;
    case 0x0F:
        term->active_charset = 0;
        return (bool_t)1;
    case 0x18:
    case 0x1A:
        /* CAN and SUB abort any sequence in progress. */
        term->state = TERM_STATE_GROUND;
        term->csi_param_count = 0;
        term->osc_length = 0;
        return (bool_t)1;
    case 0x1B:
        term->state = TERM_STATE_ESC;
        term->csi_param_count = 0;
        term->csi_private = (bool_t)0;
        term->csi_intermediate = 0;
        return (bool_t)1;
    case 0x7F:
        return (bool_t)1;
    default:
        return (bool_t)(byte < 0x20);
    }
}

/* UTF-8 accumulation for the ground state.  The grid stores whole code
 * points, so multi-byte sequences are decoded into one cell. */
static void print_byte(term_t *term, u8 byte)
{
    if (term->utf8_remaining) {
        if ((byte & 0xC0) != 0x80) {
            /* Broken sequence: emit the replacement and reprocess this byte
             * as a fresh start. */
            term->utf8_remaining = 0;
            axterm_put_cell(term, '?', 0);
        } else {
            term->utf8_code = (term->utf8_code << 6) | (u32)(byte & 0x3F);
            term->utf8_remaining--;
            if (term->utf8_remaining == 0) {
                u32 code = term->utf8_code;
                if (code < term->utf8_min || code > 0x10FFFF) {
                    code = '?';
                }
                axterm_put_cell(term, code, 0);
            }
            return;
        }
    }

    if (byte < 0x80) {
        if (byte >= 0x20) {
            axterm_put_cell(term, byte, 0);
        }
        return;
    }
    if ((byte & 0xE0) == 0xC0) {
        term->utf8_code = byte & 0x1F;
        term->utf8_remaining = 1;
        term->utf8_min = 0x80;
    } else if ((byte & 0xF0) == 0xE0) {
        term->utf8_code = byte & 0x0F;
        term->utf8_remaining = 2;
        term->utf8_min = 0x800;
    } else if ((byte & 0xF8) == 0xF0) {
        term->utf8_code = byte & 0x07;
        term->utf8_remaining = 3;
        term->utf8_min = 0x10000;
    } else {
        axterm_put_cell(term, '?', 0);
    }
}

void axterm_feed(term_t *term, u8 byte)
{
    if (!term) {
        return;
    }
    term->bytes_in++;

    /* Controls are handled in every state.  ESC switches state, and ESC
     * inside an OSC string is the start of ST ("ESC \\"), which the string
     * states handle below. */
    if (term->state == TERM_STATE_OSC_STRING) {
        if (byte == 0x07 || byte == 0x9C) {
            do_osc(term);
            term->state = TERM_STATE_GROUND;
            term->osc_length = 0;
            return;
        }
        if (byte == 0x1B) {
            term->state = TERM_STATE_STRING_IGNORE;
            return;
        }
        if (byte >= 0x20 && byte != 0x7F) {
            if (term->osc_length < TERM_MAX_OSC - 1) {
                term->osc_buffer[term->osc_length++] = (char)byte;
            }
        }
        return;
    }

    if (term->state == TERM_STATE_DCS_STRING ||
        term->state == TERM_STATE_SOS_STRING ||
        term->state == TERM_STATE_STRING_IGNORE) {
        if (byte == 0x1B) {
            term->state = TERM_STATE_STRING_IGNORE;
            return;
        }
        if (byte == 0x07 || byte == 0x9C || byte == '\\') {
            term->state = TERM_STATE_GROUND;
        }
        return;
    }

    if (handle_c0(term, byte)) {
        return;
    }

    switch (term->state) {
    case TERM_STATE_GROUND:
        print_byte(term, byte);
        break;

    case TERM_STATE_ESC:
        if (byte == '[') {
            term->state = TERM_STATE_CSI_ENTRY;
            param_reset(term);
            term->csi_private = (bool_t)0;
            break;
        }
        if (byte == ']') {
            term->state = TERM_STATE_OSC_STRING;
            term->osc_length = 0;
            break;
        }
        if (byte == 'P') {
            term->state = TERM_STATE_DCS_STRING;
            break;
        }
        if (byte == 'X') {
            term->state = TERM_STATE_SOS_STRING;
            break;
        }
        if (byte == '^' || byte == '_') {
            term->state = TERM_STATE_STRING_IGNORE;
            break;
        }
        if (byte == '(' || byte == ')' || byte == '*' || byte == '+') {
            /* Charset designator: remember the slot and wait for the
             * character that selects the set. */
            term->csi_intermediate = byte;
            term->state = TERM_STATE_ESC_INTERMEDIATE;
            break;
        }
        if (byte == '#') {
            term->csi_intermediate = byte;
            term->state = TERM_STATE_ESC_INTERMEDIATE;
            break;
        }
        if (byte >= 0x20 && byte <= 0x2F) {
            term->csi_intermediate = byte;
            term->state = TERM_STATE_ESC_INTERMEDIATE;
            break;
        }
        do_esc(term, byte);
        term->state = TERM_STATE_GROUND;
        break;

    case TERM_STATE_ESC_INTERMEDIATE: {
        u8 intermediate = term->csi_intermediate;
        if (intermediate == '(') {
            term->g0_charset = (u8)(byte == '0' ? 1 : 0);
        } else if (intermediate == ')') {
            term->g1_charset = (u8)(byte == '0' ? 1 : 0);
        } else if (intermediate == '#') {
            /* DECALN arrived through the intermediate path. */
            do_esc(term, '#');
        }
        term->csi_intermediate = 0;
        term->state = TERM_STATE_GROUND;
        break;
    }

    case TERM_STATE_CSI_ENTRY:
    case TERM_STATE_CSI_PARAM:
        if (byte >= '0' && byte <= '9') {
            i32 index = term->csi_param_count;
            if (index >= TERM_MAX_PARAMS) {
                index = TERM_MAX_PARAMS - 1;
            }
            i32 value = term->csi_params[index];
            value = value * 10 + (i32)(byte - '0');
            if (value > 65535) {
                value = 65535;
            }
            term->csi_params[index] = value;
            term->state = TERM_STATE_CSI_PARAM;
            break;
        }
        if (byte == ';' || byte == ':') {
            if (term->csi_param_count < TERM_MAX_PARAMS - 1) {
                term->csi_param_count++;
            }
            term->state = TERM_STATE_CSI_PARAM;
            break;
        }
        if (byte == '?' || byte == '<' || byte == '=' || byte == '>') {
            term->csi_private = (bool_t)1;
            term->state = TERM_STATE_CSI_PARAM;
            break;
        }
        if (byte >= 0x20 && byte <= 0x2F) {
            term->state = TERM_STATE_CSI_INTERMEDIATE;
            break;
        }
        if (byte >= 0x40 && byte <= 0x7E) {
            /* A parameter without a value counts as one parameter. */
            if (term->csi_param_count == 0 && term->csi_params[0] == 0) {
                term->csi_param_count = 0;
            } else {
                term->csi_param_count++;
            }
            do_csi(term, byte);
            term->state = TERM_STATE_GROUND;
            break;
        }
        break;

    case TERM_STATE_CSI_INTERMEDIATE:
        if (byte >= 0x40 && byte <= 0x7E) {
            do_csi(term, byte);
            term->state = TERM_STATE_GROUND;
        }
        break;

    default:
        term->state = TERM_STATE_GROUND;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Bulk output                                                         */
/* ------------------------------------------------------------------ */

void axterm_write(term_t *term, const char *text)
{
    if (!term || !text) {
        return;
    }
    while (*text) {
        axterm_feed(term, (u8)*text++);
    }
}

void axterm_write_line(term_t *term, const char *text)
{
    axterm_write(term, text);
    axterm_feed(term, '\n');
}

/* Formatted output: the kernel's own ax_vsnprintf, so the format string is
 * the same subset the rest of the tree uses. */
void axterm_printf(term_t *term, const char *fmt, ...)
{
    char buffer[256];
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    ax_vsnprintf(buffer, sizeof(buffer), fmt, args);
    __builtin_va_end(args);
    axterm_write(term, buffer);
}

void axterm_bell(term_t *term)
{
    if (!term) {
        return;
    }
    term->bell_count++;
    /* No audio device on the shell's short list; the bell shows as a flash of
     * the status line, which the painter animates. */
    axterm_set_status(term, "bell");
    term->dirty = (bool_t)1;
}
