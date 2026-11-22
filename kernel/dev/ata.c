/* AxiomOS - legacy ATA PIO driver (fallback for CHS/LBA28 controllers).
 *
 * Used when no AHCI controller is present.  Only the primary channel master
 * drive is supported; that is enough to read the ESP on machines where the
 * firmware exposes IDE rather than AHCI (QEMU's default q35 configuration
 * before the AHCI option ROM takes over, for example).
 */

#include <axiom/kernel.h>

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6

#define ATA_REG_DATA      0
#define ATA_REG_ERROR     1
#define ATA_REG_FEATURES  1
#define ATA_REG_SECCOUNT  2
#define ATA_REG_LBA0      3
#define ATA_REG_LBA1      4
#define ATA_REG_LBA2      5
#define ATA_REG_DRIVE     6
#define ATA_REG_STATUS    7
#define ATA_REG_COMMAND   7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO      0x20
#define ATA_CMD_READ_PIO_EXT  0x24
#define ATA_CMD_WRITE_PIO     0x30
#define ATA_CMD_WRITE_PIO_EXT 0x34
#define ATA_CMD_IDENTIFY      0xEC

extern u8  io_inb(u16 port);
extern void io_outb(u16 port, u8 value);
extern u16 io_inw(u16 port);
extern void io_outw(u16 port, u16 value);
extern void io_wait(void);

static bool_t g_ata_ready;
static u64 g_ata_sectors;
static bool_t g_ata_lba48;

static void ata_delay(u32 count)
{
    for (u32 i = 0; i < count; i++)
        io_wait();
}

static bool_t ata_wait_not_busy(u32 timeout_ms)
{
    u64 deadline = timer_ms() + timeout_ms;
    while (timer_ms() < deadline) {
        u8 status = io_inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY))
            return true;
        cpu_relax();
    }
    return false;
}

static bool_t ata_wait_drq(u32 timeout_ms)
{
    u64 deadline = timer_ms() + timeout_ms;
    while (timer_ms() < deadline) {
        u8 status = io_inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)
            return false;
        if (status & ATA_SR_DRQ)
            return true;
        cpu_relax();
    }
    return false;
}

static void ata_select_lba(u64 lba, u16 count, bool_t lba48)
{
    if (lba48) {
        io_outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, 0x40);
        ata_delay(4);
        io_outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, 0);
        io_outb(ATA_PRIMARY_IO + ATA_REG_LBA0, (u8)(lba >> 24));
        io_outb(ATA_PRIMARY_IO + ATA_REG_LBA1, (u8)(lba >> 32));
        io_outb(ATA_PRIMARY_IO + ATA_REG_LBA2, (u8)(lba >> 40));
    }
    io_outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay(4);
    io_outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, (u8)count);
    io_outb(ATA_PRIMARY_IO + ATA_REG_LBA0, (u8)lba);
    io_outb(ATA_PRIMARY_IO + ATA_REG_LBA1, (u8)(lba >> 8));
    io_outb(ATA_PRIMARY_IO + ATA_REG_LBA2, (u8)(lba >> 16));
}

void ata_init(void)
{
    u8 status, cl;
    u16 identify[256];

    g_ata_ready = false;
    g_ata_lba48 = false;

    /* Soft reset the channel. */
    io_outb(ATA_PRIMARY_CTRL, 0x04);
    ata_delay(20);
    io_outb(ATA_PRIMARY_CTRL, 0x00);
    ata_delay(20);

    status = io_inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (status == 0) {
        printk("ata: no controller at 0x1F0\n");
        return;
    }

    io_outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, 0xA0);
    ata_delay(20);
    io_outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, 0);
    io_outb(ATA_PRIMARY_IO + ATA_REG_LBA0, 0);
    io_outb(ATA_PRIMARY_IO + ATA_REG_LBA1, 0);
    io_outb(ATA_PRIMARY_IO + ATA_REG_LBA2, 0);
    io_outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay(20);

    status = io_inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (!status) {
        printk("ata: no drive on the primary master\n");
        return;
    }

    if (!ata_wait_drq(2000)) {
        printk("ata: IDENTIFY timed out\n");
        return;
    }

    cl = io_inb(ATA_PRIMARY_IO + ATA_REG_LBA1);
    if (cl == 0x14 || cl == 0xEB) {
        printk("ata: ATAPI device, not a disk\n");
        return;
    }

    for (u32 i = 0; i < 256; i++)
        identify[i] = io_inw(ATA_PRIMARY_IO + ATA_REG_DATA);

    if (identify[83] & (1 << 10)) {
        g_ata_lba48 = true;
        g_ata_sectors = (u64)identify[100] | ((u64)identify[101] << 16) |
                        ((u64)identify[102] << 32) | ((u64)identify[103] << 48);
    } else {
        g_ata_sectors = (u64)identify[60] | ((u64)identify[61] << 16);
    }

    g_ata_ready = true;
    printk("ata: primary master, %u sectors (%u MiB), lba48 %Zb\n",
           g_ata_sectors, g_ata_sectors / 2048, g_ata_lba48);
}

bool_t ata_read(u64 lba, u8 count, void *buffer)
{
    u16 *words = (u16 *)buffer;
    if (!g_ata_ready || !count)
        return false;

    if (!ata_wait_not_busy(5000))
        return false;

    ata_select_lba(lba, count, g_ata_lba48);
    io_outb(ATA_PRIMARY_IO + ATA_REG_COMMAND,
            (u8)(g_ata_lba48 ? ATA_CMD_READ_PIO_EXT : ATA_CMD_READ_PIO));
    ata_delay(4);

    for (u32 sector = 0; sector < count; sector++) {
        if (!ata_wait_drq(5000))
            return false;
        for (u32 i = 0; i < 256; i++)
            *words++ = io_inw(ATA_PRIMARY_IO + ATA_REG_DATA);
        ata_delay(4);
    }
    return true;
}

bool_t ata_write(u64 lba, u8 count, const void *buffer)
{
    const u16 *words = (const u16 *)buffer;
    if (!g_ata_ready || !count)
        return false;

    if (!ata_wait_not_busy(5000))
        return false;

    ata_select_lba(lba, count, g_ata_lba48);
    io_outb(ATA_PRIMARY_IO + ATA_REG_COMMAND,
            (u8)(g_ata_lba48 ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_WRITE_PIO));
    ata_delay(4);

    for (u32 sector = 0; sector < count; sector++) {
        if (!ata_wait_drq(5000))
            return false;
        for (u32 i = 0; i < 256; i++)
            io_outw(ATA_PRIMARY_IO + ATA_REG_DATA, *words++);
        io_outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, 0xE7);   /* flush cache */
        ata_delay(4);
        if (!ata_wait_not_busy(5000))
            return false;
    }
    return true;
}

bool_t ata_present(void)
{
    return g_ata_ready;
}

u64 ata_sectors(void)
{
    return g_ata_sectors;
}
