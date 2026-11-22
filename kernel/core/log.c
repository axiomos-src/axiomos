/* AxiomOS - kernel logging and formatted output.
 *
 * printk() writes to the serial console, an in-memory ring buffer (dumped
 * after a panic) and, once the framebuffer console exists, to the screen.
 * The formatting engine supports the printf subset kernel developers actually
 * use: %s %c %d %i %u %x %X %p %o %b, width, zero padding, left alignment,
 * plus AxiomOS extensions %Zb (on/off word).
 */

#include <stdarg.h>
#include <axiom/kernel.h>

#define LOG_RING_SIZE 8192

static char           g_log_ring[LOG_RING_SIZE];
static u32            g_log_head;
static u32            g_log_tail;
static enum log_level g_log_level = LOG_INFO;
static u32            g_logging_depth;

static const char *level_names[] = { "trace", "debug", "info", "warn",
                                     "error", "fatal" };

/* Framebuffer console hook, registered by the GUI/text console subsystem. */
void (*g_console_sink)(const char *text) AXIOM_WEAK;

/* ------------------------------------------------------------------ */
/* Ring buffer                                                        */
/* ------------------------------------------------------------------ */

static void ring_push(char c)
{
    u32 next = (g_log_head + 1) % LOG_RING_SIZE;
    if (next == g_log_tail)
        g_log_tail = (g_log_tail + 1) % LOG_RING_SIZE;
    g_log_ring[g_log_head] = c;
    g_log_head = next;
}

void klog_dump_ring(void)
{
    u32 i = g_log_tail;
    serial_puts("\n--- kernel log ring ---\n");
    while (i != g_log_head) {
        serial_putc(g_log_ring[i]);
        i = (i + 1) % LOG_RING_SIZE;
    }
    serial_puts("--- end of ring ---\n");
}

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

void klog_init(enum log_level level)
{
    g_log_level = level;
    g_log_head = g_log_tail = 0;
    g_logging_depth = 0;
}

void klog_set_level(enum log_level level)
{
    if (level <= LOG_FATAL)
        g_log_level = level;
}

enum log_level klog_level(void)
{
    return g_log_level;
}

/* ------------------------------------------------------------------ */
/* Primitives                                                         */
/* ------------------------------------------------------------------ */

void kputchar(char c)
{
    serial_putc(c);
    ring_push(c);
}

void kputs(const char *s)
{
    if (!s)
        s = "(null)";
    while (*s)
        kputchar(*s++);
}

void kputhex(u64 value)
{
    char buf[20];
    int i = 19;
    buf[i] = 0;
    if (!value) {
        kputchar('0');
        return;
    }
    while (value && i > 0) {
        int nibble = (int)(value & 0xF);
        buf[--i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        value >>= 4;
    }
    kputs(&buf[i]);
}

void kputdec(u64 value)
{
    char buf[24];
    int i = 0;
    if (!value) {
        kputchar('0');
        return;
    }
    while (value && i < 23) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i--)
        kputchar(buf[i]);
}

/* ------------------------------------------------------------------ */
/* Formatting engine                                                  */
/* ------------------------------------------------------------------ */

struct fmt_spec {
    bool_t left;
    bool_t plus;
    bool_t zero;
    bool_t alt;
    u32    width;
    u32    precision;
};

static void emit_padded(const char *s, u32 len, const struct fmt_spec *spec)
{
    u32 pad = (spec->width > len) ? spec->width - len : 0;
    if (spec->left) {
        for (u32 i = 0; i < len; i++)
            kputchar(s[i]);
        while (pad--)
            kputchar(' ');
    } else {
        while (pad--)
            kputchar(spec->zero ? '0' : ' ');
        for (u32 i = 0; i < len; i++)
            kputchar(s[i]);
    }
}

static u32 format_unsigned(u64 value, u32 base, bool_t upper, char *out, u32 cap)
{
    char tmp[64];
    u32 n = 0, i = 0;
    if (!value) {
        if (cap)
            out[0] = '0';
        return cap ? 1 : 0;
    }
    while (value && n < 64) {
        u32 d = (u32)(value % base);
        tmp[n++] = (char)(d < 10 ? '0' + d
                                 : (upper ? 'A' : 'a') + d - 10);
        value /= base;
    }
    while (n && i < cap)
        out[i++] = tmp[--n];
    return i;
}

static u32 format_signed(i64 value, char *out, u32 cap)
{
    char tmp[32];
    u32 n = 0, i = 0;
    u64 magnitude = value < 0 ? (u64)(-value) : (u64)value;
    if (!magnitude) {
        if (cap)
            out[0] = '0';
        return cap ? 1 : 0;
    }
    while (magnitude && n < 32) {
        tmp[n++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    }
    if (value < 0 && i < cap)
        out[i++] = '-';
    while (n && i < cap)
        out[i++] = tmp[--n];
    return i;
}

static void vprintk(const char *fmt, va_list ap)
{
    struct fmt_spec spec;

    if (g_logging_depth)
        return;
    g_logging_depth++;

    while (*fmt) {
        char c = *fmt++;
        if (c != '%') {
            kputchar(c);
            continue;
        }
        spec.left = spec.plus = spec.zero = spec.alt = false;
        spec.width = spec.precision = 0;

        for (bool_t more = true; more;) {
            switch (*fmt) {
            case '-': spec.left = true; fmt++; break;
            case '+': spec.plus = true; fmt++; break;
            case '0': spec.zero = true; fmt++; break;
            case '#': spec.alt = true; fmt++; break;
            default:  more = false; break;
            }
        }
        while (*fmt >= '0' && *fmt <= '9')
            spec.width = spec.width * 10 + (u32)(*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9')
                spec.precision = spec.precision * 10 + (u32)(*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h' || *fmt == 'j' ||
               *fmt == 't')
            fmt++;

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            u32 len = 0;
            if (!s)
                s = "(null)";
            while (s[len])
                len++;
            if (spec.precision && len > spec.precision)
                len = spec.precision;
            emit_padded(s, len, &spec);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            emit_padded(&ch, 1, &spec);
            break;
        }
        case 'd':
        case 'i': {
            char buf[40];
            i64 v = (i64)va_arg(ap, long long);
            u32 len = format_signed(v, buf, sizeof(buf));
            if (v < 0)
                spec.zero = false;   /* padding goes before the sign */
            emit_padded(buf, len, &spec);
            break;
        }
        case 'u': {
            char buf[40];
            u64 v = (u64)va_arg(ap, unsigned long long);
            u32 len = format_unsigned(v, 10, false, buf, sizeof(buf));
            emit_padded(buf, len, &spec);
            break;
        }
        case 'x':
        case 'X':
        case 'p': {
            char buf[40];
            u64 v;
            if (*fmt == 'p') {
                v = (u64)va_arg(ap, void *);
                if (!v) {
                    kputs("(nil)");
                    break;
                }
                kputs("0x");
                spec.zero = true;
                if (spec.width >= 3)
                    spec.width -= 2;
            } else {
                v = (u64)va_arg(ap, unsigned long long);
                if (spec.alt) {
                    kputs(*fmt == 'X' ? "0X" : "0x");
                    if (spec.width >= 2) {
                        spec.width -= 2;
                        spec.zero = false;
                    }
                }
            }
            u32 len = format_unsigned(v, 16, *fmt == 'X', buf, sizeof(buf));
            emit_padded(buf, len, &spec);
            break;
        }
        case 'o': {
            char buf[40];
            u64 v = (u64)va_arg(ap, unsigned long long);
            u32 len = format_unsigned(v, 8, false, buf, sizeof(buf));
            emit_padded(buf, len, &spec);
            break;
        }
        case 'b': {
            char buf[70];
            u64 v = (u64)va_arg(ap, unsigned long long);
            u32 len = format_unsigned(v, 2, false, buf, sizeof(buf));
            emit_padded(buf, len, &spec);
            break;
        }
        case 'Z': {
            char kind = *++fmt;
            if (kind == 'b') {
                int v = va_arg(ap, int);
                kputs(v ? "on" : "off");
            } else {
                kputs("?");
            }
            break;
        }
        case '%':
            kputchar('%');
            break;
        case 0:
            g_logging_depth--;
            return;
        default:
            kputchar('%');
            kputchar(*fmt);
            break;
        }
        fmt++;
    }
    g_logging_depth--;
}

void printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
}

void klog(enum log_level level, const char *fmt, ...)
{
    va_list ap;
    if (level < g_log_level)
        return;
    if (level >= LOG_WARN) {
        kputchar('[');
        kputs(level_names[level]);
        kputs("] ");
    }
    va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
    kputchar('\n');
}

/* ------------------------------------------------------------------ */
/* Panic                                                              */
/* ------------------------------------------------------------------ */

void panic(const char *fmt, ...)
{
    va_list ap;
    cli();
    serial_puts("\n\n*** AXIOMOS KERNEL PANIC ***\nreason: ");
    va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
    serial_puts("\n\nregisters:\n  cr0=");
    serial_puthex(read_cr0());
    serial_puts(" cr2=");
    serial_puthex(read_cr2());
    serial_puts(" cr3=");
    serial_puthex(read_cr3());
    serial_puts(" cr4=");
    serial_puthex(read_cr4());
    serial_puts(" ticks=");
    serial_puthex(timer_ticks());
    serial_puts("\n");
    klog_dump_ring();
    serial_puts("system halted; reset the machine to reboot\n");

    if (g_console_sink)
        g_console_sink("*** KERNEL PANIC *** see serial log");
    for (;;) {
        cli();
        hlt();
    }
}

void kassert_fail(const char *expr, const char *file, int line)
{
    panic("assertion failed: %s at %s:%d", expr, file, line);
}
