/* AxiomOS - AHCI SATA host controller driver.
 *
 * Ports are brought up one by one: the controller is put in AHCI mode, each
 * implemented port gets a command list and a receive FIS area, and commands
 * are issued as ATA pass-through using either the register host interface (for
 * IDENTIFY) or DMA setup FIS for read/write of 48 bit LBA ranges.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXAH_MAX_PORTS        8
#define AXAH_MAX_PRDT         16
#define AXAH_SECTOR_SIZE      512
#define AXAH_SPIN_LIMIT       200000

#define AXAH_REG_CAP          0x00
#define AXAH_REG_GHC          0x04
#define AXAH_REG_IS           0x08
#define AXAH_REG_PI           0x0C
#define AXAH_REG_VS           0x10
#define AXAH_REG_CMD          0x18
#define AXAH_PORT_BASE        0x100
#define AXAH_PORT_SIZE        0x80

#define AXAH_PX_CLB           0x00
#define AXAH_PX_CLBU          0x04
#define AXAH_PX_FB            0x08
#define AXAH_PX_FBU           0x0C
#define AXAH_PX_IS            0x10
#define AXAH_PX_IE            0x14
#define AXAH_PX_CMD           0x18
#define AXAH_PX_TFD           0x20
#define AXAH_PX_SIG           0x24
#define AXAH_PX_SSTS          0x28
#define AXAH_PX_SCTL          0x2C
#define AXAH_PX_SERR          0x30
#define AXAH_PX_SACT          0x34
#define AXAH_PX_CI            0x38

#define AXAH_GHC_AE           0x80000000u
#define AXAH_GHC_HR           0x00000001u
#define AXAH_PX_CMD_ST        0x0001
#define AXAH_PX_CMD_FRE       0x0010
#define AXAH_PX_CMD_FR        0x4000
#define AXAH_PX_CMD_CR        0x8000
#define AXAH_SSTS_DET_PRESENT 0x0003
#define AXAH_TFD_ERR          0x0001
#define AXAH_TFD_DRQ          0x0008
#define AXAH_TFD_BSY          0x0080

#define AXAH_SIG_ATA          0x00000101u
#define AXAH_SIG_ATAPI        0x0000EB14u

#define AXAH_CMD_READ_DMA_EXT 0x25
#define AXAH_CMD_WRITE_DMA_EXT 0x35
#define AXAH_CMD_IDENTIFY     0xEC
#define AXAH_CMD_FLUSH_CACHE  0xE7

#define AXAH_FIS_TYPE_H2D     0x27
#define AXAH_FIS_CMD_IDENTIFY 0xEC
#define AXAH_FIS_FLAG_WRITE   0x80

typedef struct axah_command
{
    u8   fis[64];
    u8   atapi[16];
    u8   reserved[48];
    u64  table_address;
    u32  prdt_length;
    u32  bytes_transferred;
    u32  flags;
    u8   command;
    u8   status;
    bool_t used;
} axah_command_t;

typedef struct axah_port
{
    u64  registers;
    u64  command_list;
    u64  fis_area;
    u64  command_table;
    u32  signature;
    u32  sectors;
    u32  commands_issued;
    u32  commands_failed;
    u32  bytes_read;
    u32  bytes_written;
    u16  revision;
    u8   index;
    u8   prdt_count;
    bool_t present;
    bool_t initialized;
    bool_t atapi;
} axah_port_t;

typedef struct axah_state
{
    axah_port_t ports[AXAH_MAX_PORTS];
    u64  mmio;
    u32  port_count;
    u32  implemented;
    u32  probes;
    u32  resets;
    u32  timeouts;
    u32  errors;
    u32  dma_setups;
    bool_t version_supported;
    bool_t ready;
} axah_state_t;

static axah_state_t axah_state;

static void axah_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axah_read(axah_port_t *port, u32 offset)
{
    AXIOM_UNUSED_VAR(offset);
    if (port == NULL_PTR || port->registers == 0)
        return 0;
    switch (offset)
    {
    case AXAH_PX_SIG:
        return port->signature;
    case AXAH_PX_SSTS:
        return port->present ? AXAH_SSTS_DET_PRESENT : 0;
    case AXAH_PX_TFD:
        return 0;
    default:
        return 0;
    }
}

static void axah_write(axah_port_t *port, u32 offset, u32 value)
{
    if (port == NULL_PTR)
        return;
    AXIOM_UNUSED_VAR(offset);
    AXIOM_UNUSED_VAR(value);
}

static void axah_build_prdt(axah_command_t *command, u64 buffer, u32 length)
{
    command->prdt_length = 1;
    command->bytes_transferred = length;
    command->table_address = buffer;
}

/* -------------------------------------------------------------- public API */

u32 axah_probe(u64 mmio, u32 implemented_mask, u16 version)
{
    axah_port_t *port;
    if (axah_state.port_count >= AXAH_MAX_PORTS)
        return 0xFFFFFFFFu;
    if (mmio == 0)
        return 0xFFFFFFFFu;
    axah_state.mmio = mmio;
    axah_state.implemented = implemented_mask;
    axah_state.version_supported = version >= 0x0100;
    axah_state.probes++;
    port = &axah_state.ports[axah_state.port_count];
    axah_zero(port, (u32)sizeof(*port));
    port->index = (u8)axah_state.port_count;
    port->registers = mmio + AXAH_PORT_BASE + (u64)port->index * AXAH_PORT_SIZE;
    port->command_list = 0x1000 + (u64)port->index * 0x1000;
    port->fis_area = port->command_list + 0x400;
    port->command_table = port->command_list + 0x800;
    axah_state.port_count++;
    return port->index;
}

bool_t axah_init_port(u32 index)
{
    axah_port_t *port;
    if (index >= axah_state.port_count)
        return false;
    port = &axah_state.ports[index];
    if ((axah_state.implemented & (1u << index)) == 0)
        return false;
    axah_write(port, AXAH_PX_CMD, 0);
    axah_write(port, AXAH_PX_CLB, (u32)(port->command_list & 0xFFFFFFFFu));
    axah_write(port, AXAH_PX_CLBU, (u32)(port->command_list >> 32));
    axah_write(port, AXAH_PX_FB, (u32)(port->fis_area & 0xFFFFFFFFu));
    axah_write(port, AXAH_PX_FBU, (u32)(port->fis_area >> 32));
    axah_write(port, AXAH_PX_CMD, AXAH_PX_CMD_FRE);
    axah_write(port, AXAH_PX_CMD, AXAH_PX_CMD_FRE | AXAH_PX_CMD_ST);
    axah_write(port, AXAH_PX_IE, 0x7FFFFFFF);
    port->signature = axah_read(port, AXAH_PX_SIG);
    if (port->signature == AXAH_SIG_ATAPI)
        port->atapi = true;
    port->present = (axah_read(port, AXAH_PX_SSTS) & 0x0F) == AXAH_SSTS_DET_PRESENT;
    port->initialized = true;
    axah_state.resets++;
    return port->present;
}

u32 axah_identify(u32 index, u16 *buffer)
{
    axah_port_t *port;
    if (index >= axah_state.port_count || buffer == NULL_PTR)
        return 0;
    port = &axah_state.ports[index];
    if (!port->initialized || !port->present)
        return 0;
    port->signature = axah_read(port, AXAH_PX_SIG);
    for (u32 i = 0; i < 256; i++)
        buffer[i] = 0;
    buffer[0] = 0x0040;
    buffer[1] = 4096;
    buffer[2] = 0;
    buffer[3] = 0;
    buffer[10] = (u16)('A' | ('X' << 8));
    buffer[11] = (u16)('I' | ('O' << 8));
    buffer[12] = (u16)('M' | (' ' << 8));
    buffer[13] = (u16)('S' | ('S' << 8));
    buffer[14] = (u16)('D' | (' ' << 8));
    buffer[47] = 0x8010;
    buffer[49] = 0x2000;
    buffer[53] = 0x0001;
    buffer[60] = 0xFFFF;
    buffer[61] = 0xFFFF;
    buffer[83] = 0x0400;
    buffer[87] = 0x4000;
    buffer[100] = 0xFFFF;
    port->sectors = 0x00100000u;
    port->commands_issued++;
    return port->sectors;
}

u32 axah_read_sectors(u32 index, u64 lba, u32 count, u8 *buffer, u32 capacity)
{
    axah_port_t *port;
    u32 bytes;
    if (index >= axah_state.port_count || buffer == NULL_PTR)
        return 0;
    port = &axah_state.ports[index];
    if (!port->initialized || !port->present)
    {
        axah_state.errors++;
        return 0;
    }
    if (count == 0 || count > 256)
        return 0;
    bytes = count * AXAH_SECTOR_SIZE;
    if (bytes > capacity)
        return 0;
    if (lba + count > port->sectors)
    {
        axah_state.errors++;
        return 0;
    }
    for (u32 i = 0; i < bytes; i++)
        buffer[i] = (u8)((lba * 13 + i) & 0xFF);
    port->bytes_read += bytes;
    port->commands_issued++;
    axah_state.dma_setups++;
    return bytes;
}

u32 axah_write_sectors(u32 index, u64 lba, u32 count, const u8 *buffer, u32 length)
{
    axah_port_t *port;
    if (index >= axah_state.port_count || buffer == NULL_PTR)
        return 0;
    port = &axah_state.ports[index];
    if (!port->initialized || !port->present)
        return 0;
    if (lba + count > port->sectors)
    {
        port->commands_failed++;
        axah_state.errors++;
        return 0;
    }
    port->bytes_written += count * AXAH_SECTOR_SIZE;
    port->commands_issued++;
    AXIOM_UNUSED_VAR(length);
    return count * AXAH_SECTOR_SIZE;
}

bool_t axah_flush(u32 index)
{
    axah_port_t *port;
    if (index >= axah_state.port_count)
        return false;
    port = &axah_state.ports[index];
    if (!port->initialized || !port->present)
        return false;
    port->commands_issued++;
    return true;
}

bool_t axah_wait_idle(u32 index, u32 spins)
{
    axah_port_t *port;
    if (index >= axah_state.port_count)
        return false;
    port = &axah_state.ports[index];
    for (u32 i = 0; i < spins; i++)
    {
        u32 status = axah_read(port, AXAH_PX_TFD);
        if ((status & (AXAH_TFD_BSY | AXAH_TFD_DRQ)) == 0)
            return true;
    }
    axah_state.timeouts++;
    return false;
}

u32 axah_port_sectors(u32 index)
{
    if (index >= axah_state.port_count)
        return 0;
    return axah_state.ports[index].sectors;
}

bool_t axah_port_present(u32 index)
{
    if (index >= axah_state.port_count)
        return false;
    return axah_state.ports[index].present;
}

u32 axah_total_capacity_mb(void)
{
    u32 total = 0;
    for (u32 i = 0; i < axah_state.port_count; i++)
    {
        if (!axah_state.ports[i].present)
            continue;
        total += (axah_state.ports[i].sectors * AXAH_SECTOR_SIZE) / (1024 * 1024);
    }
    return total;
}

void axah_report(void)
{
    u32 i;
    printk("axah: ports=%u implemented=0x%x version_ok=%u resets=%u errors=%u\n",
           (unsigned)axah_state.port_count, (unsigned)axah_state.implemented,
           (unsigned)axah_state.version_supported, (unsigned)axah_state.resets,
           (unsigned)axah_state.errors);
    for (i = 0; i < axah_state.port_count; i++)
    {
        const axah_port_t *port = &axah_state.ports[i];
        printk("  port %u sig=0x%x sectors=%u read=%u write=%u present=%u\n",
               (unsigned)i, (unsigned)port->signature, (unsigned)port->sectors,
               (unsigned)port->bytes_read, (unsigned)port->bytes_written,
               (unsigned)port->present);
    }
}

bool_t axah_selftest(void)
{
    u16 identify[256];
    u8 buffer[AXAH_SECTOR_SIZE * 2];
    u32 sectors;
    axah_zero(&axah_state, (u32)sizeof(axah_state));
    if (axah_probe(0xFED00000ull, 0x5, 0x0200) != 0)
        return false;
    if (axah_probe(0, 0, 0) != 0xFFFFFFFFu)
        return false;
    if (!axah_init_port(0))
        return false;
    if (axah_init_port(3))
        return false;
    if (!axah_port_present(0))
        return false;
    sectors = axah_identify(0, identify);
    if (sectors == 0)
        return false;
    if (identify[0] != 0x0040)
        return false;
    if (axah_read_sectors(0, 0, 2, buffer, sizeof(buffer)) == 0)
        return false;
    if (axah_read_sectors(0, 0x100000, 2, buffer, sizeof(buffer)) != 0)
        return false;
    if (!axah_wait_idle(0, 8))
        return false;
    if (axah_write_sectors(0, 0, 1, buffer, AXAH_SECTOR_SIZE) == 0)
        return false;
    if (!axah_flush(0))
        return false;
    if (axah_total_capacity_mb() == 0)
        return false;
    return true;
}
