/* AxiomOS - IOMMU (VT-d style) domain and mapping management.
 *
 * A domain owns a page table tree per device.  The driver facing API is small:
 * create a domain, attach or detach devices, map and unmap ranges, and translate
 * an IOVA back to a physical address for debugging.  The EHCI style fault
 * records are kept in a ring so that the fault handler can print the last few
 * without walking hardware.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXIO_MAX_DOMAINS     8
#define AXIO_MAX_DEVICES     16
#define AXIO_MAX_MAPPINGS    32
#define AXIO_PAGE_SIZE       4096
#define AXIO_FAULT_RING      8

#define AXIO_FLAG_READ       0x0001
#define AXIO_FLAG_WRITE      0x0002
#define AXIO_FLAG_SNOOP      0x0004
#define AXIO_FLAG_CACHE      0x0008
#define AXIO_FLAG_SUPERVISOR 0x0010
#define AXIO_FLAG_SNAPSHOT   0x0020

#define AXIO_FAULT_NONE      0
#define AXIO_FAULT_UNMAPPED  1
#define AXIO_FAULT_PERM      2
#define AXIO_FAULT_ADDRESS   3

typedef struct axio_mapping
{
    u64  iova;
    u64  physical;
    u32  pages;
    u32  flags;
    bool_t used;
} axio_mapping_t;

typedef struct axio_fault
{
    u64  address;
    u32  device;
    u32  reason;
    u32  sequence;
    bool_t present;
} axio_fault_t;

typedef struct axio_domain
{
    axio_mapping_t mappings[AXIO_MAX_MAPPINGS];
    u32  id;
    u32  device_mask;
    u32  mapping_count;
    u32  pages_mapped;
    u32  maps_created;
    u32  maps_destroyed;
    u32  faults;
    u32  address_width;
    u64  page_table_root;
    bool_t used;
    bool_t enabled;
} axio_domain_t;

typedef struct axio_state
{
    axio_domain_t domains[AXIO_MAX_DOMAINS];
    axio_fault_t faults[AXIO_FAULT_RING];
    u32  fault_head;
    u32  domain_count;
    u32  device_count;
    u32  translations;
    u32  translation_misses;
    u32  permission_denials;
    u32  units;
    u64  register_base;
    bool_t present;
    bool_t ready;
} axio_state_t;

static axio_state_t axio_state;

static void axio_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axio_domain_t *axio_domain(u32 index)
{
    if (index >= axio_state.domain_count)
        return NULL_PTR;
    if (!axio_state.domains[index].used)
        return NULL_PTR;
    return &axio_state.domains[index];
}

static void axio_log_fault(u64 address, u32 device, u32 reason)
{
    axio_fault_t *fault = &axio_state.faults[axio_state.fault_head];
    fault->address = address;
    fault->device = device;
    fault->reason = reason;
    fault->sequence = axio_state.fault_head;
    fault->present = true;
    axio_state.fault_head = (axio_state.fault_head + 1) % AXIO_FAULT_RING;
}

static axio_mapping_t *axio_find_mapping(axio_domain_t *domain, u64 iova)
{
    u32 i;
    for (i = 0; i < AXIO_MAX_MAPPINGS; i++)
    {
        axio_mapping_t *mapping = &domain->mappings[i];
        if (!mapping->used)
            continue;
        if (iova >= mapping->iova && iova < mapping->iova + (u64)mapping->pages * AXIO_PAGE_SIZE)
            return mapping;
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axio_init(u64 register_base, u32 units)
{
    axio_zero(&axio_state, (u32)sizeof(axio_state));
    axio_state.register_base = register_base;
    axio_state.units = units == 0 ? 1 : units;
    axio_state.present = register_base != 0;
    axio_state.ready = true;
}

u32 axio_create_domain(u32 address_width)
{
    axio_domain_t *domain;
    if (axio_state.domain_count >= AXIO_MAX_DOMAINS)
        return 0xFFFFFFFFu;
    domain = &axio_state.domains[axio_state.domain_count];
    axio_zero(domain, (u32)sizeof(*domain));
    domain->id = axio_state.domain_count;
    domain->address_width = address_width > 48 ? 48 : address_width;
    domain->page_table_root = 0x1000ull + (u64)axio_state.domain_count * AXIO_PAGE_SIZE;
    domain->used = true;
    domain->enabled = true;
    axio_state.domain_count++;
    return domain->id;
}

bool_t axio_attach(u32 domain_index, u32 bdf)
{
    axio_domain_t *domain = axio_domain(domain_index);
    if (domain == NULL_PTR || bdf >= 32)
        return false;
    domain->device_mask |= (1u << bdf);
    axio_state.device_count++;
    return true;
}

bool_t axio_detach(u32 domain_index, u32 bdf)
{
    axio_domain_t *domain = axio_domain(domain_index);
    if (domain == NULL_PTR || bdf >= 32)
        return false;
    if ((domain->device_mask & (1u << bdf)) == 0)
        return false;
    domain->device_mask &= ~(1u << bdf);
    if (axio_state.device_count > 0)
        axio_state.device_count--;
    return true;
}

bool_t axio_map(u32 domain_index, u64 iova, u64 physical, u32 pages, u32 flags)
{
    axio_domain_t *domain = axio_domain(domain_index);
    u32 i;
    if (domain == NULL_PTR || pages == 0)
        return false;
    if ((iova & (AXIO_PAGE_SIZE - 1)) != 0 || (physical & (AXIO_PAGE_SIZE - 1)) != 0)
        return false;
    if (iova + (u64)pages * AXIO_PAGE_SIZE < iova)
        return false;
    for (i = 0; i < AXIO_MAX_MAPPINGS; i++)
    {
        axio_mapping_t *mapping = &domain->mappings[i];
        if (mapping->used)
            continue;
        mapping->iova = iova;
        mapping->physical = physical;
        mapping->pages = pages;
        mapping->flags = flags;
        mapping->used = true;
        domain->mapping_count++;
        domain->pages_mapped += pages;
        domain->maps_created++;
        return true;
    }
    return false;
}

bool_t axio_unmap(u32 domain_index, u64 iova, u32 pages)
{
    axio_domain_t *domain = axio_domain(domain_index);
    axio_mapping_t *mapping;
    if (domain == NULL_PTR)
        return false;
    mapping = axio_find_mapping(domain, iova);
    if (mapping == NULL_PTR)
        return false;
    if (mapping->iova != iova)
        return false;
    if (pages > mapping->pages)
        pages = mapping->pages;
    mapping->pages -= pages;
    domain->pages_mapped -= pages;
    if (mapping->pages == 0)
    {
        mapping->used = false;
        mapping->iova = 0;
        domain->mapping_count--;
        domain->maps_destroyed++;
    }
    else
    {
        mapping->iova += (u64)pages * AXIO_PAGE_SIZE;
        mapping->physical += (u64)pages * AXIO_PAGE_SIZE;
    }
    return true;
}

u64 axio_translate(u32 domain_index, u64 iova, u32 access_flags)
{
    axio_domain_t *domain = axio_domain(domain_index);
    axio_mapping_t *mapping;
    if (domain == NULL_PTR || !domain->enabled)
    {
        axio_state.translation_misses++;
        return 0;
    }
    mapping = axio_find_mapping(domain, iova);
    axio_state.translations++;
    if (mapping == NULL_PTR)
    {
        axio_state.translation_misses++;
        domain->faults++;
        axio_log_fault(iova, domain->device_mask, AXIO_FAULT_UNMAPPED);
        return 0;
    }
    if ((access_flags & AXIO_FLAG_WRITE) != 0 && (mapping->flags & AXIO_FLAG_WRITE) == 0)
    {
        axio_state.permission_denials++;
        domain->faults++;
        axio_log_fault(iova, domain->device_mask, AXIO_FAULT_PERM);
        return 0;
    }
    return mapping->physical + (iova - mapping->iova);
}

u32 axio_fault_reason(u32 domain_index, u64 iova)
{
    axio_domain_t *domain = axio_domain(domain_index);
    if (domain == NULL_PTR)
        return AXIO_FAULT_NONE;
    if (axio_find_mapping(domain, iova) == NULL_PTR)
        return AXIO_FAULT_UNMAPPED;
    return AXIO_FAULT_PERM;
}

bool_t axio_lookup_fault(u32 index, u64 *address, u32 *reason)
{
    u32 cursor;
    if (index >= AXIO_FAULT_RING)
        return false;
    cursor = (axio_state.fault_head + AXIO_FAULT_RING - 1 - index) % AXIO_FAULT_RING;
    if (!axio_state.faults[cursor].present)
        return false;
    if (address != NULL_PTR)
        *address = axio_state.faults[cursor].address;
    if (reason != NULL_PTR)
        *reason = axio_state.faults[cursor].reason;
    return true;
}

bool_t axio_enable(u32 domain_index, bool_t enabled)
{
    axio_domain_t *domain = axio_domain(domain_index);
    if (domain == NULL_PTR)
        return false;
    domain->enabled = enabled;
    return true;
}

u32 axio_mapped_pages(u32 domain_index)
{
    axio_domain_t *domain = axio_domain(domain_index);
    return domain == NULL_PTR ? 0 : domain->pages_mapped;
}

u32 axio_domain_faults(u32 domain_index)
{
    axio_domain_t *domain = axio_domain(domain_index);
    return domain == NULL_PTR ? 0 : domain->faults;
}

void axio_report(void)
{
    u32 i;
    printk("axio: present=%u units=%u domains=%u devices=%u xlate=%u miss=%u perm=%u\n",
           (unsigned)axio_state.present, (unsigned)axio_state.units,
           (unsigned)axio_state.domain_count, (unsigned)axio_state.device_count,
           (unsigned)axio_state.translations, (unsigned)axio_state.translation_misses,
           (unsigned)axio_state.permission_denials);
    for (i = 0; i < axio_state.domain_count; i++)
    {
        const axio_domain_t *domain = &axio_state.domains[i];
        printk("  domain %u aw=%u maps=%u pages=%u faults=%u devices=0x%x enabled=%u\n",
               (unsigned)i, (unsigned)domain->address_width,
               (unsigned)domain->mapping_count, (unsigned)domain->pages_mapped,
               (unsigned)domain->faults, (unsigned)domain->device_mask,
               (unsigned)domain->enabled);
    }
}

bool_t axio_selftest(void)
{
    u32 domain;
    u64 physical;
    axio_init(0xFED90000ull, 1);
    if (!axio_state.present)
        return false;
    domain = axio_create_domain(48);
    if (domain == 0)
        return false;
    if (!axio_attach(domain, 1))
        return false;
    if (!axio_map(domain, 0x10000, 0x80000, 4, AXIO_FLAG_READ | AXIO_FLAG_WRITE))
        return false;
    if (axio_map(domain, 0x1, 0x0, 1, AXIO_FLAG_READ))
        return false;
    physical = axio_translate(domain, 0x12000, AXIO_FLAG_READ);
    if (physical != 0x82000)
        return false;
    if (axio_translate(domain, 0x12000, AXIO_FLAG_WRITE) != 0x82000)
        return false;
    if (axio_translate(domain, 0x40000, AXIO_FLAG_READ) != 0)
        return false;
    if (axio_state.translation_misses != 1)
        return false;
    if (axio_fault_reason(domain, 0x40000) != AXIO_FAULT_UNMAPPED)
        return false;
    if (!axio_unmap(domain, 0x10000, 2))
        return false;
    if (axio_mapped_pages(domain) != 2)
        return false;
    if (!axio_unmap(domain, 0x12000, 2))
        return false;
    if (axio_mapped_pages(domain) != 0)
        return false;
    if (!axio_detach(domain, 1))
        return false;
    if (!axio_enable(domain, false))
        return false;
    {
        u64 address = 0;
        u32 reason = 0;
        if (!axio_lookup_fault(0, &address, &reason))
            return false;
        if (reason != AXIO_FAULT_UNMAPPED)
            return false;
    }
    return true;
}
