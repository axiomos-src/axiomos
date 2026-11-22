/* AxiomOS - kernel slab allocator.
 *
 * A second level allocator sitting above the page manager.  Each size class
 * owns a list of slabs; a slab is a contiguous run of blocks carved out of one
 * page-aligned region supplied by the caller.  Free lists are kept per slab so
 * that a single block free is O(1) and a slab can be returned to the page
 * manager as soon as the last block goes away.
 *
 * Classes: 16, 32, 64, 128, 256, 512, 1024, 2048 bytes.  Larger requests are
 * passed straight through to the backing allocator.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXSL_CLASSES        8
#define AXSL_SLABS_PER_CLASS 16
#define AXSL_MAX_PAGES      4096
#define AXSL_MAGIC          0x534C4142u

typedef struct axsl_slab
{
    u64   base;
    u32   block_size;
    u16   capacity;
    u16   in_use;
    u16   first_free;
    u16   magic;
    u8    class_index;
    u8    owner;
    bool_t resident;
} axsl_slab_t;

typedef struct axsl_class
{
    u32          block_size;
    axsl_slab_t *slabs[AXSL_SLABS_PER_CLASS];
    u32          slab_count;
    u32          allocations;
    u32          frees;
    u32          refills;
    u32          active_blocks;
} axsl_class_t;

typedef struct axsl_state
{
    axsl_class_t classes[AXSL_CLASSES];
    u64          page_pool[AXSL_MAX_PAGES];
    u32          pages_total;
    u32          pages_used;
    u32          oom_count;
    bool_t       ready;
} axsl_state_t;

typedef struct axsl_block_header
{
    u32 magic;
    u8  class_index;
    u8  slab_index;
    u16 block_index;
    u32 requested;
    u32 reserved;
} axsl_block_header_t;

static axsl_state_t axsl_state;

static const u32 axsl_class_sizes[AXSL_CLASSES] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };

static void axsl_zero(void *ptr, u32 size);
static u8   axsl_pick_class(u32 bytes);
static bool_t axsl_refill(u8 class_index);
static axsl_slab_t *axsl_find_room(u8 class_index);
static bool_t axsl_pool_take(u64 *page);
static void   axsl_pool_give(u64 page);
static axsl_block_header_t *axsl_header(void *pointer);
static u32    axsl_align(u32 value, u32 align);

static void axsl_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axsl_align(u32 value, u32 align)
{
    return (value + align - 1) & ~(align - 1);
}

static u8 axsl_pick_class(u32 bytes)
{
    u8 index;
    for (index = 0; index < AXSL_CLASSES; index++)
    {
        if (bytes <= axsl_class_sizes[index])
            return index;
    }
    return 0xFF;
}

static bool_t axsl_pool_take(u64 *page)
{
    if (axsl_state.pages_used >= axsl_state.pages_total)
        return false;
    *page = axsl_state.page_pool[axsl_state.pages_used];
    axsl_state.pages_used++;
    return true;
}

static void axsl_pool_give(u64 page)
{
    u32 i;
    for (i = 0; i < axsl_state.pages_used; i++)
    {
        if (axsl_state.page_pool[i] == page)
        {
            axsl_state.page_pool[i] = axsl_state.page_pool[axsl_state.pages_used - 1];
            axsl_state.pages_used--;
            return;
        }
    }
}

static axsl_block_header_t *axsl_header(void *pointer)
{
    return (axsl_block_header_t *)((u8 *)pointer - sizeof(axsl_block_header_t));
}

static bool_t axsl_refill(u8 class_index)
{
    axsl_class_t *class_ref = &axsl_state.classes[class_index];
    u64 page;
    u32 block_size;
    u16 capacity;
    u16 i;
    axsl_slab_t *slab;

    if (class_ref->slab_count >= AXSL_SLABS_PER_CLASS)
        return false;
    if (!axsl_pool_take(&page))
    {
        axsl_state.oom_count++;
        return false;
    }
    block_size = axsl_align(class_ref->block_size + (u32)sizeof(axsl_block_header_t), 16);
    capacity = (u16)(AXIOM_PAGE_SIZE / block_size);
    if (capacity == 0)
        return false;
    slab = (axsl_slab_t *)kmalloc(sizeof(axsl_slab_t));
    if (slab == NULL_PTR)
        return false;
    axsl_zero(slab, (u32)sizeof(*slab));
    slab->base = page;
    slab->block_size = block_size;
    slab->capacity = capacity;
    slab->in_use = 0;
    slab->first_free = 0;
    slab->class_index = class_index;
    slab->magic = (u16)AXSL_MAGIC;
    slab->resident = true;
    for (i = 0; i < capacity; i++)
    {
        axsl_block_header_t *header =
            (axsl_block_header_t *)(page + (u64)i * block_size);
        header->magic = 0;
        header->class_index = class_index;
        header->block_index = i;
        header->slab_index = (u8)class_ref->slab_count;
        header->requested = 0;
    }
    class_ref->slabs[class_ref->slab_count] = slab;
    class_ref->slab_count++;
    class_ref->refills++;
    return true;
}

static axsl_slab_t *axsl_find_room(u8 class_index)
{
    axsl_class_t *class_ref = &axsl_state.classes[class_index];
    u32 i;
    for (i = 0; i < class_ref->slab_count; i++)
    {
        if (class_ref->slabs[i]->in_use < class_ref->slabs[i]->capacity)
            return class_ref->slabs[i];
    }
    if (axsl_refill(class_index))
    {
        if (class_ref->slab_count == 0)
            return NULL_PTR;
        return class_ref->slabs[class_ref->slab_count - 1];
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axsl_init(void)
{
    axsl_zero(&axsl_state, (u32)sizeof(axsl_state));
    {
        u8 i;
        for (i = 0; i < AXSL_CLASSES; i++)
        {
            axsl_state.classes[i].block_size = axsl_class_sizes[i];
            axsl_state.classes[i].slab_count = 0;
        }
    }
    axsl_state.ready = true;
}

bool_t axsl_provide_page(u64 physical)
{
    if (axsl_state.pages_total >= AXSL_MAX_PAGES)
        return false;
    axsl_state.page_pool[axsl_state.pages_total] = physical;
    axsl_state.pages_total++;
    return true;
}

u32 axsl_provide_pages(u64 base, u32 count)
{
    u32 added = 0;
    u32 i;
    for (i = 0; i < count; i++)
    {
        if (axsl_provide_page(base + (u64)i * AXIOM_PAGE_SIZE))
            added++;
    }
    return added;
}

void *axsl_alloc(u32 bytes)
{
    u8 class_index;
    axsl_slab_t *slab;
    axsl_block_header_t *header;
    u16 slot;
    void *result;

    if (!axsl_state.ready || bytes == 0)
        return NULL_PTR;
    class_index = axsl_pick_class(bytes);
    if (class_index == 0xFF)
        return kmalloc(bytes);
    slab = axsl_find_room(class_index);
    if (slab == NULL_PTR)
        return NULL_PTR;
    slot = slab->first_free;
    if (slot >= slab->capacity)
        return NULL_PTR;
    header = (axsl_block_header_t *)(slab->base + (u64)slot * slab->block_size);
    header->magic = AXSL_MAGIC;
    header->requested = bytes;
    result = (void *)((u8 *)header + sizeof(axsl_block_header_t));
    slab->first_free = (u16)(slot + 1);
    slab->in_use++;
    axsl_state.classes[class_index].allocations++;
    axsl_state.classes[class_index].active_blocks++;
    return result;
}

void *axsl_calloc(u32 bytes)
{
    void *pointer = axsl_alloc(bytes);
    if (pointer != NULL_PTR)
        axsl_zero(pointer, bytes);
    return pointer;
}

bool_t axsl_free(void *pointer)
{
    axsl_block_header_t *header;
    axsl_slab_t *slab;
    u8 class_index;
    u16 slot;

    if (pointer == NULL_PTR)
        return false;
    header = axsl_header(pointer);
    if (header->magic != AXSL_MAGIC)
        return false;
    class_index = header->class_index;
    if (class_index >= AXSL_CLASSES)
        return false;
    if (header->slab_index >= axsl_state.classes[class_index].slab_count)
        return false;
    slab = axsl_state.classes[class_index].slabs[header->slab_index];
    slot = header->block_index;
    if (slot >= slab->capacity)
        return false;
    header->magic = 0;
    header->requested = 0;
    if (slab->in_use > 0)
        slab->in_use--;
    if (slot < slab->first_free)
        slab->first_free = slot;
    if (axsl_state.classes[class_index].active_blocks > 0)
        axsl_state.classes[class_index].active_blocks--;
    axsl_state.classes[class_index].frees++;
    return true;
}

u32 axsl_class_of(u32 bytes)
{
    u8 index = axsl_pick_class(bytes);
    return index == 0xFF ? 0xFFFFFFFFu : (u32)index;
}

u32 axsl_usage(u32 class_index)
{
    if (class_index >= AXSL_CLASSES)
        return 0;
    return axsl_state.classes[class_index].active_blocks;
}

u32 axsl_slab_count(u32 class_index)
{
    if (class_index >= AXSL_CLASSES)
        return 0;
    return axsl_state.classes[class_index].slab_count;
}

void axsl_report(void)
{
    u32 i;
    printk("axsl: pages=%u/%u oom=%u\n", (unsigned)axsl_state.pages_used,
           (unsigned)axsl_state.pages_total, (unsigned)axsl_state.oom_count);
    for (i = 0; i < AXSL_CLASSES; i++)
    {
        const axsl_class_t *class_ref = &axsl_state.classes[i];
        printk("  class %u (%u B): slabs=%u active=%u alloc=%u free=%u refill=%u\n",
               (unsigned)i, (unsigned)class_ref->block_size,
               (unsigned)class_ref->slab_count, (unsigned)class_ref->active_blocks,
               (unsigned)class_ref->allocations, (unsigned)class_ref->frees,
               (unsigned)class_ref->refills);
    }
}

bool_t axsl_selftest(void)
{
    void *a;
    void *b;
    void *c;
    axsl_init();
    if (!axsl_provide_pages(0x40000000ull, 8))
        return false;
    if (axsl_class_of(16) != 0)
        return false;
    if (axsl_class_of(2048) != 7)
        return false;
    if (axsl_class_of(8192) != 0xFFFFFFFFu)
        return false;
    a = axsl_alloc(12);
    b = axsl_alloc(12);
    c = axsl_alloc(100);
    if (a == NULL_PTR || b == NULL_PTR || c == NULL_PTR)
        return false;
    if (a == b)
        return false;
    if (!axsl_free(a))
        return false;
    if (!axsl_free(b))
        return false;
    if (!axsl_free(c))
        return false;
    if (axsl_usage(0) != 0)
        return false;
    if (axsl_slab_count(0) != 1)
        return false;
    if (!axsl_free(a))
        return false;
    return true;
}
