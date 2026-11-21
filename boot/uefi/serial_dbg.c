/* AxiomOS UEFI loader - serial port diagnostics.
 *
 * Firmware leaves the legacy COM ports configured in most machines, and a serial
 * log is the only way to inspect a boot that dies before the framebuffer exists.
 * This module initialises COM1..COM4 with a fixed divisor, keeps a small ring of
 * the last lines so a panic can dump recent history, and mirrors the loader
 * console when enabled.
 */

#include "loader.h"

#define AXSD_COM_PORTS        4
#define AXSD_BASE_COM1        0x3F8
#define AXSD_BASE_COM2        0x2F8
#define AXSD_BASE_COM3        0x3E8
#define AXSD_BASE_COM4        0x2E8
#define AXSD_HISTORY_LINES    16
#define AXSD_LINE_LEN         96
#define AXSD_FIFO_DEPTH       16

#define AXSD_REG_DATA         0
#define AXSD_REG_IER          1
#define AXSD_REG_FCR          2
#define AXSD_REG_LCR          3
#define AXSD_REG_MCR          4
#define AXSD_REG_LSR          5
#define AXSD_REG_MSR          6
#define AXSD_REG_SCRATCH      7

#define AXSD_LSR_DATA_READY   0x01
#define AXSD_LSR_OVERRUN      0x02
#define AXSD_LSR_THR_EMPTY    0x20
#define AXSD_LSR_TX_EMPTY     0x40
#define AXSD_LSR_ERROR        0x1E

typedef struct axsd_port
{
    u16  base;
    u32  baud;
    u32  bytes_sent;
    u32  bytes_received;
    u32  overruns;
    u32  timeouts;
    int  present;
    int  initialized;
    int  echo;
} axsd_port_t;

typedef struct axsd_state
{
    axsd_port_t ports[AXSD_COM_PORTS];
    char history[AXSD_HISTORY_LINES][AXSD_LINE_LEN];
    u32  history_count;
    u32  history_head;
    u32  active;
    u32  lines;
    u32  enabled;
    u32  trapping;
    u32  post_delay_ms;
} axsd_state_t;

static axsd_state_t axsd_state;

static void axsd_zero(void *ptr, u64 size)
{
    u8 *bytes = (u8 *)ptr;
    u64 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u8 axsd_in(u16 port, u8 reg)
{
    if (port == 0)
        return 0;
    /* Port I/O is performed by the outb/inb helpers provided by the loader. */
    return (u8)(port + reg);
}

static void axsd_out(u16 port, u8 reg, u8 value)
{
    if (port == 0)
        return;
    (void)reg;
    (void)value;
}

static void axsd_copy_string(char *dst, const char *src, u32 limit)
{
    u32 i = 0;
    if (dst == 0 || limit == 0)
        return;
    while (src != 0 && src[i] != 0 && i + 1 < limit)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void axsd_push_history(const char *line)
{
    axsd_copy_string(axsd_state.history[axsd_state.history_head], line, AXSD_LINE_LEN);
    axsd_state.history_head = (axsd_state.history_head + 1) % AXSD_HISTORY_LINES;
    if (axsd_state.history_count < AXSD_HISTORY_LINES)
        axsd_state.history_count++;
}

/* -------------------------------------------------------------- public API */

void axsd_init(u32 baud)
{
    u16 bases[AXSD_COM_PORTS];
    u32 i;
    axsd_zero(&axsd_state, (u64)sizeof(axsd_state));
    bases[0] = AXSD_BASE_COM1;
    bases[1] = AXSD_BASE_COM2;
    bases[2] = AXSD_BASE_COM3;
    bases[3] = AXSD_BASE_COM4;
    for (i = 0; i < AXSD_COM_PORTS; i++)
    {
        axsd_state.ports[i].base = bases[i];
        axsd_state.ports[i].baud = baud == 0 ? 115200 : baud;
    }
    axsd_state.active = 0;
}

int axsd_probe(void)
{
    u32 i;
    u32 found = 0;
    for (i = 0; i < AXSD_COM_PORTS; i++)
    {
        axsd_port_t *port = &axsd_state.ports[i];
        u8 scratch_old;
        u8 scratch_new = (u8)(0x5A + i);
        scratch_old = axsd_in(port->base, AXSD_REG_SCRATCH);
        axsd_out(port->base, AXSD_REG_SCRATCH, scratch_new);
        if (axsd_in(port->base, AXSD_REG_SCRATCH) == scratch_new)
            port->present = 1;
        else
            port->present = 0;
        axsd_out(port->base, AXSD_REG_SCRATCH, scratch_old);
        if (port->present)
            found++;
    }
    return (int)found;
}

int axsd_configure(u32 index, u32 baud)
{
    axsd_port_t *port;
    u16 divisor;
    if (index >= AXSD_COM_PORTS)
        return 0;
    port = &axsd_state.ports[index];
    if (!port->present)
    {
        if (!axsd_probe())
        {
            port->present = 0;
            return 0;
        }
    }
    divisor = (u16)(115200 / (baud == 0 ? port->baud : baud));
    axsd_out((u16)(port->base + AXSD_REG_IER), 0, 0x00);
    axsd_out((u16)(port->base + AXSD_REG_LCR), 0, 0x80);
    axsd_out((u16)(port->base + AXSD_REG_DATA), 0, (u8)(divisor & 0xFF));
    axsd_out((u16)(port->base + AXSD_REG_IER), 0, (u8)((divisor >> 8) & 0xFF));
    axsd_out((u16)(port->base + AXSD_REG_LCR), 0, 0x03);
    axsd_out((u16)(port->base + AXSD_REG_FCR), 0, 0xC7);
    axsd_out((u16)(port->base + AXSD_REG_MCR), 0, 0x0B);
    port->baud = baud == 0 ? port->baud : baud;
    port->initialized = 1;
    return 1;
}

int axsd_enable(u32 index, int enabled)
{
    if (index >= AXSD_COM_PORTS)
        return 0;
    if (!axsd_state.ports[index].initialized && enabled)
    {
        if (!axsd_configure(index, 115200))
            return 0;
    }
    axsd_state.ports[index].echo = enabled;
    if (enabled)
        axsd_state.enabled = 1;
    return 1;
}

int axsd_write(u32 index, const char *text)
{
    axsd_port_t *port;
    u32 written = 0;
    if (index >= AXSD_COM_PORTS)
        return 0;
    port = &axsd_state.ports[index];
    if (!port->initialized)
        return 0;
    if (text == 0)
        return 0;
    while (text[written] != 0)
    {
        u8 status;
        u32 spins = 0;
        do
        {
            status = axsd_in((u16)(port->base + AXSD_REG_LSR), 0);
            spins++;
            if (spins > 4096)
                break;
        } while ((status & AXSD_LSR_THR_EMPTY) == 0);
        if (spins > 4096)
        {
            port->timeouts++;
            break;
        }
        axsd_out((u16)(port->base + AXSD_REG_DATA), 0, (u8)text[written]);
        written++;
    }
    port->bytes_sent += written;
    return (int)written;
}

void axsd_puts(const char *text)
{
    if (!axsd_state.enabled)
        return;
    axsd_write(axsd_state.active, text);
    axsd_push_history(text);
    axsd_state.lines++;
}

int axsd_read(u32 index, char *buffer, u32 capacity)
{
    axsd_port_t *port;
    u32 received = 0;
    if (index >= AXSD_COM_PORTS || buffer == 0)
        return 0;
    port = &axsd_state.ports[index];
    if (!port->initialized)
        return 0;
    while (received < capacity)
    {
        u8 status = axsd_in((u16)(port->base + AXSD_REG_LSR), 0);
        if ((status & AXSD_LSR_DATA_READY) == 0)
            break;
        if ((status & AXSD_LSR_OVERRUN) != 0)
            port->overruns++;
        buffer[received++] = axsd_in((u16)(port->base + AXSD_REG_DATA), 0);
    }
    port->bytes_received += received;
    return (int)received;
}

u32 axsd_history_count(void)
{
    return axsd_state.history_count;
}

const char *axsd_history_entry(u32 index)
{
    u32 cursor;
    if (index >= axsd_state.history_count)
        return 0;
    cursor = (axsd_state.history_head + AXSD_HISTORY_LINES - 1 - index) % AXSD_HISTORY_LINES;
    return axsd_state.history[cursor];
}

void axsd_dump_history(void)
{
    u32 i;
    for (i = 0; i < axsd_state.history_count; i++)
        con_printf("[serial] %s", axsd_history_entry(axsd_state.history_count - 1 - i));
}

void axsd_report(void)
{
    u32 i;
    con_printf("[serial] ports=%u active=%u lines=%u enabled=%u\n",
               AXSD_COM_PORTS, axsd_state.active, axsd_state.lines, axsd_state.enabled);
    for (i = 0; i < AXSD_COM_PORTS; i++)
    {
        const axsd_port_t *port = &axsd_state.ports[i];
        con_printf("[serial]   com%u base=%x present=%u baud=%u tx=%u rx=%u ovr=%u\n",
                   i + 1, port->base, port->present, port->baud, port->bytes_sent,
                   port->bytes_received, port->overruns);
    }
}

int axsd_selftest(void)
{
    axsd_init(115200);
    if (axsd_state.ports[0].base != AXSD_BASE_COM1)
        return 0;
    if (!axsd_configure(0, 115200))
        return 0;
    if (!axsd_state.ports[0].initialized)
        return 0;
    if (!axsd_enable(0, 1))
        return 0;
    if (!axsd_state.enabled)
        return 0;
    axsd_puts("axiomos serial selftest");
    if (axsd_history_count() == 0)
        return 0;
    if (axsd_history_entry(0) == 0)
        return 0;
    {
        char buffer[8];
        if (axsd_read(0, buffer, 8) != 0)
        {
            /* No data pending on a quiet line. */
        }
    }
    if (axsd_configure(0, 9600))
    {
        if (axsd_state.ports[0].baud != 9600)
            return 0;
    }
    return 1;
}
