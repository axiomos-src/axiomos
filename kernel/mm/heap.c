/* AxiomOS - kernel heap.
 *
 * A boundary-tagged first-fit allocator with an address-ordered free list.
 * Address ordering makes coalescing trivial: on free, the block is inserted
 * between its neighbours and both adjacent blocks are merged when contiguous.
 *
 * Block layout (16-byte header, payload follows):
 *
 *   +0  size   total block size in bytes including the header (16-aligned)
 *   +8  flags  low bit = in use, remaining bits hold a magic value so that
 *              a double free or a corrupted header is caught early
 *
 * The minimum block size is 32 bytes: 16 bytes header plus enough payload to
 * store the free-list link and still keep 16-byte alignment.
 */

#include <axiom/kernel.h>

#define HEAP_MAGIC          0xA5C0FFEE00000000ULL
#define HEAP_FLAG_USED      0x1ULL
#define HEAP_MIN_BLOCK      32ULL
#define HEAP_ALIGN          16ULL

struct heap_block {
    u64 size;
    u64 flags;
};

struct heap {
    u64             base;
    u64             size;
    struct heap_block *free_list;   /* address ordered */
    u64             used_bytes;
    u64             alloc_count;
    u64             free_count;
    u64             peak_bytes;
    bool_t          ready;
};

static struct heap g_heap;

bool_t heap_extend(u64 base, u64 size);

AXIOM_INLINE u64 block_size(struct heap_block *b)
{
    return b->size;
}

AXIOM_INLINE bool_t block_used(struct heap_block *b)
{
    return (b->flags & HEAP_FLAG_USED) != 0;
}

AXIOM_INLINE void block_mark_used(struct heap_block *b, bool_t used)
{
    b->flags = (b->flags & ~HEAP_FLAG_USED) |
               HEAP_MAGIC | (used ? HEAP_FLAG_USED : 0);
}

AXIOM_INLINE bool_t block_magic_ok(struct heap_block *b)
{
    return (b->flags & 0xFFFFFFFFFFFF0000ULL) == (HEAP_MAGIC & 0xFFFFFFFFFFFF0000ULL);
}

AXIOM_INLINE struct heap_block *block_next(struct heap_block *b)
{
    u64 addr = (u64)b + b->size;
    if (addr >= g_heap.base + g_heap.size)
        return NULL_PTR;
    return (struct heap_block *)addr;
}

/* Free-list link is stored in the payload of a free block. */
AXIOM_INLINE struct heap_block **block_link(struct heap_block *b)
{
    return (struct heap_block **)((u64)b + sizeof(struct heap_block));
}

AXIOM_INLINE struct heap_block *block_link_value(struct heap_block *b)
{
    return *block_link(b);
}

AXIOM_INLINE void block_set_link(struct heap_block *b, struct heap_block *next)
{
    *block_link(b) = next;
}

void heap_init(u64 base, u64 size)
{
    struct heap_block *initial;

    base = AXIOM_PAGE_ALIGN(base);
    size &= AXIOM_PAGE_MASK;
    if (!base || size < HEAP_MIN_BLOCK * 8) {
        printk("heap: region too small (%u bytes)\n", size);
        return;
    }

    g_heap.base = base;
    g_heap.size = size;
    g_heap.free_list = (struct heap_block *)base;
    g_heap.used_bytes = 0;
    g_heap.alloc_count = 0;
    g_heap.free_count = 0;
    g_heap.peak_bytes = 0;

    initial = (struct heap_block *)base;
    initial->size = size;
    block_mark_used(initial, false);
    block_set_link(initial, NULL_PTR);
    g_heap.ready = true;

    printk("heap: %u KiB at %p\n", size / 1024, (void *)base);
}

/* Insert into the address-ordered free list and coalesce with neighbours. */
static void heap_insert_free(struct heap_block *block)
{
    struct heap_block *prev = NULL_PTR;
    struct heap_block *cur = g_heap.free_list;

    while (cur && (u64)cur < (u64)block) {
        prev = cur;
        cur = block_link_value(cur);
    }

    block_set_link(block, cur);
    if (prev)
        block_set_link(prev, block);
    else
        g_heap.free_list = block;

    /* Merge with the following block. */
    if (cur && (u64)block + block->size == (u64)cur) {
        block->size += cur->size;
        block_set_link(block, block_link_value(cur));
    }

    /* Merge with the preceding block. */
    if (prev && (u64)prev + prev->size == (u64)block) {
        prev->size += block->size;
        block_set_link(prev, block_link_value(block));
        block = prev;
    }
}

static struct heap_block *heap_find_free(u64 size)
{
    struct heap_block *cur = g_heap.free_list;
    while (cur) {
        if (cur->size >= size)
            return cur;
        cur = block_link_value(cur);
    }
    return NULL_PTR;
}

/* Add another physically contiguous segment to the heap.  Used at boot to
 * assemble a larger heap from several allocations. */
bool_t heap_extend(u64 base, u64 size)
{
    struct heap_block *block;

    base = AXIOM_PAGE_ALIGN(base);
    size &= AXIOM_PAGE_MASK;
    if (!g_heap.ready || !base || size < HEAP_MIN_BLOCK * 4)
        return false;
    if (base + size > g_heap.base && base < g_heap.base + g_heap.size) {
        printk("heap: refusing to extend into the existing heap\n");
        return false;
    }

    block = (struct heap_block *)base;
    block->size = size;
    block_mark_used(block, false);
    block_set_link(block, NULL_PTR);
    g_heap.size += size;
    heap_insert_free(block);
    printk("heap: extended by %u KiB at %p (total %u KiB)\n", size / 1024,
           (void *)base, g_heap.size / 1024);
    return true;
}

void *kmalloc(u64 size)
{
    struct heap_block *block;
    u64 needed;

    if (!g_heap.ready || !size)
        return NULL_PTR;

    needed = AXIOM_ALIGN_UP(size + sizeof(struct heap_block), HEAP_ALIGN);
    if (needed < HEAP_MIN_BLOCK)
        needed = HEAP_MIN_BLOCK;

    block = heap_find_free(needed);
    if (!block) {
        printk("heap: out of memory allocating %u bytes (used %u KiB)\n",
               size, g_heap.used_bytes / 1024);
        return NULL_PTR;
    }

    /* Remove from the free list. */
    {
        struct heap_block *prev = NULL_PTR;
        struct heap_block *cur = g_heap.free_list;
        while (cur && cur != block) {
            prev = cur;
            cur = block_link_value(cur);
        }
        if (prev)
            block_set_link(prev, block_link_value(block));
        else
            g_heap.free_list = block_link_value(block);
    }

    /* Split if the remainder can form a valid block. */
    if (block->size >= needed + HEAP_MIN_BLOCK) {
        struct heap_block *rest =
            (struct heap_block *)((u64)block + needed);
        rest->size = block->size - needed;
        block_mark_used(rest, false);
        block_set_link(rest, NULL_PTR);
        block->size = needed;
        heap_insert_free(rest);
    }

    block_mark_used(block, true);
    g_heap.used_bytes += block->size;
    g_heap.alloc_count++;
    if (g_heap.used_bytes > g_heap.peak_bytes)
        g_heap.peak_bytes = g_heap.used_bytes;

    return (void *)((u64)block + sizeof(struct heap_block));
}

void *kzalloc(u64 size)
{
    void *ptr = kmalloc(size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}

void *kmalloc_aligned(u64 size, u64 align)
{
    void *raw;
    struct heap_block *raw_block;
    u64 addr;
    u64 gap;

    if (align <= HEAP_ALIGN || !size)
        return kmalloc(size);
    if (align & (align - 1))
        return NULL_PTR;            /* alignment must be a power of two */

    /* Over-allocate by one alignment unit; the leading gap becomes a free
     * block again so nothing is permanently lost. */
    raw = kmalloc(size + align);
    if (!raw)
        return NULL_PTR;

    raw_block = (struct heap_block *)((u64)raw - sizeof(struct heap_block));
    addr = AXIOM_ALIGN_UP((u64)raw, align);
    gap = addr - (u64)raw;

    if (gap < HEAP_MIN_BLOCK) {
        /* Not enough room for a header: give back the slack as part of the
         * payload and accept the original alignment (still 16 bytes). */
        return raw;
    }

    raw_block->size = gap;
    block_mark_used(raw_block, false);
    g_heap.used_bytes -= gap;
    heap_insert_free(raw_block);

    {
        struct heap_block *b = (struct heap_block *)(addr - sizeof(struct heap_block));
        b->size = raw_block->size;  /* placeholder, recomputed below */
        b->size = size + align - gap;
        if (b->size < HEAP_MIN_BLOCK)
            b->size = HEAP_MIN_BLOCK;
        block_mark_used(b, true);
    }
    return (void *)addr;
}

void kfree(void *ptr)
{
    struct heap_block *block;
    if (!ptr || !g_heap.ready)
        return;

    block = (struct heap_block *)((u64)ptr - sizeof(struct heap_block));
    if (!block_magic_ok(block)) {
        printk("heap: kfree on a block without magic at %p (ignored)\n", ptr);
        return;
    }
    if (!block_used(block)) {
        printk("heap: double free of %p detected\n", ptr);
        return;
    }
    if ((u64)block < g_heap.base ||
        (u64)block >= g_heap.base + g_heap.size) {
        printk("heap: kfree of %p outside the heap (ignored)\n", ptr);
        return;
    }
    if (block->size > g_heap.size) {
        printk("heap: corrupt block size at %p\n", ptr);
        return;
    }

    block_mark_used(block, false);
    g_heap.used_bytes -= block->size;
    g_heap.free_count++;
    heap_insert_free(block);
}

void *krealloc(void *ptr, u64 size)
{
    struct heap_block *block;
    void *fresh;
    u64 old_payload;

    if (!ptr)
        return kmalloc(size);
    if (!size) {
        kfree(ptr);
        return NULL_PTR;
    }

    block = (struct heap_block *)((u64)ptr - sizeof(struct heap_block));
    if (!block_magic_ok(block))
        return kmalloc(size);

    old_payload = block->size - sizeof(struct heap_block);
    if (old_payload >= size)
        return ptr;

    fresh = kmalloc(size);
    if (!fresh)
        return NULL_PTR;
    memcpy(fresh, ptr, old_payload);
    kfree(ptr);
    return fresh;
}

u64 heap_used_bytes(void)
{
    return g_heap.used_bytes;
}

u64 heap_free_bytes(void)
{
    return g_heap.size - g_heap.used_bytes;
}

void heap_dump(void)
{
    printk("heap: %u KiB total, %u KiB used, peak %u KiB, %u allocs, %u frees\n",
           g_heap.size / 1024, g_heap.used_bytes / 1024,
           g_heap.peak_bytes / 1024, g_heap.alloc_count, g_heap.free_count);

    u64 largest = 0;
    u32 count = 0;
    struct heap_block *cur = g_heap.free_list;
    while (cur && count < 128) {
        if (cur->size > largest)
            largest = cur->size;
        count++;
        cur = block_link_value(cur);
    }
    printk("heap: %u free blocks, largest %u bytes\n", count, largest);

    /* Walk the entire heap and verify the tag chain. */
    {
        struct heap_block *b = (struct heap_block *)g_heap.base;
        u32 blocks = 0, used = 0, free_blocks = 0;
        while (b && (u64)b < g_heap.base + g_heap.size && blocks < 65536) {
            if (!block_magic_ok(b)) {
                printk("heap: tag chain broken at %p\n", (void *)b);
                break;
            }
            if (b->size < HEAP_MIN_BLOCK ||
                (b->size & (HEAP_ALIGN - 1)) != 0) {
                printk("heap: bad size %u at %p\n", b->size, (void *)b);
                break;
            }
            if (block_used(b))
                used++;
            else
                free_blocks++;
            blocks++;
            b = block_next(b);
        }
        printk("heap: %u blocks (%u used, %u free) verified\n", blocks, used,
               free_blocks);
    }
}
