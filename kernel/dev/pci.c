/* AxiomOS - PCI / PCIe configuration space access and enumeration.
 *
 * ECAM (memory mapped configuration, described by the ACPI MCFG table) is used
 * when available; otherwise the legacy 0xCF8/0xCFC I/O ports are used.  The
 * driver keeps a small registry of discovered devices so the GUI's device
 * manager and the storage layer can find their controllers without walking
 * the bus again.
 */

#include <axiom/kernel.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_MAX_DEVICES 64

struct pci_device_info {
    u8  bus;
    u8  dev;
    u8  func;
    u8  class_code;
    u8  subclass;
    u8  prog_if;
    u8  revision;
    u16 vendor_id;
    u16 device_id;
    u8  header_type;
    u8  irq_line;
    u64 bar[6];
    u64 bar_size[6];
    char name[48];
};

static struct pci_device_info g_devices[PCI_MAX_DEVICES];
static u32 g_device_count;
static u64 g_ecam_base;
static bool_t g_ecam_available;

extern u32 io_inl(u16 port);
extern void io_outl(u16 port, u32 value);

/* ------------------------------------------------------------------ */
/* Configuration space primitives                                     */
/* ------------------------------------------------------------------ */

static u32 ecam_address(u8 bus, u8 dev, u8 func, u8 off)
{
    return (u32)(g_ecam_base + ((u64)bus << 20) + ((u64)dev << 15) +
                 ((u64)func << 12) + off);
}

u32 pci_read32(u8 bus, u8 dev, u8 func, u8 off)
{
    if (g_ecam_available) {
        u32 addr = ecam_address(bus, dev, func, off & 0xFC);
        return *(volatile u32 *)(u64)addr;
    }
    u32 address = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
                  ((u32)func << 8) | (off & 0xFC);
    io_outl(PCI_CONFIG_ADDRESS, address);
    return io_inl(PCI_CONFIG_DATA);
}

u16 pci_read16(u8 bus, u8 dev, u8 func, u8 off)
{
    u32 value = pci_read32(bus, dev, func, off & 0xFC);
    return (u16)((value >> ((off & 2) * 8)) & 0xFFFF);
}

u8 pci_read8(u8 bus, u8 dev, u8 func, u8 off)
{
    u32 value = pci_read32(bus, dev, func, off & 0xFC);
    return (u8)((value >> ((off & 3) * 8)) & 0xFF);
}

void pci_write32(u8 bus, u8 dev, u8 func, u8 off, u32 value)
{
    if (g_ecam_available) {
        u32 addr = ecam_address(bus, dev, func, off & 0xFC);
        *(volatile u32 *)(u64)addr = value;
        return;
    }
    u32 address = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
                  ((u32)func << 8) | (off & 0xFC);
    io_outl(PCI_CONFIG_ADDRESS, address);
    io_outl(PCI_CONFIG_DATA, value);
}

u64 pci_bar(u8 bus, u8 dev, u8 func, u8 bar)
{
    u8 off = (u8)(0x10 + bar * 4);
    u32 value = pci_read32(bus, dev, func, off);
    u64 address;
    if (value & 1)
        return (u64)(value & ~0x3U);        /* I/O space BAR */
    address = (u64)(value & ~0xFULL);
    if ((value & 0x6) == 0x4) {
        /* 64-bit memory BAR: combine with the next BAR slot. */
        u32 high = pci_read32(bus, dev, func, (u8)(off + 4));
        address |= (u64)high << 32;
    }
    return address;
}

static u64 pci_bar_size(u8 bus, u8 dev, u8 func, u8 bar)
{
    u8 off = (u8)(0x10 + bar * 4);
    u32 original = pci_read32(bus, dev, func, off);
    if (original & 1) {
        /* I/O BAR: use 16-bit probing. */
        pci_write32(bus, dev, func, off, 0xFFFFFFFF);
        u32 probed = pci_read32(bus, dev, func, off);
        pci_write32(bus, dev, func, off, original);
        u32 mask = probed & 0xFFFC;
        return mask ? (~mask + 1) & 0xFFFF : 0;
    }
    pci_write32(bus, dev, func, off, 0xFFFFFFFF);
    u32 probed = pci_read32(bus, dev, func, off);
    pci_write32(bus, dev, func, off, original);
    u32 mask = probed & ~0xF;
    if (!mask)
        return 0;
    return (u64)(~mask + 1);
}

u8 pci_find_capability(u8 bus, u8 dev, u8 func, u8 cap_id)
{
    u8 status = (u8)(pci_read16(bus, dev, func, 0x06) >> 8);
    if (!(status & 0x10))
        return 0;
    u8 pointer = pci_read8(bus, dev, func, 0x34) & 0xFC;
    for (u32 guard = 0; guard < 48 && pointer; guard++) {
        u8 id = pci_read8(bus, dev, func, pointer);
        if (id == cap_id)
            return pointer;
        pointer = pci_read8(bus, dev, func, (u8)(pointer + 1)) & 0xFC;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Naming                                                             */
/* ------------------------------------------------------------------ */

static const char *class_name(u8 class_code, u8 subclass)
{
    switch (class_code) {
    case 0x00: return "unclassified";
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller (AHCI)";
        case 0x08: return "NVMe controller";
        default:   return "mass storage controller";
        }
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia device";
    case 0x06: return "bridge";
    case 0x07: return "serial bus controller";
    case 0x08: return "system peripheral";
    case 0x09: return "input device controller";
    case 0x0C: return "serial bus controller";
    case 0x0D: return "wireless controller";
    default:   return "unknown device";
    }
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                        */
/* ------------------------------------------------------------------ */

static void pci_add_device(u8 bus, u8 dev, u8 func,
                           const struct pci_device_info *probe)
{
    if (g_device_count >= PCI_MAX_DEVICES)
        return;
    g_devices[g_device_count] = *probe;
    g_devices[g_device_count].bus = bus;
    g_devices[g_device_count].dev = dev;
    g_devices[g_device_count].func = func;
    g_device_count++;
}

static void pci_probe_function(u8 bus, u8 dev, u8 func)
{
    struct pci_device_info info;
    u32 header;
    u16 vendor;

    memset(&info, 0, sizeof(info));
    vendor = (u16)pci_read32(bus, dev, func, 0x00);
    if (vendor == 0xFFFF || vendor == 0)
        return;

    info.vendor_id = vendor;
    info.device_id = (u16)(pci_read32(bus, dev, func, 0x00) >> 16);
    info.revision = pci_read8(bus, dev, func, 0x08);
    info.prog_if = pci_read8(bus, dev, func, 0x09);
    info.subclass = pci_read8(bus, dev, func, 0x0A);
    info.class_code = pci_read8(bus, dev, func, 0x0B);
    header = pci_read8(bus, dev, func, 0x0E);
    info.header_type = (u8)header;
    info.irq_line = pci_read8(bus, dev, func, 0x3C);

    /* Base address registers (only for endpoint header types). */
    if ((header & 0x7F) == 0x00) {
        for (u8 b = 0; b < 6; b++) {
            info.bar[b] = pci_bar(bus, dev, func, b);
            info.bar_size[b] = info.bar[b] ? pci_bar_size(bus, dev, func, b) : 0;
        }
    }

    snprintf_name(info.name, sizeof(info.name), "%s", class_name(info.class_code,
                                                                info.subclass));
    pci_add_device(bus, dev, func, &info);
}

void pci_init(void)
{
    g_device_count = 0;
    g_ecam_base = acpi_ecam_base();
    g_ecam_available = false;

    if (g_ecam_base) {
        /* Map the first 16 MiB of ECAM (buses 0..15) and verify that the
         * mapping reads a real vendor id at 0:0.0. */
        u64 virt = vmm_map_mmio(g_ecam_base, 16 * 1024 * 1024);
        if (virt) {
            g_ecam_base = virt;
            g_ecam_available = true;
            u16 vendor = (u16)(*(volatile u32 *)(u64)ecam_address(0, 0, 0, 0) & 0xFFFF);
            if (vendor == 0xFFFF)
                printk("pci: ECAM mapped at %p (bus 0 device 0 is empty)\n",
                       (void *)g_ecam_base);
            else
                printk("pci: ECAM mapped at %p\n", (void *)g_ecam_base);
        }
    }
    if (!g_ecam_available)
        printk("pci: using legacy configuration ports\n");

    /* Brute force enumeration: 4 buses keeps boot time acceptable while still
     * finding the chipset devices QEMU exposes. */
    for (u8 bus = 0; bus < 4; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u16 vendor = (u16)pci_read32(bus, dev, 0, 0x00);
            if (vendor == 0xFFFF || vendor == 0)
                continue;
            pci_probe_function(bus, dev, 0);

            /* Multi-function devices need the remaining 7 functions probed. */
            if (pci_read8(bus, dev, 0, 0x0E) & 0x80) {
                for (u8 func = 1; func < 8; func++) {
                    if ((u16)pci_read32(bus, dev, func, 0x00) != 0xFFFF)
                        pci_probe_function(bus, dev, func);
                }
            }
        }
    }

    printk("pci: %u device(s) found\n", (u64)g_device_count);
}

bool_t pci_find_class(u8 class_code, u8 subclass, u8 *bus, u8 *dev, u8 *func,
                     u8 index)
{
    u8 seen = 0;
    for (u32 i = 0; i < g_device_count; i++) {
        if (g_devices[i].class_code != class_code ||
            g_devices[i].subclass != subclass)
            continue;
        if (seen++ != index)
            continue;
        if (bus) *bus = g_devices[i].bus;
        if (dev) *dev = g_devices[i].dev;
        if (func) *func = g_devices[i].func;
        return true;
    }
    return false;
}

u32 pci_device_count(void)
{
    return g_device_count;
}

const void *pci_device_at(u32 index)
{
    if (index >= g_device_count)
        return NULL_PTR;
    return &g_devices[index];
}

const char *pci_device_name(u32 index)
{
    if (index >= g_device_count)
        return "?";
    return g_devices[index].name;
}

void pci_dump(void)
{
    printk("pci: %u devices\n", (u64)g_device_count);
    for (u32 i = 0; i < g_device_count; i++) {
        const struct pci_device_info *d = &g_devices[i];
        printk("  %02x:%02x.%u %04x:%04x %-28s irq %u bar0 %p\n",
               (u64)d->bus, (u64)d->dev, (u64)d->func, (u64)d->vendor_id,
               (u64)d->device_id, d->name, (u64)d->irq_line, (void *)d->bar[0]);
    }
}

/* Small helper so the enumeration code does not depend on libc's snprintf. */
void snprintf_name(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}
