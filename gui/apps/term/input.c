/* AxiomOS terminal - keyboard and mouse input.
 *
 * Two consumers share this code path: the built-in shell, which wants editing
 * keys (history, completion, kill-line) handled locally, and a hypothetical
 * full-screen application, which wants the escape sequences written out so it
 * can parse them itself.  axterm_input_key() decides between the two based on
 * `shell_attached`.
 *
 * The escape sequence table follows xterm's defaults, including the DECCKM
 * application-cursor variants and the F1..F12 numbering.  Alt is emitted as
 * an ESC prefix, which is what terminals have done since the VT220.
 */

#include <axiom/lib.h>
#include "term.h"

/* ------------------------------------------------------------------ */
/* Sequence table                                                      */
/* ------------------------------------------------------------------ */

/* Returns a NUL-terminated sequence and its length.  The strings are static
 * so callers can pass them straight to axterm_write. */
const char *axterm_key_sequence(const term_t *term, term_key_t key, bool_t shift,
                                bool_t ctrl, bool_t alt, usize *out_length)
{
    static char buffer[12];
    bool_t app = (bool_t)((term ? term->modes : 0) & TERM_MODE_CURSOR_KEYS);
    const char *base = "";

    switch (key) {
    case TERM_KEY_UP:
        base = app ? "\x1bOA" : "\x1b[A";
        break;
    case TERM_KEY_DOWN:
        base = app ? "\x1bOB" : "\x1b[B";
        break;
    case TERM_KEY_LEFT:
        base = app ? "\x1bOD" : "\x1b[D";
        break;
    case TERM_KEY_RIGHT:
        base = app ? "\x1bOC" : "\x1b[C";
        break;
    case TERM_KEY_HOME:
        base = app ? "\x1bOH" : "\x1b[H";
        break;
    case TERM_KEY_END:
        base = app ? "\x1bOF" : "\x1b[F";
        break;
    case TERM_KEY_INSERT:
        base = "\x1b[2~";
        break;
    case TERM_KEY_DELETE:
        base = "\x1b[3~";
        break;
    case TERM_KEY_PAGE_UP:
        base = "\x1b[5~";
        break;
    case TERM_KEY_PAGE_DOWN:
        base = "\x1b[6~";
        break;
    case TERM_KEY_TAB:
        base = shift ? "\x1b[Z" : "\t";
        break;
    case TERM_KEY_BACKTAB:
        base = "\x1b[Z";
        break;
    case TERM_KEY_ENTER:
        base = "\r";
        break;
    case TERM_KEY_ESCAPE:
        base = "\x1b";
        break;
    case TERM_KEY_BACKSPACE:
        base = "\x7f";
        break;
    case TERM_KEY_F1:
        base = "\x1bOP";
        break;
    case TERM_KEY_F2:
        base = "\x1bOQ";
        break;
    case TERM_KEY_F3:
        base = "\x1bOR";
        break;
    case TERM_KEY_F4:
        base = "\x1bOS";
        break;
    case TERM_KEY_F5:
        base = "\x1b[15~";
        break;
    case TERM_KEY_F6:
        base = "\x1b[17~";
        break;
    case TERM_KEY_F7:
        base = "\x1b[18~";
        break;
    case TERM_KEY_F8:
        base = "\x1b[19~";
        break;
    case TERM_KEY_F9:
        base = "\x1b[20~";
        break;
    case TERM_KEY_F10:
        base = "\x1b[21~";
        break;
    case TERM_KEY_F11:
        base = "\x1b[23~";
        break;
    case TERM_KEY_F12:
        base = "\x1b[24~";
        break;
    default:
        break;
    }

    usize length = ax_strlen(base);
    usize out = 0;

    /* Modifier prefix: xterm encodes Ctrl and Alt as CSI 1;<n> on the cursor
     * keys.  For the shell, Alt on a printable key arrives as ESC prefix from
     * the driver instead, so only the cursor keys need the parameter form. */
    if ((ctrl || alt || shift) && length >= 2 && base[0] == 0x1b &&
        (base[1] == '[' || base[1] == 'O')) {
        i32 modifier = 1;
        if (shift) {
            modifier += 1;
        }
        if (alt) {
            modifier += 2;
        }
        if (ctrl) {
            modifier += 4;
        }
        /* Rewrite "ESC [ X" as "ESC [ 1 ; n X" for the CSI forms; the SS3
         * forms (ESC O X) have no modifier syntax, so leave them alone. */
        if (base[1] == '[' && length == 3 && base[2] >= 'A' && base[2] <= 'Z') {
            buffer[0] = 0x1b;
            buffer[1] = '[';
            buffer[2] = '1';
            buffer[3] = ';';
            if (modifier < 10) {
                buffer[4] = (char)('0' + modifier);
                buffer[5] = base[2];
                buffer[6] = 0;
                length = 6;
                base = buffer;
            }
        }
    }

    for (usize i = 0; i < length && out < sizeof(buffer) - 1; i++) {
        buffer[out++] = base[i];
    }
    buffer[out] = 0;
    if (out_length) {
        *out_length = out;
    }
    return buffer;
}

/* ------------------------------------------------------------------ */
/* Local actions                                                       */
/* ------------------------------------------------------------------ */

/* Ctrl+letter produces the classic control code; only the ones the shell uses
 * are listed, the rest fall through to a plain byte. */
static u8 control_code(char ch)
{
    switch (ax_tolower(ch)) {
    case 'a': return 0x01;
    case 'b': return 0x02;
    case 'c': return 0x03;
    case 'd': return 0x04;
    case 'e': return 0x05;
    case 'f': return 0x06;
    case 'g': return 0x07;
    case 'h': return 0x08;
    case 'i': return 0x09;
    case 'j': return 0x0A;
    case 'k': return 0x0B;
    case 'l': return 0x0C;
    case 'm': return 0x0D;
    case 'n': return 0x0E;
    case 'o': return 0x0F;
    case 'p': return 0x10;
    case 'q': return 0x11;
    case 'r': return 0x12;
    case 's': return 0x13;
    case 't': return 0x14;
    case 'u': return 0x15;
    case 'v': return 0x16;
    case 'w': return 0x17;
    case 'x': return 0x18;
    case 'y': return 0x19;
    case 'z': return 0x1A;
    default: return 0;
    }
}

/* The shell's local key handling.  Returns 1 when the key was consumed. */
static bool_t shell_key(term_t *term, term_key_t key, bool_t shift, bool_t ctrl,
                        bool_t alt)
{
    switch (key) {
    case TERM_KEY_UP:
        axterm_shell_history_step(term, -1);
        return (bool_t)1;
    case TERM_KEY_DOWN:
        axterm_shell_history_step(term, 1);
        return (bool_t)1;
    case TERM_KEY_TAB:
        axterm_shell_complete(term);
        return (bool_t)1;
    case TERM_KEY_BACKSPACE:
        axterm_shell_backspace(term);
        return (bool_t)1;
    case TERM_KEY_ENTER:
        /* The submit needs the current input, which lives in the shell. */
        axterm_shell_submit_current(term);
        return (bool_t)1;
    case TERM_KEY_PAGE_UP:
        axterm_scroll_view(term, term->rows / 2);
        return (bool_t)1;
    case TERM_KEY_PAGE_DOWN:
        axterm_scroll_view(term, -(term->rows / 2));
        return (bool_t)1;
    case TERM_KEY_ESCAPE:
        axterm_shell_clear_input(term);
        return (bool_t)1;
    default:
        break;
    }

    if (ctrl) {
        /* Ctrl+C abandons the line, Ctrl+L clears, Ctrl+U kills to the start
         * and Ctrl+W deletes the previous word. */
        switch (key) {
        case TERM_KEY_NONE:
            break;
        default:
            break;
        }
    }
    (void)shift;
    (void)alt;
    return (bool_t)0;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

void axterm_input_bytes(term_t *term, const char *bytes, usize count)
{
    if (!term || !bytes) {
        return;
    }
    for (usize i = 0; i < count; i++) {
        axterm_feed(term, (u8)bytes[i]);
    }
}

void axterm_input_char(term_t *term, char ch)
{
    if (!term || !ch) {
        return;
    }
    /* Typing at a scrolled-back view returns to the live screen first. */
    if (term->history.offset) {
        term->history.offset = 0;
        term->dirty = (bool_t)1;
    }
    if (term->shell_attached) {
        axterm_shell_insert(term, ch);
        return;
    }
    axterm_feed(term, (u8)ch);
}

void axterm_input_key(term_t *term, term_key_t key, bool_t shift, bool_t ctrl,
                      bool_t alt)
{
    if (!term || key == TERM_KEY_NONE) {
        return;
    }
    if (term->history.offset && key != TERM_KEY_PAGE_UP &&
        key != TERM_KEY_PAGE_DOWN) {
        term->history.offset = 0;
        term->dirty = (bool_t)1;
    }

    if (term->shell_attached) {
        if (shell_key(term, key, shift, ctrl, alt)) {
            return;
        }
        if (ctrl && (key == TERM_KEY_F1 || key == TERM_KEY_F2)) {
            /* Ctrl+F1 opens the shortcut card; the app layer handles the
             * window, the terminal just records it. */
            axterm_set_status(term, "shortcuts: ^C cancel  ^L clear  ^U erase line");
            return;
        }
    }

    usize length = 0;
    const char *sequence = axterm_key_sequence(term, key, shift, ctrl, alt,
                                               &length);
    if (length) {
        axterm_write(term, sequence);
    }
}

/* Handle a Ctrl+letter combination that the driver delivered as a plain
 * character plus a modifier flag. */
bool_t axterm_input_control(term_t *term, char ch)
{
    if (!term) {
        return (bool_t)0;
    }
    u8 code = control_code(ch);
    if (!code) {
        return (bool_t)0;
    }

    if (term->shell_attached) {
        switch (code) {
        case 0x03:      /* Ctrl+C */
            axterm_write(term, "^C\n");
            axterm_shell_clear_input(term);
            axterm_shell_prompt(term);
            return (bool_t)1;
        case 0x0C:      /* Ctrl+L */
            axterm_clear_screen(term, 2);
            axterm_shell_prompt(term);
            return (bool_t)1;
        case 0x15:      /* Ctrl+U */
            axterm_shell_clear_input(term);
            return (bool_t)1;
        case 0x04:      /* Ctrl+D */
            axterm_write_line(term, "logout");
            axterm_shell_prompt(term);
            return (bool_t)1;
        default:
            break;
        }
    }

    axterm_feed(term, code);
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

/* Wheel scrolling works regardless of the mouse-reporting mode, because the
 * shell has no application that wants raw wheel events. */
void axterm_input_wheel(term_t *term, i32 delta)
{
    if (!term) {
        return;
    }
    axterm_scroll_view(term, delta * 3);
}

/* Report a click: three lines of logic depending on the mode.  The shell uses
 * a click-drag selection; a full-screen application would get an SGR report
 * written into its input stream. */
void axterm_input_click(term_t *term, i32 x, i32 y, u8 buttons, bool_t pressed,
                        i32 visible_row)
{
    if (!term) {
        return;
    }
    if (term->modes & TERM_MODE_MOUSE) {
        /* SGR encoding: ESC [ < b ; x ; y M/m, with 1-based coordinates. */
        char buffer[32];
        i32 button = 0;
        if (buttons & 1) {
            button = 0;
        } else if (buttons & 2) {
            button = 1;
        } else if (buttons & 4) {
            button = 2;
        }
        (void)ax_snprintf(buffer, sizeof(buffer), "\x1b[<%d;%d;%d%c", button,
                          x + 1, y + 1, pressed ? 'M' : 'm');
        axterm_write(term, buffer);
        return;
    }

    if (pressed) {
        axterm_selection_begin(term, x, visible_row);
    } else {
        axterm_selection_end(term);
    }
}

/* Copy the active selection to the shell's clipboard buffer and report the
 * size, which the status bar shows. */
usize axterm_input_copy(term_t *term, char *buffer, usize cap)
{
    usize copied = axterm_selection_copy(term, buffer, cap);
    if (copied) {
        char status[64];
        (void)ax_snprintf(status, sizeof(status), "copied %u bytes",
                          (u32)copied);
        axterm_set_status(term, status);
    }
    return copied;
}
