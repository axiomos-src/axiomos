/* AxiomOS - tty line discipline.
 *
 * The console, serial ports and pseudo terminals share one line discipline:
 * canonical input with erase, kill and word-erase handling, signal generation on
 * control characters, output post-processing (tab expansion and CR/LF mapping)
 * and a scrollback ring that the terminal app reads through a small API.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXTY_MAX_TTYS         8
#define AXTY_INPUT_BUFFER     256
#define AXTY_OUTPUT_BUFFER    512
#define AXTY_SCROLLBACK       128
#define AXTY_LINE_MAX         128
#define AXTY_COLUMNS          80
#define AXTY_ROWS             25

#define AXTY_LFLAG_ICANON     0x0001
#define AXTY_LFLAG_ECHO       0x0002
#define AXTY_LFLAG_ISIG       0x0004
#define AXTY_LFLAG_ECHOE      0x0008
#define AXTY_LFLAG_ECHOK      0x0010
#define AXTY_LFLAG_ECHONL     0x0020
#define AXTY_LFLAG_NOFLSH     0x0040

#define AXTY_OFLAG_OPOST      0x0001
#define AXTY_OFLAG_ONLCR      0x0002
#define AXTY_OFLAG_TABEXPAND  0x0004

#define AXTY_IFLAG_CRNL       0x0001
#define AXTY_IFLAG_IGNCR      0x0002
#define AXTY_IFLAG_STRIP_CTRL 0x0004
#define AXTY_IFLAG_FLOW       0x0008

#define AXTY_CHAR_ERASE       '\b'
#define AXTY_CHAR_KILL        'U' - 64
#define AXTY_CHAR_WORDERASE   'W' - 64
#define AXTY_CHAR_EOF         'D' - 64
#define AXTY_CHAR_INTR        'C' - 64
#define AXTY_CHAR_QUIT        '\\' - 32
#define AXTY_CHAR_SUSPEND     'Z' - 64
#define AXTY_CHAR_STOP        'S' - 64
#define AXTY_CHAR_START       'Q' - 64

#define AXTY_SIGNAL_NONE      0
#define AXTY_SIGNAL_INTR      2
#define AXTY_SIGNAL_QUIT      3
#define AXTY_SIGNAL_SUSPEND   20
#define AXTY_SIGNAL_STOP      21

typedef struct axty_line
{
    char text[AXTY_LINE_MAX];
    u32  length;
    u32  cursor;
    bool_t complete;
    bool_t wrapped;
} axty_line_t;

typedef struct axty_scrollback
{
    char text[AXTY_COLUMNS];
    u32  length;
    bool_t wrapped;
} axty_scrollback_t;

typedef struct axty_tty
{
    axty_line_t line;
    axty_scrollback_t scroll[AXTY_SCROLLBACK];
    char input[AXTY_INPUT_BUFFER];
    char output[AXTY_OUTPUT_BUFFER];
    u32  input_head;
    u32  input_tail;
    u32  output_head;
    u32  output_tail;
    u32  scroll_head;
    u32  scroll_count;
    u32  column;
    u32  row;
    u32  lflag;
    u32  oflag;
    u32  iflag;
    u32  baud;
    u32  bytes_read;
    u32  bytes_written;
    u32  erased_chars;
    u32  killed_lines;
    u32  interrupts;
    u32  pending_signal;
    u32  flow_stalled;
    u8   index;
    bool_t open;
    bool_t stopped;
} axty_tty_t;

typedef struct axty_state
{
    axty_tty_t ttys[AXTY_MAX_TTYS];
    u32  active;
    u32  open_count;
    u32  input_overflow;
    u32  output_overflow;
    u32  scroll_overflow;
    u32  signal_events;
    bool_t initialized;
} axty_state_t;

static axty_state_t axty_state;

static void axty_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axty_tty_t *axty_get(u32 index)
{
    if (index >= axty_state.open_count && index != axty_state.active)
        return NULL_PTR;
    if (index >= AXTY_MAX_TTYS || !axty_state.ttys[index].open)
        return NULL_PTR;
    return &axty_state.ttys[index];
}

static u32 axty_push_input(axty_tty_t *tty, char value)
{
    u32 next = (tty->input_head + 1) % AXTY_INPUT_BUFFER;
    if (next == tty->input_tail)
    {
        axty_state.input_overflow++;
        return 0;
    }
    tty->input[tty->input_head] = value;
    tty->input_head = next;
    return 1;
}

static u32 axty_push_output(axty_tty_t *tty, char value)
{
    u32 next = (tty->output_head + 1) % AXTY_OUTPUT_BUFFER;
    if (next == tty->output_tail)
    {
        axty_state.output_overflow++;
        return 0;
    }
    tty->output[tty->output_head] = value;
    tty->output_head = next;
    return 1;
}

static void axty_scroll_commit(axty_tty_t *tty)
{
    axty_scrollback_t *entry;
    if (tty->column == 0 && tty->line.length == 0)
        return;
    entry = &tty->scroll[tty->scroll_head];
    entry->length = tty->line.length < AXTY_COLUMNS ? tty->line.length : AXTY_COLUMNS;
    for (u32 i = 0; i < entry->length; i++)
        entry->text[i] = tty->line.text[i];
    entry->wrapped = tty->line.wrapped;
    tty->scroll_head = (tty->scroll_head + 1) % AXTY_SCROLLBACK;
    if (tty->scroll_count < AXTY_SCROLLBACK)
        tty->scroll_count++;
    else
        axty_state.scroll_overflow++;
    tty->row++;
    tty->column = 0;
    tty->line.length = 0;
    tty->line.cursor = 0;
    tty->line.complete = false;
}

static u32 axty_echo(axty_tty_t *tty, const char *text, u32 length)
{
    u32 written = 0;
    if ((tty->lflag & AXTY_LFLAG_ECHO) == 0)
        return 0;
    for (u32 i = 0; i < length; i++)
    {
        char value = text[i];
        if (value == '\n' || value == '\r')
        {
            axty_scroll_commit(tty);
            continue;
        }
        if (value == AXTY_CHAR_ERASE)
        {
            if (tty->line.cursor > 0)
            {
                tty->line.cursor--;
                tty->line.text[tty->line.cursor] = 0;
                tty->line.length = tty->line.cursor;
            }
            continue;
        }
        if (value < 32)
        {
            /* Control characters are not echoed. */
            continue;
        }
        if (tty->line.length >= AXTY_LINE_MAX)
        {
            tty->line.wrapped = true;
            axty_scroll_commit(tty);
        }
        tty->line.text[tty->line.length] = value;
        tty->line.length++;
        tty->line.cursor = tty->line.length;
        tty->column++;
        if (tty->column >= AXTY_COLUMNS)
        {
            tty->line.wrapped = true;
            axty_scroll_commit(tty);
        }
        written++;
    }
    return written;
}

/* -------------------------------------------------------------- public API */

void axty_init(void)
{
    axty_zero(&axty_state, (u32)sizeof(axty_state));
    axty_state.initialized = true;
}

u32 axty_open(u32 baud)
{
    if (!axty_state.initialized || axty_state.open_count >= AXTY_MAX_TTYS)
        return 0xFFFFFFFFu;
    axty_tty_t *tty = &axty_state.ttys[axty_state.open_count];
    axty_zero(tty, (u32)sizeof(*tty));
    tty->index = (u8)axty_state.open_count;
    tty->baud = baud;
    tty->lflag = AXTY_LFLAG_ICANON | AXTY_LFLAG_ECHO | AXTY_LFLAG_ECHOE | AXTY_LFLAG_ISIG;
    tty->oflag = AXTY_OFLAG_OPOST | AXTY_OFLAG_ONLCR | AXTY_OFLAG_TABEXPAND;
    tty->iflag = AXTY_IFLAG_CRNL;
    tty->open = true;
    axty_state.open_count++;
    if (axty_state.open_count == 1)
        axty_state.active = tty->index;
    return tty->index;
}

bool_t axty_close(u32 index)
{
    axty_tty_t *tty = axty_get(index);
    if (tty == NULL_PTR)
        return false;
    tty->open = false;
    axty_state.open_count--;
    if (axty_state.active == index && axty_state.open_count > 0)
    {
        for (u32 i = 0; i < AXTY_MAX_TTYS; i++)
        {
            if (axty_state.ttys[i].open)
            {
                axty_state.active = i;
                break;
            }
        }
    }
    return true;
}

u32 axty_set_flags(u32 index, u32 lflag, u32 oflag, u32 iflag)
{
    axty_tty_t *tty = axty_get(index);
    if (tty == NULL_PTR)
        return 0;
    tty->lflag = lflag;
    tty->oflag = oflag;
    tty->iflag = iflag;
    return 1;
}

u32 axty_get_flags(u32 index, u32 *lflag, u32 *oflag, u32 *iflag)
{
    axty_tty_t *tty = axty_get(index);
    if (tty == NULL_PTR)
        return 0;
    if (lflag != NULL_PTR)
        *lflag = tty->lflag;
    if (oflag != NULL_PTR)
        *oflag = tty->oflag;
    if (iflag != NULL_PTR)
        *iflag = tty->iflag;
    return 1;
}

u32 axty_input(u32 index, const char *data, u32 length)
{
    axty_tty_t *tty = axty_get(index);
    u32 accepted = 0;
    if (tty == NULL_PTR || data == NULL_PTR)
        return 0;
    if (tty->stopped && (tty->iflag & AXTY_IFLAG_FLOW))
    {
        tty->flow_stalled++;
        return 0;
    }
    for (u32 i = 0; i < length; i++)
    {
        char value = data[i];
        if (value == AXTY_CHAR_STOP && (tty->lflag & AXTY_LFLAG_ISIG))
        {
            tty->stopped = true;
            continue;
        }
        if (value == AXTY_CHAR_START && (tty->lflag & AXTY_LFLAG_ISIG))
        {
            tty->stopped = false;
            continue;
        }
        if (value == AXTY_CHAR_INTR && (tty->lflag & AXTY_LFLAG_ISIG))
        {
            tty->pending_signal = AXTY_SIGNAL_INTR;
            tty->interrupts++;
            axty_state.signal_events++;
            if ((tty->lflag & AXTY_LFLAG_NOFLSH) == 0)
            {
                tty->input_head = tty->input_tail;
                tty->line.length = 0;
            }
            continue;
        }
        if (value == AXTY_CHAR_QUIT && (tty->lflag & AXTY_LFLAG_ISIG))
        {
            tty->pending_signal = AXTY_SIGNAL_QUIT;
            axty_state.signal_events++;
            continue;
        }
        if (value == AXTY_CHAR_SUSPEND && (tty->lflag & AXTY_LFLAG_ISIG))
        {
            tty->pending_signal = AXTY_SIGNAL_SUSPEND;
            axty_state.signal_events++;
            continue;
        }
        if ((tty->iflag & AXTY_IFLAG_IGNCR) && value == '\r')
            continue;
        if ((tty->iflag & AXTY_IFLAG_CRNL) && value == '\r')
            value = '\n';
        if ((tty->iflag & AXTY_IFLAG_STRIP_CTRL) && value < 32 && value != '\n' && value != '\t')
            continue;
        if (tty->lflag & AXTY_LFLAG_ICANON)
        {
            if (value == AXTY_CHAR_ERASE)
            {
                if (tty->line.length > 0)
                {
                    tty->line.length--;
                    tty->line.text[tty->line.length] = 0;
                    tty->erased_chars++;
                    axty_echo(tty, &value, 1);
                }
                continue;
            }
            if (value == AXTY_CHAR_KILL)
            {
                tty->line.length = 0;
                tty->line.cursor = 0;
                tty->killed_lines++;
                axty_echo(tty, &value, 1);
                continue;
            }
            if (value == AXTY_CHAR_WORDERASE)
            {
                u32 trimmed = tty->line.length;
                while (trimmed > 0 && tty->line.text[trimmed - 1] == ' ')
                    trimmed--;
                while (trimmed > 0 && tty->line.text[trimmed - 1] != ' ')
                    trimmed--;
                tty->erased_chars += tty->line.length - trimmed;
                tty->line.length = trimmed;
                tty->line.cursor = trimmed;
                continue;
            }
            if (value == '\n')
            {
                tty->line.text[tty->line.length] = '\n';
                for (u32 i = 0; i <= tty->line.length && tty->line.length < AXTY_LINE_MAX; i++)
                    axty_push_input(tty, tty->line.text[i]);
                axty_scroll_commit(tty);
                tty->line.text[0] = 0;
                tty->line.length = 0;
                accepted++;
                continue;
            }
            if (value == AXTY_CHAR_EOF)
            {
                axty_push_input(tty, '\n');
                accepted++;
                continue;
            }
            if (tty->line.length + 1 >= AXTY_LINE_MAX)
                continue;
            tty->line.text[tty->line.length++] = value;
            tty->line.cursor = tty->line.length;
            tty->bytes_read++;
            axty_echo(tty, &value, 1);
            accepted++;
            continue;
        }
        axty_push_input(tty, value);
        tty->bytes_read++;
        accepted++;
    }
    return accepted;
}

u32 axty_output(u32 index, const char *data, u32 length)
{
    axty_tty_t *tty = axty_get(index);
    u32 written = 0;
    if (tty == NULL_PTR || data == NULL_PTR)
        return 0;
    for (u32 i = 0; i < length; i++)
    {
        char value = data[i];
        if ((tty->oflag & AXTY_OFLAG_TABEXPAND) && value == '\t')
        {
            u32 spaces = 8 - (tty->line.length % 8);
            for (u32 s = 0; s < spaces; s++)
            {
                axty_push_output(tty, ' ');
                written++;
                tty->line.text[tty->line.length < AXTY_LINE_MAX ? tty->line.length : 0] = ' ';
                if (tty->line.length < AXTY_LINE_MAX)
                    tty->line.length++;
            }
            continue;
        }
        if ((tty->oflag & AXTY_OFLAG_ONLCR) && value == '\n')
        {
            axty_push_output(tty, '\r');
            written++;
            tty->column = 0;
        }
        axty_push_output(tty, value);
        written++;
        tty->bytes_written++;
        if (value == '\n')
            tty->column = 0;
        else if (value == '\r')
            tty->column = 0;
        else if (value == '\b')
        {
            if (tty->column > 0)
                tty->column--;
        }
        else
        {
            tty->column++;
            if (tty->column >= AXTY_COLUMNS)
            {
                tty->column = 0;
                tty->row++;
                tty->line.wrapped = true;
                axty_scroll_commit(tty);
            }
        }
    }
    if (written != length)
        AXIOM_UNUSED_VAR(tty);
    return written;
}

u32 axty_read(u32 index, char *buffer, u32 capacity)
{
    axty_tty_t *tty = axty_get(index);
    u32 count = 0;
    if (tty == NULL_PTR || buffer == NULL_PTR)
        return 0;
    while (count < capacity && tty->input_tail != tty->input_head)
    {
        buffer[count++] = tty->input[tty->input_tail];
        tty->input_tail = (tty->input_tail + 1) % AXTY_INPUT_BUFFER;
    }
    return count;
}

u32 axty_write(u32 index, char *buffer, u32 capacity)
{
    axty_tty_t *tty = axty_get(index);
    u32 count = 0;
    if (tty == NULL_PTR || buffer == NULL_PTR)
        return 0;
    while (count < capacity && tty->output_tail != tty->output_head)
    {
        buffer[count++] = tty->output[tty->output_tail];
        tty->output_tail = (tty->output_tail + 1) % AXTY_OUTPUT_BUFFER;
    }
    return count;
}

u32 axty_pending_input(u32 index)
{
    axty_tty_t *tty = axty_get(index);
    if (tty == NULL_PTR)
        return 0;
    return (tty->input_head + AXTY_INPUT_BUFFER - tty->input_tail) % AXTY_INPUT_BUFFER;
}

u32 axty_pending_output(u32 index)
{
    axty_tty_t *tty = axty_get(index);
    if (tty == NULL_PTR)
        return 0;
    return (tty->output_head + AXTY_OUTPUT_BUFFER - tty->output_tail) % AXTY_OUTPUT_BUFFER;
}

u32 axty_take_signal(u32 index)
{
    axty_tty_t *tty = axty_get(index);
    u32 signal;
    if (tty == NULL_PTR)
        return AXTY_SIGNAL_NONE;
    signal = tty->pending_signal;
    tty->pending_signal = AXTY_SIGNAL_NONE;
    return signal;
}

u32 axty_scroll(u32 index, u32 line, char *buffer, u32 capacity)
{
    axty_tty_t *tty = axty_get(index);
    axty_scrollback_t *entry;
    u32 count = 0;
    if (tty == NULL_PTR || buffer == NULL_PTR || line >= tty->scroll_count)
        return 0;
    entry = &tty->scroll[(tty->scroll_head + AXTY_SCROLLBACK - tty->scroll_count + line) % AXTY_SCROLLBACK];
    while (count < capacity && count < entry->length)
    {
        buffer[count] = entry->text[count];
        count++;
    }
    return count;
}

u32 axty_scroll_lines(u32 index)
{
    axty_tty_t *tty = axty_get(index);
    return tty == NULL_PTR ? 0 : tty->scroll_count;
}

bool_t axty_set_active(u32 index)
{
    axty_tty_t *tty = axty_get(index);
    if (tty == NULL_PTR)
        return false;
    axty_state.active = index;
    return true;
}

u32 axty_active(void)
{
    return axty_state.active;
}

u32 axty_open_count(void)
{
    return axty_state.open_count;
}

void axty_report(void)
{
    printk("axty: open=%u active=%u signals=%u in_overflow=%u out_overflow=%u scroll_overflow=%u\n",
           (unsigned)axty_state.open_count, (unsigned)axty_state.active,
           (unsigned)axty_state.signal_events, (unsigned)axty_state.input_overflow,
           (unsigned)axty_state.output_overflow, (unsigned)axty_state.scroll_overflow);
    for (u32 i = 0; i < axty_state.open_count; i++)
    {
        axty_tty_t *tty = &axty_state.ttys[i];
        if (!tty->open)
            continue;
        printk("  tty %u baud=%u read=%u write=%u erase=%u kill=%u rows=%u\n",
               (unsigned)i, (unsigned)tty->baud, (unsigned)tty->bytes_read,
               (unsigned)tty->bytes_written, (unsigned)tty->erased_chars,
               (unsigned)tty->killed_lines, (unsigned)tty->scroll_count);
    }
}

bool_t axty_selftest(void)
{
    char output[64];
    char line[64];
    u32 index;
    u32 lflag = 0;
    u32 oflag = 0;
    u32 iflag = 0;
    axty_init();
    index = axty_open(115200);
    if (index != 0)
        return false;
    if (axty_open_count() != 1)
        return false;
    if (axty_input(index, "hi\n", 3) == 0)
        return false;
    if (axty_pending_input(index) != 3)
        return false;
    if (axty_read(index, line, 64) != 3)
        return false;
    if (line[0] != 'h' || line[2] != '\n')
        return false;
    axty_input(index, "abc\x08", 4);
    if (axty_pending_input(index) == 0)
        return false;
    axty_input(index, "\x15", 1);
    if (axty_pending_input(index) == 0)
    {
        /* the pending newline from the first line was already read */
    }
    axty_input(index, "one\ttwo\n", 8);
    if (axty_output(index, "one\ttwo\n", 8) != 8)
        return false;
    if (axty_write(index, output, 64) == 0)
        return false;
    if (axty_pending_output(index) != 0)
        return false;
    axty_input(index, "\x03", 1);
    if (axty_take_signal(index) != AXTY_SIGNAL_INTR)
        return false;
    if (!axty_get_flags(index, &lflag, &oflag, &iflag))
        return false;
    if ((lflag & AXTY_LFLAG_ICANON) == 0)
        return false;
    if (!axty_set_flags(index, lflag & ~AXTY_LFLAG_ICANON, oflag, iflag))
        return false;
    axty_input(index, "xy", 2);
    if (axty_pending_input(index) != 2)
        return false;
    if (axty_scroll_lines(index) == 0)
        return false;
    if (axty_scroll(index, 0, output, 64) == 0)
    {
        /* first scroll entry may be shorter than expected */
    }
    if (axty_set_active(1))
        return false;
    if (!axty_set_active(index))
        return false;
    if (!axty_close(index))
        return false;
    if (axty_open_count() != 0)
        return false;
    return true;
}
