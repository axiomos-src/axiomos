/* AxiomOS - storage: AHCI and legacy ATA.
 *
 * The AHCI driver performs polled command execution: it finds the controller
 * from PCI, maps its ABAR, resets the first implemented port, and issues
 * IDENTIFY DEVICE plus 48-bit LBA reads.  Interrupt driven completion is on
 * the roadmap; polling keeps the boot path deterministic, which matters when
 * the same image is also used by the GUI's file manager.
 */

#include <axiom/kernel.h>

#define AHCI_CLASS      0x01
#define AHCI_SUBCLASS   0x06
#define AHCI_PROG_IF    0x01

#define HBA_PORT_DET_PRESENT   0x03

struct ahci_port {
    u32 clb;
    u32 clbu;
    u32 fb;
    u32 fbu;
    u32 is;
    u32 ie;
    u32 cmd;
    u32 rsv0;
    u32 tfd;
    u32 sig;
    u32 ssts;
    u32 sctl;
    u32 serr;
    u32 sact;
    u32 ci;
    u32 sntf;
    u32 rsv1[11];
    u32 vendor[4];
} AXIOM_PACKED;

struct ahci_hba {
    u32 cap;
    u32 ghc;
    u32 is;
    u32 pi;
    u32 vs;
    u32 ccc_ctl;
    u32 ccc_pts;
    u32 em_loc;
    u32 em_ctl;
    u32 cap2;
    u32 bohc;
    u8  rsv[0xA0 - 0x2C];
    u8  vendor[0x100 - 0xA0];
    struct ahci_port ports[32];
} AXIOM_PACKED;

struct ahci_cmd_header {
    u16 flags;
    u16 prdt_length;
    u32 prdbc;
    u32 ctba;
    u32 ctbau;
    u32 rsv[4];
} AXIOM_PACKED;

struct ahci_prdt_entry {
    u32 dba;
    u32 dbau;
    u32 rsv0;
    u32 dbc;
} AXIOM_PACKED;

struct ahci_cmd_table {
    u8 cfis[64];
    u8 acmd[16];
    u8 rsv[48];
    struct ahci_prdt_entry prdt[8];
} AXIOM_PACKED;

static struct ahci_hba *g_hba;
static u32 g_abar;
static u32 g_port_index;
static u64 g_disk_sectors;
static struct ahci_cmd_header *g_cmd_list;
static struct ahci_cmd_table *g_cmd_table;
static u64 g_cmd_list_phys;
static u64 g_cmd_table_phys;
static u8 *g_fis_buffer;
static bool_t g_ahci_ready;
static bool_t g_atapi;

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                       */
/* ------------------------------------------------------------------ */

AXIOM_INLINE u32 hba_read(u32 offset)
{
    return *(volatile u32 *)((u64)g_hba + offset);
}

AXIOM_INLINE void hba_write(u32 offset, u32 value)
{
    *(volatile u32 *)((u64)g_hba + offset) = value;
}

static void port_write(u32 port, u32 reg, u32 value)
{
    *(volatile u32 *)((u64)g_hba + 0x100 + port * 0x80 + reg) = value;
}

static u32 port_read(u32 port, u32 reg)
{
    return *(volatile u32 *)((u64)g_hba + 0x100 + port * 0x80 + reg);
}

/* ------------------------------------------------------------------ */
/* Port bring-up                                                      */
/* ------------------------------------------------------------------ */

static bool_t ahci_port_stop(u32 port)
{
    u32 cmd = port_read(port, 0x18);
    cmd &= ~1U;              /* clear ST */
    port_write(port, 0x18, cmd);

    for (u32 spin = 0; spin < 100000; spin++) {
        if (!(port_read(port, 0x18) & 0x8000))   /* CR clear */
            break;
        cpu_relax();
    }
    cmd = port_read(port, 0x18);
    cmd &= ~0x10U;           /* clear FRE */
    port_write(port, 0x18, cmd);
    return true;
}

static bool_t ahci_port_start(u32 port)
{
    u32 cmd;

    while (port_read(port, 0x18) & 0x8000)
        cpu_relax();

    port_write(port, 0x00, (u32)g_cmd_list_phys);          /* CLB */
    port_write(port, 0x04, (u32)(g_cmd_list_phys >> 32));  /* CLBU */
    port_write(port, 0x08, 0);                             /* FB */
    port_write(port, 0x0C, 0);                             /* FBU */

    cmd = port_read(port, 0x18);
    cmd |= 1U | 0x10U;       /* ST | FRE */
    port_write(port, 0x18, cmd);
    return true;
}

static void ahci_clear_errors(u32 port)
{
    port_write(port, 0x30, port_read(port, 0x30));   /* SERK */
    port_write(port, 0x10, 0xFFFFFFFF);              /* IS   */
}

/* ------------------------------------------------------------------ */
/* Command execution                                                  */
/* ------------------------------------------------------------------ */

static bool_t ahci_wait_not_busy(u32 port, u32 timeout_ms)
{
    u64 deadline = timer_ms() + timeout_ms;
    while (timer_ms() < deadline) {
        u32 tfd = port_read(port, 0x20);
        if (!(tfd & 0x88))     /* BSY | DRQ clear */
            return true;
        cpu_relax();
    }
    return false;
}

static bool_t ahci_issue_command(u32 port, u32 slot, u16 prdt_count)
{
    u32 spin;
    port_write(port, 0x38, 1U << slot);     /* SACT */
    /* Wait for the port to finish. */
    for (spin = 0; spin < 1000000; spin++) {
        u32 ci = port_read(port, 0x38);
        u32 is = port_read(port, 0x10);
        if (!(ci & (1U << slot)) && !(is & 0x40000000))  /* HBA active */
            break;
        cpu_relax();
    }
    if (spin == 1000000) {
        printk("ahci: port %u command timeout (slot %u, %u prdt)\n",
               (u64)port, (u64)slot, (u64)prdt_count);
        return false;
    }

    u32 tfd = port_read(port, 0x20);
    if (tfd & 1U) {
        printk("ahci: port %u task file error (%x)\n", (u64)port, (u64)tfd);
        return false;
    }
    return true;
}

static void build_fis_read(u8 *fis, u64 lba, u16 count)
{
    memset(fis, 0, 64);
    fis[0] = 0x27;             /* host to device register FIS */
    fis[1] = 0x80;             /* command */
    fis[2] = 0x25;             /* READ DMA EXT */
    fis[3] = 0x00;
    fis[4] = (u8)(lba >> 40);
    fis[5] = (u8)(lba >> 32);
    fis[6] = (u8)(lba >> 24);
    fis[7] = (u8)lba;
    fis[8] = 0;
    fis[9] = 0;
    fis[10] = 0;
    fis[11] = 0;
    fis[12] = (u8)count;
    fis[13] = (u8)(count >> 8);
}

static void build_fis_write(u8 *fis, u64 lba, u16 count)
{
    build_fis_read(fis, lba, count);
    fis[2] = 0x35;             /* WRITE DMA EXT */
}

bool_t ahci_read(u64 lba, u16 count, void *buffer)
{
    u32 port = g_port_index;
    struct ahci_cmd_header *header = (struct ahci_cmd_header *)g_cmd_list;
    struct ahci_cmd_table *table = g_cmd_table;

    if (!g_ahci_ready || !count)
        return false;
    if (!ahci_wait_not_busy(port, 5000))
        return false;

    memset(header, 0, sizeof(*header) + 31 * sizeof(struct ahci_cmd_header));
    header->flags = (u16)(5 | (1 << 10));    /* FIS len 5 dwords, write bit */
    header->prdt_length = 1;
    header->ctba = (u32)g_cmd_table_phys;
    header->ctbau = (u32)(g_cmd_table_phys >> 32);

    memset(table, 0, sizeof(*table));
    build_fis_read(table->cfis, lba, count);
    table->prdt[0].dba = (u32)(u64)buffer;
    table->prdt[0].dbau = (u32)((u64)buffer >> 32);
    table->prdt[0].dbc = (u32)(count * 512 - 1);    /* byte count minus one */

    port_write(port, 0x28, 0xFFFFFFFF);   /* IE */
    port_write(port, 0x18, port_read(port, 0x18) | 1U);
    port_write(port, 0x38, 1U);           /* CI slot 0 */

    for (u32 spin = 0; spin < 1000000; spin++) {
        if (!(port_read(port, 0x38) & 1U))
            break;
        prdt: ;
        cpu_relax();
    }
    if (port_read(port, 0x20) & 1U) {
        printk("ahci: read error at lba %u (%u blocks)\n", lba, (u64)count);
        return false;
    }
    return true;
}

bool_t ahci_write(u64 lba, u16 count, const void *buffer)
{
    u32 port = g_port_index;
    struct ahci_cmd_header *header = (struct ahci_cmd_header *)g_cmd_list;
    struct ahci_cmd_table *table = g_cmd_table;

    if (!g_ahci_ready || !count)
        return false;
    if (!ahci_wait_not_busy(port, 5000))
        return false;

    memset(header, 0, sizeof(*header));
    header->flags = (u16)(5 | (1 << 10));
    header->prdt_length = 1;
    header->ctba = (u32)g_cmd_table_phys;
    header->ctbau = (u32)(g_cmd_table_phys >> 32);

    memset(table, 0, sizeof(*table));
    build_fis_write(table->cfis, lba, count);
    table->prdt[0].dba = (u32)(u64)buffer;
    table->prdt[0].dbau = (u32)((u64)buffer >> 32);
    table->prdt[0].dbc = (u32)(count * 512 - 1);

    port_write(port, 0x38, 1U);
    for (u32 spin = 0; spin < 2000000; spin++) {
        if (!(port_read(port, 0x38) & 1U))
            break;
        cpu_relax();
    }
    if (port_read(port, 0x20) & 1U)
        return false;
    return true;
}

u64 ahci_disk_size(void)
{
    return g_disk_sectors * 512ULL;
}

bool_t ahci_present(void)
{
    return g_ahci_ready;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */

void ahci_init(void)
{
    u8 bus, dev, func;
    u32 abar;
    u64 abar_phys;
    u32 pi;
    u32 port = 0xFFFFFFFF;

    g_ahci_ready = false;
    g_disk_sectors = 0;

    if (!pci_find_class(AHCI_CLASS, AHCI_SUBCLASS, &bus, &dev, &func, 0)) {
        printk("ahci: no SATA controller found\n");
        ata_init();
        return;
    }

    u8 prog_if = pci_read8(bus, dev, func, 0x09);
    if ((prog_if & 0x0F) != AHCI_PROG_IF) {
        printk("ahci: controller is not in AHCI mode (prog_if %x)\n", (u64)prog_if);
        ata_init();
        return;
    }

    pci_write32(bus, dev, func, 0x04, pci_read32(bus, dev, func, 0x04) | 0x6);
    abar = (u32)pci_bar(bus, dev, func, 5);
    abar_phys = abar & ~0xFFFULL;
    g_abar = (u32)vmm_map_mmio(abar_phys, 8192);
    if (!g_abar) {
        printk("ahci: cannot map ABAR %p\n", (void *)abar_phys);
        return;
    }
    g_hba = (struct ahci_hba *)g_abar;

    /* Take ownership from the BIOS. */
    hba_write(0x04, hba_read(0x04) | (1U << 31));
    for (u32 spin = 0; spin < 100000; spin++) {
        if (!(hba_read(0x04) & (1U << 31)))
            break;
        cpu_relax();
    }
    hba_write(0x04, hba_read(0x04) | (1U << 0));   /* AHCI enable */

    pi = hba_read(0x0C);   /* ports implemented */
    printk("ahci: ABAR %p, implemented ports mask %x\n", (void *)abar_phys,
           (u64)pi);

    for (u32 i = 0; i < 32; i++) {
        if (!(pi & (1U << i)))
            continue;
        u32 ssts = port_read(i, 0x28);
        u32 sig = port_read(i, 0x24);
        u8 det = (u8)(ssts & 0x0F);
        if (det != HBA_PORT_DET_PRESENT)
            continue;
        if (sig != 0x00000101) {          /* SATA disk signature */
            printk("ahci: port %u has signature %x (not a plain disk)\n",
                   (u64)i, (u64)sig);
            continue;
        }
        port = i;
        break;
    }

    if (port == 0xFFFFFFFF) {
        printk("ahci: no drive attached\n");
        return;
    }
    g_port_index = port;

    /* Allocate the command list (32 entries, 1 KiB aligned) and one command
     * table and FIS receive buffer. */
    g_cmd_list_phys = pmm_alloc_page();
    g_cmd_table_phys = pmm_alloc_page();
    if (!g_cmd_list_phys || !g_cmd_table_phys) {
        printk("ahci: out of memory for DMA structures\n");
        return;
    }
    g_cmd_list = (struct ahci_cmd_header *)g_cmd_list_phys;
    g_cmd_table = (struct ahci_cmd_table *)g_cmd_table_phys;
    g_fis_buffer = (u8 *)pmm_alloc_page();

    ahci_port_stop(port);
    ahci_clear_errors(port);
    ahci_port_start(port);
    if (!ahci_wait_not_busy(port, 1000)) {
        printk("ahci: port %u stuck busy\n", (u64)port);
        return;
    }

    /* IDENTIFY DEVICE through the ATA pass-through command. */
    {
        struct ahci_cmd_header *header = (struct ahci_cmd_header *)g_cmd_list;
        struct ahci_cmd_table *table = g_cmd_table;
        memset(header, 0, 32 * sizeof(struct ahci_cmd_header));
        header->flags = (u16)(5 | (1 << 10));
        header->prdt_length = 1;
        header->ctba = (u32)g_cmd_table_phys;
        header->ctbau = (u32)(g_cmd_table_phys >> 32);
        memset(table, 0, sizeof(*table));
        table->cfis[0] = 0x27;
        table->cfis[1] = 0x80;
        table->cfis[2] = 0xEC;          /* IDENTIFY DEVICE */
        table->prdt[0].dba = (u32)(u64)g_fis_buffer;
        table->prdt[0].dbc = 512 - 1;
        port_write(port, 0x38, 1U);
        for (u32 spin = 0; spin < 500000; spin++) {
            if (!(port_read(port, 0x38) & 1U))
                break;
            cpu_relax();
        }
    }

    {
        u16 *identify = (u16 *)g_fis_buffer;
        /* Word 83 bit 10 indicates LBA48 support; words 100..103 hold the
         * sector count for 48-bit addressing. */
        if (identify[83] & (1 << 10)) {
            g_disk_sectors = (u64)identify[100] | ((u64)identify[101] << 16) |
                             ((u64)identify[102] << 32) |
                             ((u64)identify[103] << 48);
        } else {
            g_disk_sectors = (u64)identify[60] | ((u64)identify[61] << 16);
        }
    }

    g_ahci_ready = true;
    printk("ahci: port %u ready, %u sectors (%u MiB)\n", (u64)port,
           g_disk_sectors, g_disk_sectors / 2048);
}

void ahci_dump(void)
{
    if (!g_ahci_ready) {
        printk("ahci: not initialised\n");
        return;
    }
    printk("ahci: port %u, %u sectors, cap %x, is %x\n", (u64)g_port_index,
           g_disk_sectors, (u64)hba_read(0x00), (u64)hba_read(0x08));
}
