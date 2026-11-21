/* AxiomOS - x86_64 paging helpers.
 *
 * The kernel runs on 4-level page tables with a shared physical identity map in
 * the low half.  These helpers walk, populate and tear down tables, and keep a
 * small record of the mapping metadata (attributes plus a name) so the sysmon
 * app can show what the address space looks like without parsing page tables.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXPG_ENTRIES        512
#define AXPG_MAX_TRACKED    128
#define AXPG_ENTRY_PRESENT  0x001ull
#define AXPG_ENTRY_WRITE    0x002ull
#define AXPG_ENTRY_USER     0x004ull
#define AXPG_ENTRY_NX       0x8000000000000000ull
#define AXPG_ADDR_MASK      0x000FFFFFFFFFF000ull

typedef struct axpg_table
{
    u64 entries[AXPG_ENTRIES];
} axpg_table_t;

typedef struct axpg_mapping
{
    u64         virtual_address;
    u64         physical_address;
    u64         pages;
    u64         flags;
    u32         owner;
    const char *name;
    bool_t      present;
    bool_t      huge;
} axpg_mapping_t;

typedef struct axpg_space
{
    axpg_mapping_t mappings[AXPG_MAX_TRACKED];
    u32  count;
    u64  pml4_root;
    u64  mapped_pages;
    u32  walks;
    u32  faults;
    u32  splits;
    u32  huge_entries;
    bool_t ready;
} axpg_space_t;

static axpg_space_t axpg_space;

static void axpg_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u16 axpg_index(u64 address, u32 level)
{
    u32 shift = 12 + level * 9;
    return (u16)((address >> shift) & 0x1FFu);
}

static axpg_table_t *axpg_table_at(u64 physical)
{
    if (physical == 0)
        return NULL_PTR;
    return (axpg_table_t *)(u64)physical;
}

static u64 axpg_entry_address(u64 entry)
{
    return entry & AXPG_ADDR_MASK;
}

static axpg_mapping_t *axpg_find(u64 virtual_address)
{
    u32 i;
    for (i = 0; i < axpg_space.count; i++)
    {
        axpg_mapping_t *mapping = &axpg_space.mappings[i];
        if (!mapping->present)
            continue;
        if (virtual_address >= mapping->virtual_address &&
            virtual_address < mapping->virtual_address + mapping->pages * 4096ull)
            return mapping;
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axpg_init(u64 pml4_root)
{
    axpg_zero(&axpg_space, (u32)sizeof(axpg_space));
    axpg_space.pml4_root = pml4_root;
    axpg_space.ready = true;
}

bool_t axpg_map(u64 virtual_address, u64 physical_address, u64 pages, u64 flags,
                const char *name)
{
    axpg_mapping_t *mapping;
    if (!axpg_space.ready || pages == 0)
        return false;
    if ((virtual_address & 0xFFFull) != 0 || (physical_address & 0xFFFull) != 0)
        return false;
    if (axpg_space.count >= AXPG_MAX_TRACKED)
        return false;
    mapping = &axpg_space.mappings[axpg_space.count];
    axpg_zero(mapping, (u32)sizeof(*mapping));
    mapping->virtual_address = virtual_address;
    mapping->physical_address = physical_address;
    mapping->pages = pages;
    mapping->flags = flags;
    mapping->owner = 0;
    mapping->name = name;
    mapping->present = true;
    mapping->huge = (bool_t)(pages >= 512 && (flags & 0x80) != 0);
    if (mapping->huge)
        axpg_space.huge_entries++;
    axpg_space.mapped_pages += pages;
    axpg_space.count++;
    return true;
}

bool_t axpg_unmap(u64 virtual_address)
{
    axpg_mapping_t *mapping = axpg_find(virtual_address);
    if (mapping == NULL_PTR)
        return false;
    mapping->present = false;
    if (axpg_space.mapped_pages >= mapping->pages)
        axpg_space.mapped_pages -= mapping->pages;
    return true;
}

bool_t axpg_translate(u64 virtual_address, u64 *physical)
{
    axpg_table_t *pml4;
    axpg_table_t *pdpt;
    axpg_table_t *pd;
    axpg_table_t *pt;
    u64 entry;
    if (!axpg_space.ready || axpg_space.pml4_root == 0)
        return false;
    axpg_space.walks++;
    pml4 = axpg_table_at(axpg_space.pml4_root);
    if (pml4 == NULL_PTR)
        return false;
    entry = pml4->entries[axpg_index(virtual_address, 3)];
    if ((entry & AXPG_ENTRY_PRESENT) == 0)
        return false;
    pdpt = axpg_table_at(axpg_entry_address(entry));
    if (pdpt == NULL_PTR)
        return false;
    entry = pdpt->entries[axpg_index(virtual_address, 2)];
    if ((entry & AXPG_ENTRY_PRESENT) == 0)
        return false;
    if ((entry & 0x80) != 0)
    {
        *physical = (entry & 0x000FFFFFC0000000ull) +
                    (virtual_address & 0x3FFFFFFFull);
        return true;
    }
    pd = axpg_table_at(axpg_entry_address(entry));
    if (pd == NULL_PTR)
        return false;
    entry = pd->entries[axpg_index(virtual_address, 1)];
    if ((entry & AXPG_ENTRY_PRESENT) == 0)
        return false;
    if ((entry & 0x80) != 0)
    {
        *physical = (entry & 0x000FFFFFFFE00000ull) +
                    (virtual_address & 0x1FFFFFull);
        return true;
    }
    pt = axpg_table_at(axpg_entry_address(entry));
    if (pt == NULL_PTR)
        return false;
    entry = pt->entries[axpg_index(virtual_address, 0)];
    if ((entry & AXPG_ENTRY_PRESENT) == 0)
        return false;
    *physical = axpg_entry_address(entry) + (virtual_address & 0xFFFull);
    return true;
}

bool_t axpg_is_mapped(u64 virtual_address)
{
    u64 physical = 0;
    return axpg_translate(virtual_address, &physical);
}

bool_t axpg_protect(u64 virtual_address, u64 flags)
{
    axpg_mapping_t *mapping = axpg_find(virtual_address);
    if (mapping == NULL_PTR)
        return false;
    mapping->flags = flags;
    return true;
}

u64 axpg_flags_for(u8 kind)
{
    switch (kind)
    {
    case 0: return AXPG_ENTRY_PRESENT | AXPG_ENTRY_WRITE;         /* kernel data */
    case 1: return AXPG_ENTRY_PRESENT | AXPG_ENTRY_WRITE | AXPG_ENTRY_NX;
    case 2: return AXPG_ENTRY_PRESENT | AXPG_ENTRY_USER | AXPG_ENTRY_NX;
    case 3: return AXPG_ENTRY_PRESENT | AXPG_ENTRY_USER | AXPG_ENTRY_WRITE;
    default: return AXPG_ENTRY_PRESENT;
    }
}

u64 axpg_mapped_pages(void)
{
    return axpg_space.mapped_pages;
}

u32 axpg_mapping_count(void)
{
    return axpg_space.count;
}

const char *axpg_mapping_name(u32 index)
{
    if (index >= axpg_space.count)
        return "";
    return axpg_space.mappings[index].name == NULL_PTR ? "" :
           axpg_space.mappings[index].name;
}

void axpg_note_fault(void)
{
    axpg_space.faults++;
}

void axpg_report(void)
{
    u32 i;
    printk("axpg: mappings=%u pages=%llu walks=%u faults=%u huge=%u\n",
           (unsigned)axpg_space.count,
           (unsigned long long)axpg_space.mapped_pages,
           (unsigned)axpg_space.walks, (unsigned)axpg_space.faults,
           (unsigned)axpg_space.huge_entries);
    for (i = 0; i < axpg_space.count; i++)
    {
        const axpg_mapping_t *mapping = &axpg_space.mappings[i];
        if (!mapping->present)
            continue;
        printk("  %-12s 0x%llx -> 0x%llx pages=%llu flags=0x%llx\n",
               mapping->name == NULL_PTR ? "?" : mapping->name,
               (unsigned long long)mapping->virtual_address,
               (unsigned long long)mapping->physical_address,
               (unsigned long long)mapping->pages,
               (unsigned long long)mapping->flags);
    }
}

/* ------------------------------------------------------------ test tables */

static axpg_table_t axpg_test_pml4;
static axpg_table_t axpg_test_pdpt;
static axpg_table_t axpg_test_pd;
static axpg_table_t axpg_test_pt;

bool_t axpg_selftest(void)
{
    u64 physical = 0;
    axpg_zero(&axpg_test_pml4, (u32)sizeof(axpg_test_pml4));
    axpg_zero(&axpg_test_pdpt, (u32)sizeof(axpg_test_pdpt));
    axpg_zero(&axpg_test_pd, (u32)sizeof(axpg_test_pd));
    axpg_zero(&axpg_test_pt, (u32)sizeof(axpg_test_pt));
    axpg_test_pml4.entries[0] = (u64)(u64)&axpg_test_pdpt | AXPG_ENTRY_PRESENT |
                                AXPG_ENTRY_WRITE;
    axpg_test_pdpt.entries[0] = (u64)(u64)&axpg_test_pd | AXPG_ENTRY_PRESENT |
                                AXPG_ENTRY_WRITE;
    axpg_test_pd.entries[0] = (u64)(u64)&axpg_test_pt | AXPG_ENTRY_PRESENT |
                              AXPG_ENTRY_WRITE;
    axpg_test_pt.entries[2] = 0x0000000040000000ull | AXPG_ENTRY_PRESENT |
                              AXPG_ENTRY_WRITE;
    axpg_init((u64)(u64)&axpg_test_pml4);
    if (!axpg_translate(0x2000, &physical))
        return false;
    if (physical != 0x40000000ull)
        return false;
    if (axpg_translate(0x3000, &physical))
        return false;
    if (!axpg_map(0xFFFF800000000000ull, 0, 512, 0x03, "identity"))
        return false;
    if (axpg_mapped_pages() != 512)
        return false;
    if (axpg_mapping_count() != 1)
        return false;
    if (!axpg_unmap(0xFFFF800000000000ull))
        return false;
    if (axpg_mapped_pages() != 0)
        return false;
    if (axpg_flags_for(3) != (AXPG_ENTRY_PRESENT | AXPG_ENTRY_USER | AXPG_ENTRY_WRITE))
        return false;
    return true;
}
