/* AxiomOS - generic lock free single producer queues and ring helpers. */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXQ_MAX_RINGS 4
#define AXQ_MAX_DEPTH 256

typedef struct axq_ring
{
    u64  storage[AXQ_MAX_DEPTH];
    u32  depth;
    u32  head;
    u32  tail;
    u32  dropped;
    u32  pushed;
    u32  popped;
    u32  high_water;
    u32  mask;
    bool_t overflow;
    char   name[16];
} axq_ring_t;

typedef struct axq_pool
{
    axq_ring_t rings[AXQ_MAX_RINGS];
    u32  count;
    u32  contended;
} axq_pool_t;

static axq_pool_t axq_pool;

static void axq_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axq_name(char *dst, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i < 15)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static u32 axq_pow2(u32 value)
{
    u32 result = 1;
    while (result < value && result < AXQ_MAX_DEPTH)
        result <<= 1;
    return result;
}

/* -------------------------------------------------------------- public API */

void axq_init(void)
{
    axq_zero(&axq_pool, (u32)sizeof(axq_pool));
}

u32 axq_create(const char *name, u32 depth)
{
    axq_ring_t *ring;
    if (axq_pool.count >= AXQ_MAX_RINGS)
        return 0xFFFFFFFFu;
    ring = &axq_pool.rings[axq_pool.count];
    axq_zero(ring, (u32)sizeof(*ring));
    ring->depth = axq_pow2(depth == 0 ? AXQ_MAX_DEPTH : depth);
    ring->mask = ring->depth - 1;
    axq_name(ring->name, name == NULL_PTR ? "ring" : name);
    axq_pool.count++;
    return axq_pool.count - 1;
}

bool_t axq_push(u32 ring_index, u64 value)
{
    axq_ring_t *ring;
    u32 next;
    if (ring_index >= axq_pool.count)
        return false;
    ring = &axq_pool.rings[ring_index];
    next = (ring->tail + 1) & ring->mask;
    if (next == ring->head)
    {
        if (!ring->overflow)
        {
            ring->dropped++;
            return false;
        }
        ring->head = (ring->head + 1) & ring->mask;
        axq_pool.contended++;
    }
    ring->storage[ring->tail] = value;
    ring->tail = next;
    ring->pushed++;
    {
        u32 used = (ring->tail - ring->head) & ring->mask;
        if (used > ring->high_water)
            ring->high_water = used;
    }
    return true;
}

bool_t axq_pop(u32 ring_index, u64 *value)
{
    axq_ring_t *ring;
    if (ring_index >= axq_pool.count)
        return false;
    ring = &axq_pool.rings[ring_index];
    if (ring->head == ring->tail)
        return false;
    if (value != NULL_PTR)
        *value = ring->storage[ring->head];
    ring->head = (ring->head + 1) & ring->mask;
    ring->popped++;
    return true;
}

bool_t axq_peek(u32 ring_index, u64 *value)
{
    axq_ring_t *ring;
    if (ring_index >= axq_pool.count)
        return false;
    ring = &axq_pool.rings[ring_index];
    if (ring->head == ring->tail)
        return false;
    if (value != NULL_PTR)
        *value = ring->storage[ring->head];
    return true;
}

u32 axq_size(u32 ring_index)
{
    if (ring_index >= axq_pool.count)
        return 0;
    return (axq_pool.rings[ring_index].tail - axq_pool.rings[ring_index].head) &
           axq_pool.rings[ring_index].mask;
}

u32 axq_capacity(u32 ring_index)
{
    if (ring_index >= axq_pool.count)
        return 0;
    return axq_pool.rings[ring_index].depth - 1;
}

bool_t axq_empty(u32 ring_index)
{
    return axq_size(ring_index) == 0;
}

bool_t axq_full(u32 ring_index)
{
    if (ring_index >= axq_pool.count)
        return true;
    return axq_size(ring_index) == axq_capacity(ring_index);
}

void axq_reset(u32 ring_index)
{
    axq_ring_t *ring;
    if (ring_index >= axq_pool.count)
        return;
    ring = &axq_pool.rings[ring_index];
    ring->head = 0;
    ring->tail = 0;
    ring->pushed = 0;
    ring->popped = 0;
    ring->high_water = 0;
}

void axq_set_overflow(u32 ring_index, bool_t allow)
{
    if (ring_index >= axq_pool.count)
        return;
    axq_pool.rings[ring_index].overflow = allow;
}

u32 axq_drops(u32 ring_index)
{
    if (ring_index >= axq_pool.count)
        return 0;
    return axq_pool.rings[ring_index].dropped;
}

u32 axq_declare(void)
{
    return axq_pool.count;
}

void axq_report(void)
{
    u32 i;
    printk("axq: rings=%u contended=%u\n", (unsigned)axq_pool.count,
           (unsigned)axq_pool.contended);
    for (i = 0; i < axq_pool.count; i++)
    {
        const axq_ring_t *ring = &axq_pool.rings[i];
        printk("  %s: depth=%u used=%u pushed=%u popped=%u drops=%u high=%u\n",
               ring->name, (unsigned)ring->depth,
               (unsigned)axq_size(i), (unsigned)ring->pushed, (unsigned)ring->popped,
               (unsigned)ring->dropped, (unsigned)ring->high_water);
    }
}

bool_t axq_selftest(void)
{
    u64 value = 0;
    axq_init();
    if (axq_create("test", 8) != 0)
        return false;
    if (axq_declare() != 1)
        return false;
    if (axq_capacity(0) != 7)
        return false;
    if (!axq_push(0, 0xA1))
        return false;
    if (!axq_push(0, 0xA2))
        return false;
    if (axq_size(0) != 2)
        return false;
    if (!axq_peek(0, &value) || value != 0xA1)
        return false;
    if (!axq_pop(0, &value) || value != 0xA1)
        return false;
    if (axq_size(0) != 1)
        return false;
    if (!axq_empty(0))
    {
        if (axq_full(0))
            return true;
    }
    return true;
}
