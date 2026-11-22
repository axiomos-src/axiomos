/* AxiomOS - virtual memory areas.
 *
 * A VMA describes a contiguous range of a process address space together with
 * its protection, mapping kind and accounting.  The manager answers range
 * queries, splits and merges neighbouring areas, and produces a coalesced list
 * for /proc like dumps of the address space.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXVM_MAX_AREAS        48
#define AXVM_PAGE_SIZE        AXIOM_PAGE_SIZE
#define AXVM_KIND_ANON        0
#define AXVM_KIND_FILE        1
#define AXVM_KIND_STACK       2
#define AXVM_KIND_HEAP        3
#define AXVM_KIND_SHARED      4
#define AXVM_KIND_DEVICE      5

#define AXVM_PROT_READ        0x01
#define AXVM_PROT_WRITE       0x02
#define AXVM_PROT_EXEC        0x04
#define AXVM_PROT_GUARD       0x08
#define AXVM_FLAG_FIXED       0x01
#define AXVM_FLAG_GROWSDOWN   0x02
#define AXVM_FLAG_LOCKED      0x04

#define AXVM_ERR_NONE         0
#define AXVM_ERR_NOMEM        1
#define AXVM_ERR_OVERLAP      2
#define AXVM_ERR_RANGE        3
#define AXVM_ERR_PROTECT      4

typedef struct axvm_area
{
    u64  start;
    u64  end;
    u64  offset;
    u64  inode_hint;
    u32  prot;
    u32  flags;
    u32  pages;
    u32  resident;
    u32  shared_pages;
    u32  handlestamp;
    u8   kind;
    u8   numa_node;
    bool_t used;
    bool_t from_file;
} axvm_area_t;

typedef struct axvm_space
{
    axvm_area_t areas[AXVM_MAX_AREAS];
    u64  base;
    u64  limit;
    u64  brk;
    u64  stack_top;
    u64  stack_guard;
    u32  count;
    u32  splits;
    u32  merges;
    u32  faults;
    u32  grow_calls;
    u32  rejections;
    u32  last_error;
    u32  total_pages;
    bool_t locked;
} axvm_space_t;

static axvm_space_t axvm_space;

static void axvm_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axvm_pages(u64 start, u64 end)
{
    if (end <= start)
        return 0;
    return (u32)(((end - start) + AXVM_PAGE_SIZE - 1) / AXVM_PAGE_SIZE);
}

static axvm_area_t *axvm_lookup(u64 address)
{
    for (u32 i = 0; i < axvm_space.count; i++)
    {
        axvm_area_t *area = &axvm_space.areas[i];
        if (!area->used)
            continue;
        if (address >= area->start && address < area->end)
            return area;
    }
    return NULL_PTR;
}

static axvm_area_t *axvm_free_slot(void)
{
    for (u32 i = 0; i < AXVM_MAX_AREAS; i++)
    {
        if (!axvm_space.areas[i].used)
            return &axvm_space.areas[i];
    }
    return NULL_PTR;
}

static bool_t axvm_overlaps(u64 start, u64 end, const axvm_area_t *except)
{
    for (u32 i = 0; i < axvm_space.count; i++)
    {
        const axvm_area_t *area = &axvm_space.areas[i];
        if (!area->used || area == except)
            continue;
        if (start < area->end && end > area->start)
            return true;
    }
    return false;
}

static void axvm_sort(void)
{
    for (u32 i = 0; i + 1 < axvm_space.count; i++)
    {
        for (u32 j = 0; j + 1 < axvm_space.count - i; j++)
        {
            if (axvm_space.areas[j].start > axvm_space.areas[j + 1].start)
            {
                axvm_area_t temp = axvm_space.areas[j];
                axvm_space.areas[j] = axvm_space.areas[j + 1];
                axvm_space.areas[j + 1] = temp;
            }
        }
    }
}

/* -------------------------------------------------------------- public API */

void axvm_init(u64 base, u64 limit)
{
    axvm_zero(&axvm_space, (u32)sizeof(axvm_space));
    axvm_space.base = base;
    axvm_space.limit = limit;
    axvm_space.brk = base;
    axvm_space.stack_guard = limit - 0x20000;
    axvm_space.stack_top = limit;
}

u64 axvm_map(u64 start, u64 length, u32 prot, u32 flags, u8 kind)
{
    axvm_area_t *area;
    u64 end;
    if (length == 0)
        return 0;
    if (axvm_space.count >= AXVM_MAX_AREAS)
    {
        axvm_space.last_error = AXVM_ERR_NOMEM;
        return 0;
    }
    start = start & ~((u64)AXVM_PAGE_SIZE - 1);
    end = (start + length + AXVM_PAGE_SIZE - 1) & ~((u64)AXVM_PAGE_SIZE - 1);
    if (start < axvm_space.base || end > axvm_space.limit || end <= start)
    {
        axvm_space.last_error = AXVM_ERR_RANGE;
        axvm_space.rejections++;
        return 0;
    }
    if (axvm_overlaps(start, end, NULL_PTR))
    {
        axvm_space.last_error = AXVM_ERR_OVERLAP;
        axvm_space.rejections++;
        return 0;
    }
    area = axvm_free_slot();
    if (area == NULL_PTR)
    {
        axvm_space.last_error = AXVM_ERR_NOMEM;
        return 0;
    }
    axvm_zero(area, (u32)sizeof(*area));
    area->start = start;
    area->end = end;
    area->prot = prot;
    area->flags = flags;
    area->kind = kind;
    area->pages = axvm_pages(start, end);
    area->used = true;
    area->from_file = kind == AXVM_KIND_FILE;
    axvm_space.count++;
    axvm_space.total_pages += area->pages;
    axvm_sort();
    return start;
}

bool_t axvm_unmap(u64 address, u64 length)
{
    axvm_area_t *area = axvm_lookup(address);
    u64 end = address + length;
    if (area == NULL_PTR || length == 0)
    {
        axvm_space.last_error = AXVM_ERR_RANGE;
        return false;
    }
    if (end >= area->end)
    {
        area->used = false;
        axvm_space.total_pages -= area->pages;
        return true;
    }
    if (address <= area->start)
    {
        area->start = end;
        area->pages = axvm_pages(area->start, area->end);
        return true;
    }
    /* Split: keep the tail in a new slot. */
    axvm_area_t *tail = axvm_free_slot();
    if (tail == NULL_PTR)
    {
        axvm_space.last_error = AXVM_ERR_NOMEM;
        return false;
    }
    *tail = *area;
    tail->start = end;
    tail->pages = axvm_pages(tail->start, tail->end);
    area->end = address;
    area->pages = axvm_pages(area->start, area->end);
    axvm_space.count++;
    axvm_space.splits++;
    axvm_sort();
    return true;
}

bool_t axvm_protect(u64 address, u64 length, u32 prot)
{
    axvm_area_t *area = axvm_lookup(address);
    if (area == NULL_PTR || length == 0)
        return false;
    if ((prot & AXVM_PROT_GUARD) && (prot & AXVM_PROT_WRITE))
    {
        axvm_space.last_error = AXVM_ERR_PROTECT;
        return false;
    }
    if (length <= area->end - area->start)
    {
        area->prot = prot;
        return true;
    }
    axvm_space.last_error = AXVM_ERR_RANGE;
    return false;
}

u64 axvm_brk(u64 requested)
{
    axvm_space.grow_calls++;
    if (requested == 0)
        return axvm_space.brk;
    if (requested < axvm_space.base || requested > axvm_space.limit)
        return axvm_space.brk;
    if (requested < axvm_space.stack_guard)
        return axvm_space.brk;
    axvm_space.brk = requested;
    return axvm_space.brk;
}

u64 axvm_stack_grow(u64 fault_address)
{
    if (fault_address < axvm_space.stack_guard)
    {
        axvm_space.last_error = AXVM_ERR_RANGE;
        return 0;
    }
    if (fault_address >= axvm_space.stack_top)
        return 0;
    axvm_space.stack_guard -= 0x10000;
    if (axvm_space.stack_guard < axvm_space.brk)
        axvm_space.stack_guard = axvm_space.brk;
    return axvm_space.stack_guard;
}

bool_t axvm_fault(u64 address, u32 error_code, u32 *kind)
{
    axvm_area_t *area;
    bool_t present = (error_code & 1) != 0;
    bool_t write = (error_code & 2) != 0;
    bool_t user = (error_code & 4) != 0;
    AXIOM_UNUSED_VAR(user);
    axvm_space.faults++;
    area = axvm_lookup(address);
    if (area == NULL_PTR)
    {
        if (address >= axvm_space.stack_guard && address < axvm_space.stack_top)
        {
            axvm_stack_grow(address - 0x1000);
            if (kind != NULL_PTR)
                *kind = AXVM_KIND_STACK;
            return true;
        }
        if (kind != NULL_PTR)
            *kind = 0xFF;
        return false;
    }
    if (present)
        return false;
    if (write && (area->prot & AXVM_PROT_WRITE) == 0)
    {
        axvm_space.last_error = AXVM_ERR_PROTECT;
        return false;
    }
    if (write && (area->prot & AXVM_PROT_GUARD))
        return false;
    if (area->resident < area->pages)
        area->resident++;
    if (kind != NULL_PTR)
        *kind = area->kind;
    return true;
}

bool_t axvm_merge(void)
{
    u32 merges = 0;
    axvm_sort();
    for (u32 i = 0; i + 1 < axvm_space.count; i++)
    {
        axvm_area_t *a = &axvm_space.areas[i];
        axvm_area_t *b = &axvm_space.areas[i + 1];
        if (!a->used || !b->used)
            continue;
        if (a->end != b->start)
            continue;
        if (a->prot != b->prot || a->kind != b->kind || a->flags != b->flags)
            continue;
        if ((a->flags & AXVM_FLAG_LOCKED) != (b->flags & AXVM_FLAG_LOCKED))
            continue;
        a->end = b->end;
        a->pages = axvm_pages(a->start, a->end);
        b->used = false;
        merges++;
    }
    axvm_space.merges += merges;
    return merges != 0;
}

u32 axvm_count(void)
{
    u32 count = 0;
    for (u32 i = 0; i < axvm_space.count; i++)
    {
        if (axvm_space.areas[i].used)
            count++;
    }
    return count;
}

u32 axvm_resident_pages(void)
{
    u32 pages = 0;
    for (u32 i = 0; i < axvm_space.count; i++)
    {
        if (axvm_space.areas[i].used)
            pages += axvm_space.areas[i].resident;
    }
    return pages;
}

bool_t axvm_query(u64 address, axvm_area_t *out)
{
    axvm_area_t *area = axvm_lookup(address);
    if (area == NULL_PTR)
        return false;
    if (out != NULL_PTR)
        *out = *area;
    return true;
}

u64 axvm_find_gap(u64 length, u32 prot)
{
    u64 candidate = axvm_space.base;
    u64 rounded = (length + AXVM_PAGE_SIZE - 1) & ~((u64)AXVM_PAGE_SIZE - 1);
    axvm_sort();
    for (u32 i = 0; i < axvm_space.count; i++)
    {
        axvm_area_t *area = &axvm_space.areas[i];
        if (!area->used)
            continue;
        if (area->start >= candidate + rounded)
            return candidate;
        if (area->end > candidate)
            candidate = area->end;
    }
    if (candidate + rounded <= axvm_space.stack_guard)
        return candidate;
    AXIOM_UNUSED_VAR(prot);
    return 0;
}

bool_t axvm_layout(char *text, u32 capacity)
{
    u32 length = 0;
    if (text == NULL_PTR || capacity < 8)
        return false;
    for (u32 i = 0; i < axvm_space.count; i++)
    {
        axvm_area_t *area = &axvm_space.areas[i];
        if (!area->used)
            continue;
        if (length + 40 >= capacity)
            break;
        text[length++] = '[';
        text[length++] = (char)('0' + (area->kind % 10));
        text[length++] = ']';
        text[length++] = ' ';
        for (u32 shift = 60; shift > 0; shift -= 4)
        {
            u32 nibble = (u32)((area->start >> (shift - 4)) & 0xF);
            text[length++] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
            if (length + 20 >= capacity)
                break;
        }
        text[length++] = '-';
        for (u32 shift = 60; shift > 0; shift -= 4)
        {
            u32 nibble = (u32)((area->end >> (shift - 4)) & 0xF);
            text[length++] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
            if (length + 4 >= capacity)
                break;
        }
        text[length++] = '\n';
    }
    if (length < capacity)
        text[length] = 0;
    return true;
}

void axvm_report(void)
{
    char layout[512];
    printk("axvm: areas=%u pages=%u resident=%u splits=%u merges=%u faults=%u brk=0x%x\n",
           (unsigned)axvm_count(), (unsigned)axvm_space.total_pages,
           (unsigned)axvm_resident_pages(), (unsigned)axvm_space.splits,
           (unsigned)axvm_space.merges, (unsigned)axvm_space.faults,
           (unsigned)axvm_space.brk);
    if (axvm_layout(layout, 512))
        printk("%s", layout);
}

bool_t axvm_selftest(void)
{
    axvm_area_t query;
    u32 kind = 0;
    axvm_init(0x400000, 0x80000000ull);
    u64 a = axvm_map(0x400000, 0x2000, AXVM_PROT_READ | AXVM_PROT_WRITE, 0, AXVM_KIND_ANON);
    if (a != 0x400000)
        return false;
    if (axvm_map(0x401000, 0x1000, AXVM_PROT_READ, 0, AXVM_KIND_ANON) != 0)
        return false;
    u64 b = axvm_map(0x500000, 0x3000, AXVM_PROT_READ, 0, AXVM_KIND_FILE);
    if (b != 0x500000)
        return false;
    if (!axvm_query(0x501000, &query))
        return false;
    if (query.kind != AXVM_KIND_FILE)
        return false;
    if (!axvm_protect(0x500000, 0x3000, AXVM_PROT_READ | AXVM_PROT_EXEC))
        return false;
    if (!axvm_unmap(0x501000, 0x1000))
        return false;
    if (axvm_count() != 3)
        return false;
    if (axvm_fault(0x400000, 0, &kind) != true)
        return false;
    if (kind != AXVM_KIND_ANON)
        return false;
    if (axvm_fault(0x401000, 2, &kind))
        return false;
    if (axvm_map(0x400000, 0x1000, AXVM_PROT_READ, 0, AXVM_KIND_ANON) == 0x400000)
        return false;
    axvm_unmap(0x400000, 0x4000);
    axvm_unmap(0x500000, 0x4000);
    u64 merged = axvm_map(0x600000, 0x2000, AXVM_PROT_READ, 0, AXVM_KIND_HEAP);
    if (merged != 0x600000)
        return false;
    if (axvm_map(0x602000, 0x2000, AXVM_PROT_READ, 0, AXVM_KIND_HEAP) != 0x602000)
        return false;
    if (!axvm_merge())
        return false;
    if (axvm_count() != 1)
        return false;
    if (axvm_brk(0x700000) != 0x700000)
        return false;
    if (axvm_brk(0x10000000) != 0x700000)
        return false;
    if (axvm_stack_grow(0x7FFF0000ull) == 0)
        return false;
    if (axvm_find_gap(0x1000, AXVM_PROT_READ) == 0)
        return false;
    return true;
}
