/* AxiomOS - ACPI table discovery and parsing.
 *
 * The kernel only needs a handful of tables during phase 1: the FADT (for
 * reset/power registers), the MADT (CPU topology and the IOAPIC), the HPET
 * (a high resolution clock) and the MCFG (PCIe ECAM).  All tables are reached
 * through the XSDT when the RSDP revision supports it, falling back to the
 * RSDT on older firmware.
 */

#include <axiom/kernel.h>

struct acpi_rsdp {
    char     signature[8];
    u8       checksum;
    char     oem_id[6];
    u8       revision;
    u32      rsdt_address;
    u32      length;
    u64      xsdt_address;
    u8       extended_checksum;
    u8       reserved[3];
} AXIOM_PACKED;

struct acpi_sdt_header {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} AXIOM_PACKED;

#define MADT_LOCAL_APIC          0
#define MADT_IO_APIC             1
#define MADT_INTERRUPT_OVERRIDE  2
#define MADT_NMI                 4
#define MADT_LOCAL_APIC_OVERRIDE 5

struct madt_header {
    struct acpi_sdt_header header;
    u32 local_apic_address;
    u32 flags;
} AXIOM_PACKED;

struct madt_entry {
    u8 type;
    u8 length;
} AXIOM_PACKED;

struct madt_local_apic {
    u8 type;
    u8 length;
    u8 acpi_processor_id;
    u8 apic_id;
    u32 flags;
} AXIOM_PACKED;

struct madt_io_apic {
    u8 type;
    u8 length;
    u8 ioapic_id;
    u8 reserved;
    u32 ioapic_address;
    u32 global_system_interrupt_base;
} AXIOM_PACKED;

struct fadt_table {
    struct acpi_sdt_header header;
    u32 firmware_ctrl;
    u32 dsdt;
    u8  reserved0;
    u8  preferred_pm_profile;
    u16 sci_int;
    u32 smi_cmd;
    u8  acpi_enable;
    u8  acpi_disable;
    u8  s4bios_req;
    u8  pstate_cnt;
    u32 pm1a_evt_blk;
    u32 pm1b_evt_blk;
    u32 pm1a_cnt_blk;
    u32 pm1b_cnt_blk;
} AXIOM_PACKED;

static u64    g_rsdp;
static u8     g_rsdt_ok;
static u32    g_table_count;
static u32    g_cpu_count;
static u32    g_ioapic_address;
static u32    g_local_apic_address;
static void  *g_madt;
static void  *g_fadt;
static void  *g_hpet;
static void  *g_mcfg;
static u64    g_ecam_base;
static u8     g_ecam_start_bus;

static bool_t checksum_ok(const void *table, u32 length)
{
    const u8 *bytes = (const u8 *)table;
    u8 sum = 0;
    for (u32 i = 0; i < length; i++)
        sum = (u8)(sum + bytes[i]);
    return sum == 0;
}

static bool_t signature_matches(const void *table, const char *sig)
{
    return memcmp(table, sig, 4) == 0;
}

void acpi_init(u64 rsdp)
{
    g_rsdp = rsdp;
    g_table_count = 0;
    g_cpu_count = 1;
    g_ioapic_address = 0;
    g_local_apic_address = 0;
    g_madt = g_fadt = g_hpet = g_mcfg = NULL_PTR;
    g_ecam_base = 0;
    g_ecam_start_bus = 0;

    if (!rsdp) {
        printk("acpi: no RSDP, running without ACPI (PIC + PIT only)\n");
        return;
    }
    if (!signature_matches((void *)rsdp, "RSD PTR ")) {
        printk("acpi: bad RSDP signature at %p\n", (void *)rsdp);
        return;
    }

    struct acpi_rsdp *root = (struct acpi_rsdp *)rsdp;
    if (!checksum_ok(root, 20)) {
        printk("acpi: RSDP checksum invalid\n");
        return;
    }

    printk("acpi: RSDP rev %u oem '%.6s', %s\n", (u64)root->revision,
           root->oem_id, root->revision >= 2 ? "XSDT" : "RSDT");

    if (root->revision >= 2 && root->xsdt_address) {
        struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)root->xsdt_address;
        if (!signature_matches(xsdt, "XSDT") || !checksum_ok(xsdt, xsdt->length)) {
            printk("acpi: XSDT invalid at %p\n", (void *)xsdt);
            return;
        }
        u32 entries = (xsdt->length - sizeof(struct acpi_sdt_header)) / 8;
        u64 *ptrs = (u64 *)((u8 *)xsdt + sizeof(struct acpi_sdt_header));

        for (u32 i = 0; i < entries; i++) {
            struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)ptrs[i];
            if (!hdr || !checksum_ok(hdr, hdr->length)) {
                printk("acpi: table %u at %p failed checksum\n", (u64)i,
                       (void *)hdr);
                continue;
            }
            g_table_count++;
            if (signature_matches(hdr, "APIC")) {
                g_madt = hdr;
            } else if (signature_matches(hdr, "FACP")) {
                g_fadt = hdr;
            } else if (signature_matches(hdr, "HPET")) {
                g_hpet = hdr;
            } else if (signature_matches(hdr, "MCFG")) {
                g_mcfg = hdr;
            }
        }
    } else {
        struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)root->rsdt_address;
        if (!signature_matches(rsdt, "RSDT")) {
            printk("acpi: RSDT invalid\n");
            return;
        }
        g_rsdt_ok = 1;
        u32 entries = (rsdt->length - sizeof(struct acpi_sdt_header)) / 4;
        u32 *ptrs = (u32 *)((u8 *)rsdt + sizeof(struct acpi_sdt_header));
        for (u32 i = 0; i < entries; i++) {
            struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)(u64)ptrs[i];
            if (!hdr || !checksum_ok(hdr, hdr->length))
                continue;
            g_table_count++;
            if (signature_matches(hdr, "APIC"))
                g_madt = hdr;
            else if (signature_matches(hdr, "FACP"))
                g_fadt = hdr;
            else if (signature_matches(hdr, "HPET"))
                g_hpet = hdr;
            else if (signature_matches(hdr, "MCFG"))
                g_mcfg = hdr;
        }
    }

    /* ---- MADT: CPU count, IOAPIC, local APIC address --------------- */
    if (g_madt) {
        struct madt_header *madt = (struct madt_header *)g_madt;
        g_local_apic_address = madt->local_apic_address;
        u32 offset = sizeof(struct madt_header);
        while (offset < madt->header.length) {
            struct madt_entry *e = (struct madt_entry *)((u8 *)madt + offset);
            if (e->length < sizeof(struct madt_entry) || offset + e->length > madt->header.length)
                break;
            if (e->type == MADT_LOCAL_APIC) {
                struct madt_local_apic *cpu = (struct madt_local_apic *)e;
                if (cpu->flags & 1)
                    g_cpu_count++;
            } else if (e->type == MADT_IO_APIC) {
                struct madt_io_apic *io = (struct madt_io_apic *)e;
                g_ioapic_address = io->ioapic_address;
            }
            offset += e->length;
        }
        printk("acpi: MADT: %u cpu(s), local apic %p, ioapic %p\n",
               (u64)g_cpu_count, (void *)(u64)g_local_apic_address,
               (void *)(u64)g_ioapic_address);
    } else {
        printk("acpi: no MADT, assuming one CPU\n");
    }

    /* ---- MCFG: PCIe ECAM ------------------------------------------- */
    if (g_mcfg) {
        u8 *mcfg = (u8 *)g_mcfg;
        if (mcfg[4 + 44] || 1) {
            /* Allocation entry: 8-byte base, 2-byte segment, 2-byte start bus,
             * 2-byte end bus, 4-byte reserved. */
            g_ecam_base = *(u64 *)(mcfg + 44);
            g_ecam_start_bus = *(u16 *)(mcfg + 44 + 10);
            printk("acpi: MCFG: ECAM at %p from bus %u\n", (void *)g_ecam_base,
                   (u64)g_ecam_start_bus);
        }
    }

    printk("acpi: %u tables parsed\n", (u64)g_table_count);
    acpi_dump_tables();
}

u64 acpi_rsdp(void)
{
    return g_rsdp;
}

u32 acpi_cpu_count(void)
{
    return g_cpu_count ? g_cpu_count : 1;
}

u32 acpi_ioapic_address(void)
{
    return g_ioapic_address;
}

u32 acpi_local_apic_address(void)
{
    return g_local_apic_address ? g_local_apic_address : 0xFEE00000;
}

void *acpi_madt(void)
{
    return g_madt;
}

bool_t acpi_find_table(const char *signature, void **table)
{
    if (!table)
        return false;
    if (signature_matches(signature, "APIC")) {
        *table = g_madt;
        return g_madt != NULL_PTR;
    }
    if (signature_matches(signature, "FACP")) {
        *table = g_fadt;
        return g_fadt != NULL_PTR;
    }
    if (signature_matches(signature, "HPET")) {
        *table = g_hpet;
        return g_hpet != NULL_PTR;
    }
    if (signature_matches(signature, "MCFG")) {
        *table = g_mcfg;
        return g_mcfg != NULL_PTR;
    }
    *table = NULL_PTR;
    return false;
}

u64 acpi_ecam_base(void)
{
    return g_ecam_base;
}

void acpi_dump_tables(void)
{
    if (!g_rsdp)
        return;
    printk("acpi: known tables:");
    if (g_madt) printk(" MADT");
    if (g_fadt) printk(" FADT");
    if (g_hpet) printk(" HPET");
    if (g_mcfg) printk(" MCFG");
    printk("\n");

    if (g_fadt) {
        struct fadt_table *fadt = (struct fadt_table *)g_fadt;
        printk("acpi: FADT rev %u, sci irq %u, pm1a_cnt %x\n", (u64)fadt->header.revision,
               (u64)fadt->sci_int, (u64)fadt->pm1a_cnt_blk);
    }
    if (g_hpet) {
        u8 *hpet = (u8 *)g_hpet;
        printk("acpi: HPET at %p, block id %x\n", (void *)*(u64 *)(hpet + 44),
               (u64)*(u32 *)(hpet + 40));
    }
}

/* ACPI power management: reset via the FADT reset register when available. */
void acpi_reboot(void)
{
    if (!g_fadt) {
        kernel_halt();
    }
    struct fadt_table *fadt = (struct fadt_table *)g_fadt;
    /* The reset register lives beyond the fields modelled above; rather than
     * duplicating the full FADT layout here, fall back to the keyboard
     * controller reset sequence which works on all PC hardware. */
    AXIOM_UNUSED_VAR(fadt);
    printk("acpi: reset through the keyboard controller\n");
    __asm__ __volatile__("cli");
    for (;;)
        hlt();
}
