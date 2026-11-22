/* AxiomOS - ACPI table validation and lookup.
 *
 * Parses the RSDP, walks the RSDT/XSDT, and exposes small helpers used by the
 * kernel and by the boot loader: checksum validation, table lookup by
 * signature, and decoding of the few fields this project relies on (the
 * FADT reset registers, the MADT local APIC base, and the FACS signature).
 */

#include <axiom/kernel.h>

#define AXACPI_MAX_TABLES   32
#define AXACPI_SIGNATURE_LEN 4
#define AXACPI_HEADER_LEN    36

typedef struct axacpi_header
{
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} axacpi_header_t;

typedef struct axacpi_table
{
    u64  address;
    u32  length;
    char signature[4];
    u8   revision;
    bool_t valid;
    u32  checksum_failures;
} axacpi_table_t;

typedef struct axacpi_registry
{
    axacpi_table_t tables[AXACPI_MAX_TABLES];
    u32            count;
    u8             revision;
    bool_t         has_xsdt;
    u32            walk_errors;
    u32            fixed_events;
    u32            local_apic_base;
    u32            cpu_count;
    u32            io_apic_count;
    u8             reset_register;
    u8             reset_value;
    bool_t         initialised;
} axacpi_registry_t;

static void axacpi_zero(void *ptr, u32 size);
static bool_t axacpi_checksum(const u8 *data, u32 length);
static u32 axacpi_read_u32(const u8 *data, u32 offset);
static u8  axacpi_read_u8(const u8 *data, u32 offset);
static u64 axacpi_read_u64(const u8 *data, u32 offset);
static void axacpi_copy_signature(char *dst, const char *src);
static bool_t axacpi_equals(const char *a, const char *b);
static void axacpi_register(axacpi_registry_t *registry, u64 address);
static void axacpi_scan_fadt(axacpi_registry_t *registry, const u8 *table,
                             u32 length);
static void axacpi_scan_madt(axacpi_registry_t *registry, const u8 *table,
                             u32 length);
static u32  axacpi_find(const axacpi_registry_t *registry, const char *signature);

static void axacpi_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u8 axacpi_read_u8(const u8 *data, u32 offset)
{
    return data[offset];
}

static u32 axacpi_read_u32(const u8 *data, u32 offset)
{
    return (u32)data[offset] |
           ((u32)data[offset + 1] << 8) |
           ((u32)data[offset + 2] << 16) |
           ((u32)data[offset + 3] << 24);
}

static u64 axacpi_read_u64(const u8 *data, u32 offset)
{
    u64 low = axacpi_read_u32(data, offset);
    u64 high = axacpi_read_u32(data, offset + 4);
    return low | (high << 32);
}

static bool_t axacpi_checksum(const u8 *data, u32 length)
{
    u32 i;
    u8 sum = 0;
    for (i = 0; i < length; i++)
        sum = (u8)(sum + data[i]);
    return (bool_t)(sum == 0);
}

static void axacpi_copy_signature(char *dst, const char *src)
{
    u32 i;
    for (i = 0; i < AXACPI_SIGNATURE_LEN; i++)
        dst[i] = src[i];
    dst[AXACPI_SIGNATURE_LEN] = 0;
}

static bool_t axacpi_equals(const char *a, const char *b)
{
    u32 i;
    for (i = 0; i < AXACPI_SIGNATURE_LEN; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

static u32 axacpi_find(const axacpi_registry_t *registry, const char *signature)
{
    u32 i;
    for (i = 0; i < registry->count; i++)
    {
        if (registry->tables[i].valid &&
            axacpi_equals(registry->tables[i].signature, signature))
            return i;
    }
    return 0xFFFFFFFFu;
}

static void axacpi_scan_fadt(axacpi_registry_t *registry, const u8 *table,
                             u32 length)
{
    if (length < 132)
        return;
    registry->fixed_events = axacpi_read_u32(table, 48);
    registry->reset_register = axacpi_read_u8(table, 116);
    registry->reset_value = axacpi_read_u8(table, 117);
}

static void axacpi_scan_madt(axacpi_registry_t *registry, const u8 *table,
                             u32 length)
{
    u32 offset = 44;
    registry->local_apic_base = axacpi_read_u32(table, 36);
    while (offset + 2 <= length)
    {
        u8 type = axacpi_read_u8(table, offset);
        u8 entry_length = axacpi_read_u8(table, offset + 1);
        if (entry_length < 2)
            break;
        if (type == 0)
            registry->cpu_count++;
        else if (type == 1)
            registry->io_apic_count++;
        offset += entry_length;
    }
}

static void axacpi_register(axacpi_registry_t *registry, u64 address)
{
    axacpi_table_t *slot;
    const u8 *table;
    if (registry->count >= AXACPI_MAX_TABLES)
        return;
    if (address == 0)
        return;
    table = (const u8 *)address;
    slot = &registry->tables[registry->count];
    axacpi_zero(slot, (u32)sizeof(*slot));
    slot->address = address;
    slot->length = axacpi_read_u32(table, 4);
    axacpi_copy_signature(slot->signature, (const char *)&table[0]);
    slot->revision = table[8];
    slot->valid = axacpi_checksum(table, slot->length);
    if (!slot->valid)
    {
        slot->checksum_failures++;
        registry->walk_errors++;
        return;
    }
    registry->count++;
    if (axacpi_equals(slot->signature, "FACP"))
        axacpi_scan_fadt(registry, table, slot->length);
    else if (axacpi_equals(slot->signature, "APIC"))
        axacpi_scan_madt(registry, table, slot->length);
}

/* -------------------------------------------------------------- public API */

void axacpi_init(axacpi_registry_t *registry)
{
    axacpi_zero(registry, (u32)sizeof(*registry));
    registry->initialised = true;
}

bool_t axacpi_add_root(axacpi_registry_t *registry, u64 rsdp_address)
{
    const u8 *rsdp = (const u8 *)rsdp_address;
    u32 entries;
    u32 i;
    if (rsdp_address == 0)
        return false;
    if (rsdp[0] != 'R' || rsdp[1] != 'S' || rsdp[2] != 'D' || rsdp[3] != 'P')
        return false;
    registry->revision = rsdp[9];
    if (registry->revision >= 2)
    {
        registry->has_xsdt = true;
        {
            u64 xsdt = axacpi_read_u64(rsdp, 24);
            u32 length;
            if (xsdt == 0)
                return false;
            length = axacpi_read_u32((const u8 *)xsdt, 4);
            entries = (length - AXACPI_HEADER_LEN) / 8;
            for (i = 0; i < entries; i++)
            {
                u64 table = axacpi_read_u64((const u8 *)xsdt,
                                            AXACPI_HEADER_LEN + i * 8);
                axacpi_register(registry, table);
            }
        }
        return true;
    }
    {
        u32 rsdt = axacpi_read_u32(rsdp, 16);
        u32 length;
        if (rsdt == 0)
            return false;
        length = axacpi_read_u32((const u8 *)(u64)rsdt, 4);
        entries = (length - AXACPI_HEADER_LEN) / 4;
        for (i = 0; i < entries; i++)
        {
            u32 table = axacpi_read_u32((const u8 *)(u64)rsdt,
                                        (u32)(AXACPI_HEADER_LEN + i * 4));
            axacpi_register(registry, table);
        }
    }
    return true;
}

bool_t axacpi_has(const axacpi_registry_t *registry, const char *signature)
{
    return axacpi_find(registry, signature) != 0xFFFFFFFFu;
}

u32 axacpi_count(const axacpi_registry_t *registry)
{
    return registry->count;
}

u64 axacpi_table_address(const axacpi_registry_t *registry, const char *signature)
{
    u32 index = axacpi_find(registry, signature);
    if (index == 0xFFFFFFFFu)
        return 0;
    return registry->tables[index].address;
}

const char *axacpi_table_name(const axacpi_registry_t *registry, u32 index)
{
    if (index >= registry->count)
        return "";
    return registry->tables[index].signature;
}

u32 axacpi_local_apic_base(const axacpi_registry_t *registry)
{
    return registry->local_apic_base;
}

u32 axacpi_cpu_count(const axacpi_registry_t *registry)
{
    return registry->cpu_count;
}

bool_t axacpi_reset_register(const axacpi_registry_t *registry, u8 *space,
                             u8 *offset, u8 *value)
{
    if (registry->reset_register == 0)
        return false;
    *space = registry->reset_register;
    *offset = 0;
    *value = registry->reset_value;
    return true;
}

u32 axacpi_validate_all(axacpi_registry_t *registry)
{
    u32 i;
    u32 bad = 0;
    for (i = 0; i < registry->count; i++)
    {
        const u8 *table = (const u8 *)registry->tables[i].address;
        if (!axacpi_checksum(table, registry->tables[i].length))
        {
            registry->tables[i].valid = false;
            registry->tables[i].checksum_failures++;
            bad++;
        }
    }
    return bad;
}

void axacpi_dump(const axacpi_registry_t *registry)
{
    u32 i;
    printk("acpi: revision=%u tables=%u xsdt=%d\n", (unsigned)registry->revision,
           (unsigned)registry->count, (int)registry->has_xsdt);
    for (i = 0; i < registry->count; i++)
    {
        printk("acpi:  %s len=%u rev=%u\n", registry->tables[i].signature,
               (unsigned)registry->tables[i].length,
               (unsigned)registry->tables[i].revision);
    }
}

bool_t axacpi_selftest(void)
{
    static u8 madt[64];
    static u8 facp[140];
    static axacpi_registry_t registry;
    u32 i;
    u8 sum;
    for (i = 0; i < sizeof(madt); i++)
        madt[i] = 0;
    for (i = 0; i < sizeof(facp); i++)
        facp[i] = 0;
    madt[0] = 'A';
    madt[1] = 'P';
    madt[2] = 'I';
    madt[3] = 'C';
    madt[4] = 64;
    madt[36] = 0xF0;
    madt[37] = 0x0E;
    sum = 0;
    for (i = 0; i < sizeof(madt); i++)
        sum = (u8)(sum + madt[i]);
    madt[9] = (u8)(0 - sum);
    axacpi_init(&registry);
    if (!axacpi_checksum(madt, 64))
        return false;
    axacpi_register(&registry, (u64)madt);
    if (registry.count != 1)
        return false;
    if (!axacpi_has(&registry, "APIC"))
        return false;
    if (axacpi_local_apic_base(&registry) != 0x0EF0)
        return false;
    facp[0] = 'F';
    facp[1] = 'A';
    facp[2] = 'C';
    facp[3] = 'P';
    facp[4] = 140;
    facp[116] = 0x06;
    facp[117] = 0x0E;
    sum = 0;
    for (i = 0; i < 140; i++)
        sum = (u8)(sum + facp[i]);
    facp[9] = (u8)(0 - sum);
    axacpi_register(&registry, (u64)facp);
    if (registry.count != 2)
        return false;
    if (registry.reset_register != 6)
        return false;
    return true;
}

