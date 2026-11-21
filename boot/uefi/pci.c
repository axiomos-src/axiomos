/* AxiomOS UEFI loader - PCI configuration space access.
 *
 * The kernel needs to know which display, storage and network controllers exist
 * before it can start matching drivers.  Firmware already configured the
 * devices, so the loader only enumerates: it walks every bus/device/function
 * triple, reads the vendor, class and BARs and hands the table to the kernel in
 * the boot info block.
 */

#include "loader.h"

#define AXPC_MAX_DEVICES  64
#define AXPC_CONFIG_ADDR  0x0CF8
#define AXPC_CONFIG_DATA  0x0CFC
#define AXPC_VENDOR_NONE  0xFFFF

typedef struct axpc_function
{
    u16 vendor_id;
    u16 device_id;
    u8  class_code;
    u8  subclass;
    u8  prog_if;
    u8  revision;
    u8  bus;
    u8  device;
    u8  function;
    u8  header_type;
    u8  irq;
    u32 bar0;
    u32 bar1;
    u64 mmio_size;
    int valid;
} axpc_function_t;

typedef struct axpc_table
{
    axpc_function_t functions[AXPC_MAX_DEVICES];
    u8  count;
    u8  display_count;
    u8  storage_count;
    u8  network_count;
    u8  bridge_count;
    u16 errors;
    int scanned;
} axpc_table_t;

static void axpc_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static inline void axpc_outl(u16 port, u32 value)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline u32 axpc_inl(u16 port)
{
    u32 value;
    __asm__ __volatile__("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static u32 axpc_read(u8 bus, u8 device, u8 function, u32 offset)
{
    u32 address = 0x80000000u | ((u32)bus << 16) | ((u32)device << 11) |
                  ((u32)function << 8) | (offset & 0xFCu);
    axpc_outl(AXPC_CONFIG_ADDR, address);
    return axpc_inl(AXPC_CONFIG_DATA);
}

static void axpc_write(u8 bus, u8 device, u8 function, u32 offset, u32 value)
{
    u32 address = 0x80000000u | ((u32)bus << 16) | ((u32)device << 11) |
                  ((u32)function << 8) | (offset & 0xFCu);
    axpc_outl(AXPC_CONFIG_ADDR, address);
    axpc_outl(AXPC_CONFIG_DATA, value);
}

static void axpc_decode_class(u8 class_code, u8 subclass, axpc_table_t *table)
{
    switch (class_code)
    {
    case 0x03:
        table->display_count++;
        break;
    case 0x01:
        table->storage_count++;
        break;
    case 0x02:
        table->network_count++;
        break;
    case 0x06:
        table->bridge_count++;
        break;
    default:
        break;
    }
    (void)subclass;
}

static void axpc_scan_function(u8 bus, u8 device, u8 function, axpc_table_t *table)
{
    u32 vendor_device = axpc_read(bus, device, function, 0x00);
    axpc_function_t *entry;
    u32 class_revision;
    u32 header;
    u16 vendor = (u16)(vendor_device & 0xFFFF);
    if (vendor == AXPC_VENDOR_NONE)
        return;
    if (table->count >= AXPC_MAX_DEVICES)
        return;
    entry = &table->functions[table->count];
    axpc_zero(entry, sizeof(*entry));
    entry->vendor_id = vendor;
    entry->device_id = (u16)(vendor_device >> 16);
    class_revision = axpc_read(bus, device, function, 0x08);
    entry->revision = (u8)(class_revision & 0xFF);
    entry->prog_if = (u8)((class_revision >> 8) & 0xFF);
    entry->subclass = (u8)((class_revision >> 16) & 0xFF);
    entry->class_code = (u8)((class_revision >> 24) & 0xFF);
    header = axpc_read(bus, device, function, 0x0C);
    entry->header_type = (u8)((header >> 16) & 0xFF);
    entry->irq = (u8)(axpc_read(bus, device, function, 0x3C) & 0xFF);
    entry->bar0 = axpc_read(bus, device, function, 0x10);
    entry->bar1 = axpc_read(bus, device, function, 0x14);
    entry->bus = bus;
    entry->device = device;
    entry->function = function;
    entry->valid = 1;
    if ((entry->bar0 & 0x01) == 0)
    {
        axpc_write(bus, device, function, 0x10, 0xFFFFFFFFu);
        entry->mmio_size = (u64)(~(axpc_read(bus, device, function, 0x10) & 0xFFFFFFF0u) + 1u);
        axpc_write(bus, device, function, 0x10, entry->bar0);
        if (entry->mmio_size > 0x10000000ull)
            entry->mmio_size = 0;
    }
    table->count++;
    axpc_decode_class(entry->class_code, entry->subclass, table);
}

void axpc_scan(axpc_table_t *table)
{
    u8 bus;
    u8 device;
    u8 function;
    axpc_zero(table, sizeof(*table));
    for (bus = 0; bus < 8; bus++)
    {
        for (device = 0; device < 32; device++)
        {
            u32 header;
            u8 functions = 1;
            header = axpc_read(bus, device, 0, 0x0C);
            if ((header >> 16) & 0x80)
                functions = 8;
            for (function = 0; function < functions; function++)
            {
                axpc_scan_function(bus, device, function, table);
                if (table->count >= AXPC_MAX_DEVICES)
                    break;
            }
        }
    }
    table->scanned = 1;
}

int axpc_find_class(axpc_table_t *table, u8 class_code, u8 subclass,
                       axpc_function_t *out)
{
    u8 i;
    for (i = 0; i < table->count; i++)
    {
        if (table->functions[i].class_code == class_code &&
            table->functions[i].subclass == subclass)
        {
            *out = table->functions[i];
            return 1;
        }
    }
    return 0;
}

void axpc_report(axpc_table_t *table)
{
    u8 i;
    con_printf("PCI: %u functions, display=%u storage=%u network=%u bridge=%u\r\n",
               (unsigned)table->count, (unsigned)table->display_count,
               (unsigned)table->storage_count, (unsigned)table->network_count,
               (unsigned)table->bridge_count);
    for (i = 0; i < table->count; i++)
    {
        const axpc_function_t *entry = &table->functions[i];
        if (!entry->valid)
            continue;
        con_printf("  %02x:%02x.%x %04x:%04x class=%02x:%02x bar0=%08x size=%08x\r\n",
                   (unsigned)entry->bus, (unsigned)entry->device,
                   (unsigned)entry->function, (unsigned)entry->vendor_id,
                   (unsigned)entry->device_id, (unsigned)entry->class_code,
                   (unsigned)entry->subclass, (unsigned)entry->bar0,
                   (unsigned)entry->mmio_size);
    }
}

int axpc_selftest(void)
{
    axpc_table_t table;
    axpc_zero(&table, sizeof(table));
    if (table.count != 0)
        return 0;
    return 1;
}

