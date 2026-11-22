/* AxiomOS - printf engine.
 *
 * The kernel console, the boot log, the GUI terminal and the syscall tracer all
 * format through this one engine.  It is deliberately freestanding: no locale,
 * no floating point, no allocation.  The output goes either straight into the
 * destination buffer (ax_vsnprintf semantics: truncate, always terminate, return
 * the untruncated length) or through ax_vscnprintf which reports the number of
 * characters actually stored, which is what the log ring wants.
 */

#include <axiom/lib.h>

#define FMT_FLAG_LEFT     0x01u
#define FMT_FLAG_PLUS     0x02u
#define FMT_FLAG_SPACE    0x04u
#define FMT_FLAG_HASH     0x08u
#define FMT_FLAG_ZERO     0x10u

#define FMT_LEN_NONE      0
#define FMT_LEN_HH        1
#define FMT_LEN_H         2
#define FMT_LEN_L         3
#define FMT_LEN_LL        4
#define FMT_LEN_Z         5

struct fmt_sink {
    char  *dst;
    usize  cap;       /* capacity including terminator */
    usize  len;       /* characters emitted so far */
    bool_t overflow;
};

static void sink_putc(struct fmt_sink *s, char c)
{
    if (s->dst && s->len + 1 < s->cap) {
        s->dst[s->len] = c;
    } else if (s->dst && s->len + 1 == s->cap) {
        /* keep the last byte for the terminator */
        s->overflow = true;
    } else {
        s->overflow = true;
    }
    s->len++;
}

static void sink_write(struct fmt_sink *s, const char *text, usize n)
{
    for (usize i = 0; i < n; i++) {
        sink_putc(s, text[i]);
    }
}

static void sink_repeat(struct fmt_sink *s, char c, usize count)
{
    while (count--) {
        sink_putc(s, c);
    }
}

static u32 parse_number(const char **pp)
{
    const char *p = *pp;
    u32 value = 0;

    while (ax_isdigit(*p)) {
        u32 digit = (u32)(*p - '0');
        if (value > 0x0FFFFFFF) {
            value = 0x0FFFFFFF;
        }
        value = value * 10 + digit;
        p++;
    }
    *pp = p;
    return value;
}

static u32 parse_flags(const char **pp)
{
    const char *p = *pp;
    u32 flags = 0;

    for (;;) {
        switch (*p) {
        case '-':
            flags |= FMT_FLAG_LEFT;
            p++;
            break;
        case '+':
            flags |= FMT_FLAG_PLUS;
            p++;
            break;
        case ' ':
            flags |= FMT_FLAG_SPACE;
            p++;
            break;
        case '#':
            flags |= FMT_FLAG_HASH;
            p++;
            break;
        case '0':
            flags |= FMT_FLAG_ZERO;
            p++;
            break;
        default:
            *pp = p;
            return flags;
        }
    }
}

static u32 parse_length(const char **pp)
{
    const char *p = *pp;

    switch (*p) {
    case 'h':
        if (p[1] == 'h') {
            *pp = p + 2;
            return FMT_LEN_HH;
        }
        *pp = p + 1;
        return FMT_LEN_H;
    case 'l':
        if (p[1] == 'l') {
            *pp = p + 2;
            return FMT_LEN_LL;
        }
        *pp = p + 1;
        return FMT_LEN_L;
    case 'z':
        *pp = p + 1;
        return FMT_LEN_Z;
    default:
        *pp = p;
        return FMT_LEN_NONE;
    }
}

/* Emit a fully formed number body with padding and sign handling. */
static void emit_number(struct fmt_sink *s, u64 value, bool_t negative,
                        int base, bool_t upper, u32 flags, u32 width,
                        u32 precision)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;
    char body[32];
    usize digits_len = 0;
    usize total;
    char sign = 0;
    char prefix_c = 0;

    if (precision == 0 && value == 0) {
        digits_len = 0;
    } else {
        do {
            body[digits_len++] = digits[value % (u64)base];
            value /= (u64)base;
        } while (value && digits_len < sizeof(body));
    }

    if (negative) {
        sign = '-';
    } else if (flags & FMT_FLAG_PLUS) {
        sign = '+';
    } else if (flags & FMT_FLAG_SPACE) {
        sign = ' ';
    }
    if ((flags & FMT_FLAG_HASH) && base == 16) {
        prefix_c = 'x';
    } else if ((flags & FMT_FLAG_HASH) && base == 8) {
        prefix_c = 'o';
    }

    total = digits_len + (sign ? 1u : 0u) + (prefix_c ? 2u : 0u);
    if (precision > digits_len) {
        total += precision - digits_len;
    }
    if (width > total && !(flags & FMT_FLAG_LEFT) && !(flags & FMT_FLAG_ZERO)) {
        sink_repeat(s, ' ', width - total);
    }

    if (sign) {
        sink_putc(s, sign);
    }
    if (prefix_c == 'x') {
        sink_putc(s, '0');
        sink_putc(s, upper ? 'X' : 'x');
    } else if (prefix_c == 'o') {
        sink_putc(s, '0');
    }
    if (width > total && !(flags & FMT_FLAG_LEFT) && (flags & FMT_FLAG_ZERO) &&
        precision == 0) {
        sink_repeat(s, '0', width - total);
    }
    if (precision > digits_len) {
        sink_repeat(s, '0', precision - digits_len);
    }
    while (digits_len) {
        sink_putc(s, body[--digits_len]);
    }
    if (width > total && (flags & FMT_FLAG_LEFT)) {
        sink_repeat(s, ' ', width - total);
    }
}

/* Emit a string with width and precision, honouring the left flag. */
static void emit_string(struct fmt_sink *s, const char *text, u32 flags,
                        u32 width, u32 precision)
{
    usize len = text ? ax_strlen(text) : 0;

    if (!text) {
        text = "(null)";
        len = 6;
    }
    if (precision && precision < len) {
        len = precision;
    }
    if (width > len && !(flags & FMT_FLAG_LEFT)) {
        sink_repeat(s, ' ', width - len);
    }
    sink_write(s, text, len);
    if (width > len && (flags & FMT_FLAG_LEFT)) {
        sink_repeat(s, ' ', width - len);
    }
}

static int format_engine(struct fmt_sink *sink, const char *fmt, __builtin_va_list args)
{
    const char *p = fmt;

    while (*p) {
        if (*p != '%') {
            sink_putc(sink, *p++);
            continue;
        }
        p++;
        if (*p == '%') {
            sink_putc(sink, '%');
            p++;
            continue;
        }

        u32 flags = parse_flags(&p);
        u32 width = 0;
        u32 precision = 0;
        bool_t have_precision = false;

        if (*p == '*') {
            int w = __builtin_va_arg(args, int);
            if (w < 0) {
                flags |= FMT_FLAG_LEFT;
                width = (u32)(-w);
            } else {
                width = (u32)w;
            }
            p++;
        } else if (ax_isdigit(*p)) {
            width = parse_number(&p);
        }
        if (*p == '.') {
            p++;
            have_precision = true;
            if (*p == '*') {
                int prec = __builtin_va_arg(args, int);
                precision = prec > 0 ? (u32)prec : 0;
                p++;
            } else {
                precision = parse_number(&p);
            }
        }

        u32 length = parse_length(&p);
        char conv = *p ? *p++ : 0;

        switch (conv) {
        case 'd':
        case 'i': {
            i64 v;
            if (length == FMT_LEN_LL) {
                v = __builtin_va_arg(args, long long);
            } else if (length == FMT_LEN_L || length == FMT_LEN_Z) {
                v = __builtin_va_arg(args, long);
            } else {
                v = __builtin_va_arg(args, int);
            }
            if (length == FMT_LEN_H) {
                v = (i16)v;
            } else if (length == FMT_LEN_HH) {
                v = (i8)v;
            }
            bool_t negative = v < 0;
            u64 magnitude = negative ? ((u64)(-(v + 1)) + 1ULL) : (u64)v;
            emit_number(sink, magnitude, negative, 10, false, flags, width,
                        have_precision ? precision : 0);
            break;
        }
        case 'u': {
            u64 v;
            if (length == FMT_LEN_LL) {
                v = __builtin_va_arg(args, unsigned long long);
            } else if (length == FMT_LEN_L || length == FMT_LEN_Z) {
                v = __builtin_va_arg(args, unsigned long);
            } else {
                v = __builtin_va_arg(args, unsigned int);
            }
            if (length == FMT_LEN_H) {
                v = (u16)v;
            } else if (length == FMT_LEN_HH) {
                v = (u8)v;
            }
            emit_number(sink, v, false, 10, false, flags, width,
                        have_precision ? precision : 0);
            break;
        }
        case 'x':
        case 'X': {
            u64 v;
            if (length == FMT_LEN_LL) {
                v = __builtin_va_arg(args, unsigned long long);
            } else if (length == FMT_LEN_L || length == FMT_LEN_Z) {
                v = __builtin_va_arg(args, unsigned long);
            } else {
                v = __builtin_va_arg(args, unsigned int);
            }
            emit_number(sink, v, false, 16, conv == 'X', flags, width,
                        have_precision ? precision : 0);
            break;
        }
        case 'o': {
            u64 v = __builtin_va_arg(args, unsigned int);
            emit_number(sink, v, false, 8, false, flags, width,
                        have_precision ? precision : 0);
            break;
        }
        case 'b': {
            /* AxiomOS extension: binary, used by the memory map dump. */
            u64 v = __builtin_va_arg(args, unsigned long long);
            emit_number(sink, v, false, 2, false, flags, width,
                        have_precision ? precision : 0);
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(args, int);
            if (width > 1 && !(flags & FMT_FLAG_LEFT)) {
                sink_repeat(sink, ' ', width - 1);
            }
            sink_putc(sink, c);
            if (width > 1 && (flags & FMT_FLAG_LEFT)) {
                sink_repeat(sink, ' ', width - 1);
            }
            break;
        }
        case 's':
            emit_string(sink, __builtin_va_arg(args, const char *), flags, width,
                        have_precision ? precision : 0);
            break;
        case 'p': {
            u64 v = (u64)__builtin_va_arg(args, void *);
            sink_putc(sink, '0');
            sink_putc(sink, 'x');
            emit_number(sink, v, false, 16, false, FMT_FLAG_ZERO,
                        width > 2 ? width - 2 : 16, 0);
            break;
        }
        case 0:
            p--;
            break;
        default:
            sink_putc(sink, '%');
            sink_putc(sink, conv);
            break;
        }
    }
    return (int)sink->len;
}

static void sink_init(struct fmt_sink *s, char *dst, usize cap)
{
    s->dst = dst;
    s->cap = cap;
    s->len = 0;
    s->overflow = false;
    if (dst && cap) {
        dst[0] = 0;
    }
}

int ax_vsnprintf(char *dst, usize cap, const char *fmt, __builtin_va_list args)
{
    struct fmt_sink sink;
    int written;

    sink_init(&sink, dst, cap);
    written = format_engine(&sink, fmt, args);
    if (dst && cap) {
        usize stored = sink.len < cap ? sink.len : cap - 1;
        dst[stored] = 0;
    }
    return written;
}

int ax_vscnprintf(char *dst, usize cap, const char *fmt, __builtin_va_list args)
{
    struct fmt_sink sink;
    int written;

    sink_init(&sink, dst, cap);
    written = format_engine(&sink, fmt, args);
    if (dst && cap) {
        usize stored = sink.len < cap ? sink.len : cap - 1;
        dst[stored] = 0;
        return (int)stored;
    }
    (void)written;
    return 0;
}

int ax_snprintf(char *dst, usize cap, const char *fmt, ...)
{
    __builtin_va_list args;
    int written;

    __builtin_va_start(args, fmt);
    written = ax_vsnprintf(dst, cap, fmt, args);
    __builtin_va_end(args);
    return written;
}
