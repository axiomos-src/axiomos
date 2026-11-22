/* AxiomOS - COM1 serial driver.
 *
 * The serial port is the kernel's earliest and most reliable output channel:
 * it works before the framebuffer is mapped, before the heap exists, and it
 * survives a panic.  QEMU is configured with -serial stdio by the run script,
 * so every log line lands in the terminal that launched the VM.
 */

#include <axiom/kernel.h>

#define COM1_PORT 0x3F8

/* 16550 UART register offsets from the base port. */
#define UART_DATA        0
#define UART_IER         1
#define UART_FCR         2
#define UART_LCR         3
#define UART_MCR         4
#define UART_LSR         5
#define UART_MSR         6
#define UART_SCR         7

#define UART_LSR_THRE    0x20   /* transmit holding register empty */
#define UART_LSR_DR      0x01   /* data ready                      */

static bool_t g_serial_ready;
static u16    g_serial_port = COM1_PORT;

AXIOM_INLINE void outb(u16 port, u8 value)
{
    __asm__ __volatile__("outb %0, %1" :: "a"(value), "Nd"(port) : "memory");
}

AXIOM_INLINE u8 inb(u16 port)
{
    u8 value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

AXIOM_INLINE void io_wait(void)
{
    outb(0x80, 0);
}

void serial_init(u16 port)
{
    g_serial_port = port;

    outb(port + UART_IER, 0x00);        /* disable interrupts            */
    outb(port + UART_LCR, 0x80);        /* enable DLAB                   */
    outb(port + UART_DATA, 0x01);       /* divisor low: 115200            */
    outb(port + UART_IER, 0x00);        /* divisor high                  */
    outb(port + UART_LCR, 0x03);        /* 8N1                           */
    outb(port + UART_FCR, 0xC7);        /* enable FIFO, clear, 14B       */
    outb(port + UART_MCR, 0x0B);        /* DTR + RTS + OUT2              */
    outb(port + UART_SCR, 0xA5);

    /* Verify that the scratch register holds what was written; if not there
     * is no UART at this address and output should silently no-op. */
    if (inb(port + UART_SCR) == 0xA5) {
        g_serial_ready = true;
        g_serial_ready = true;
    }
}

bool_t serial_ready(void)
{
    return g_serial_ready;
}

static bool_t serial_wait_tx(void)
{
    /* A stuck UART would hang the kernel forever; bound the wait. */
    for (u32 spins = 0; spins < 100000; spins++) {
        if (inb(g_serial_port + UART_LSR) & UART_LSR_THRE)
            return true;
        cpu_relax();
    }
    return false;
}

void serial_putc(char c)
{
    if (!g_serial_ready)
        return;
    if (c == '\n') {
        serial_wait_tx();
        outb(g_serial_port + UART_DATA, '\r');
    }
    if (!serial_wait_tx())
        return;
    outb(g_serial_port + UART_DATA, (u8)c);
}

void serial_puts(const char *s)
{
    if (!s)
        return;
    while (*s)
        serial_putc(*s++);
}

void serial_puthex(u64 v)
{
    char buf[20];
    int i = 19;
    buf[i] = 0;
    if (v == 0) {
        serial_putc('0');
        return;
    }
    while (v && i) {
        int nibble = (int)(v & 0xF);
        buf[--i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        v >>= 4;
    }
    serial_puts(&buf[i]);
}

/* Early debug helper used before the logging subsystem is initialised. */
void early_trace(const char *stage, u64 value)
{
    if (!g_serial_ready)
        return;
    serial_puts("[boot] ");
    serial_puts(stage);
    if (value) {
        serial_puts(" = 0x");
        serial_puthex(value);
    }
    serial_putc('\n');
}
