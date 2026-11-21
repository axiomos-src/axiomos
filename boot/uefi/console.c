/* AxiomOS UEFI loader - console output.
 *
 * FreeType and friends obviously are not available inside firmware, so the
 * loader prints through EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL before graphics are
 * initialised, and through a tiny built-in 5x7 dot matrix afterwards.  The
 * dot-matrix font is also used by the boot splash so that the loader does not
 * depend on any asset files being present on the ESP.
 */

#include "loader.h"

extern const unsigned char font5x7[96][5];

static efi_simple_text_output_protocol *g_out;
static efi_uintn g_cols;
static efi_uintn g_rows;

/* ---------------- 16550 UART mirror -------------------------------- */
/* The firmware console is invisible in a headless VM, so every character
 * printed through con_puts is mirrored to COM1 as well.  This is the only
 * reliable diagnostic channel between the loader and the host. */
#define UART_PORT   0x3F8u

static void uart_init(void)
{
    __asm__ __volatile__("outb %%al, %%dx" : : "a"((u8)0x00), "d"((u16)(UART_PORT + 1)));  /* IER */
    __asm__ __volatile__("outb %%al, %%dx" : : "a"((u8)0x80), "d"((u16)(UART_PORT + 3)));  /* DLAB */
    __asm__ __volatile__("outb %%al, %%dx" : : "a"((u8)0x01), "d"((u16)(UART_PORT + 0)));  /* 115200 */
    __asm__ __volatile__("outb %%al, %%dx" : : "a"((u8)0x00), "d"((u16)(UART_PORT + 1)));
    __asm__ __volatile__("outb %%al, %%dx" : : "a"((u8)0x03), "d"((u16)(UART_PORT + 3)));  /* 8N1 */
    __asm__ __volatile__("outb %%al, %%dx" : : "a"((u8)0xC7), "d"((u16)(UART_PORT + 2)));  /* FIFO */
    __asm__ __volatile__("outb %%al, %%dx" : : "a"((u8)0x0B), "d"((u16)(UART_PORT + 4)));  /* DTR|RTS|OUT2 */
}

static void uart_putc(char c)
{
    u16 base = (u16)UART_PORT;
    int guard = 0;

    while ((*(volatile u8 *)(u64)(base + 5) & 0x20) == 0 && guard < 100000)
        guard++;
    __asm__ __volatile__("outb %%al, %%dx" : : "a"((u8)c), "d"(base));
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

void con_init(efi_system_table *st)
{
    uart_init();
    g_out = st->con_out;
    g_cols = 80;
    g_rows = 25;
    if (g_out) {
        g_out->clear_screen(g_out);
        g_out->enable_cursor(g_out, EFI_TRUE);
        g_out->query_mode(g_out, 0, &g_cols, &g_rows);
    }
}

void con_clear(void)
{
    if (g_out)
        g_out->clear_screen(g_out);
}

void con_puts(const char *s)
{
    efi_char16 buf[256];
    efi_uintn n = 0;
    /* When the firmware console is available it already terminates on the
     * serial port (OVMF's ConSplitter includes the serial terminal), so mirroring
     * to COM1 as well would print every line twice. */
    if (!g_out) {
        uart_puts(s);
        return;
    }
    while (*s) {
        buf[n++] = (efi_char16)*s++;
        if (n == 255) {
            buf[n] = 0;
            g_out->output_string(g_out, buf);
            n = 0;
        }
    }
    if (n) {
        buf[n] = 0;
        g_out->output_string(g_out, buf);
    }
}

void con_hex(u64 value)
{
    char buf[24];
    u_to_hex(value, buf, 1);
    con_puts("0x");
    con_puts(buf);
}

void con_dec(u64 value)
{
    char buf[24];
    u_to_dec(value, buf);
    con_puts(buf);
}

/* A very small printf subset:
 *   %s %c %d %i %u %x %X %p %% %l (l is an ignored length modifier prefix)
 */
void con_printf(const char *fmt, ...)
{
    __builtin_va_list args;
    char buf[32];

    __builtin_va_start(args, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            char tmp[2] = { *fmt++, 0 };
            con_puts(tmp);
            continue;
        }
        fmt++;
        /* Width and precision specifiers (for example "%-6s") are consumed and
         * ignored: the loader never pads, but it must not print them either. */
        while (*fmt == '-' || *fmt == '+' || *fmt == '0' || *fmt == '.' ||
               *fmt == ' ' || (*fmt >= '1' && *fmt <= '9')) {
            fmt++;
        }
        if (*fmt == 'l' && (*(fmt + 1) == 'l' || *(fmt + 1) == 'd' ||
                            *(fmt + 1) == 'u' || *(fmt + 1) == 'x')) {
            fmt++;          /* consume the 'l' */
            if (*fmt == 'l')
                fmt++;      /* consume the second 'l' */
        }
        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(args, const char *);
            con_puts(s ? s : "(null)");
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(args, int);
            char tmp[2] = { c, 0 };
            con_puts(tmp);
            break;
        }
        case 'd':
        case 'i': {
            i64 v = (i64)__builtin_va_arg(args, long long);
            if (v < 0) {
                con_puts("-");
                v = -v;
            }
            u_to_dec((u64)v, buf);
            con_puts(buf);
            break;
        }
        case 'u': {
            u64 v = (u64)__builtin_va_arg(args, unsigned long long);
            u_to_dec(v, buf);
            con_puts(buf);
            break;
        }
        case 'x':
        case 'X': {
            u64 v = (u64)__builtin_va_arg(args, unsigned long long);
            u_to_hex(v, buf, 1);
            con_puts(*fmt == 'X' ? "0x" : "");
            con_puts(buf);
            break;
        }
        case 'p': {
            u64 v = (u64)__builtin_va_arg(args, void *);
            u_to_hex(v, buf, 8);
            con_puts("0x");
            con_puts(buf);
            break;
        }
        case '%':
            con_puts("%");
            break;
        case 'b': {   /* boolean, AxiomOS extension: prints on/off */
            int v = __builtin_va_arg(args, int);
            con_puts(v ? "on" : "off");
            break;
        }
        default:
            con_puts("%?");
            break;
        }
        fmt++;
    }
    __builtin_va_end(args);
}

/* ------------------------------------------------------------------ */
/* Boot splash helpers (independent of GOP state)                      */
/* ------------------------------------------------------------------ */

/* Draw a glyph using the 5x7 table into a linear framebuffer. */
void con_draw_char(u8 *fb, u32 pitch, u32 x, u32 y, char c, u32 fg, u32 scale)
{
    const unsigned char *glyph;
    u32 row, col;
    if (c < 32 || c > 127)
        c = '?';
    glyph = font5x7[(int)c - 32];
    for (row = 0; row < 7; row++) {
        unsigned char bits = glyph[row];
        for (col = 0; col < 5; col++) {
            if (!(bits & (0x80 >> col)))
                continue;
            for (u32 sy = 0; sy < scale; sy++) {
                for (u32 sx = 0; sx < scale; sx++) {
                    u32 px = x + col * scale + sx;
                    u32 py = y + row * scale + sy;
                    u32 *dst = (u32 *)(fb + (u64)py * pitch + (u64)px * 4);
                    *dst = fg;
                }
            }
        }
    }
}

void con_draw_string(u8 *fb, u32 pitch, u32 x, u32 y, const char *s, u32 fg, u32 scale)
{
    while (*s) {
        con_draw_char(fb, pitch, x, y, *s, fg, scale);
        x += 6 * scale;
        s++;
    }
}


