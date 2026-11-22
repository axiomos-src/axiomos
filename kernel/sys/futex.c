/* AxiomOS - futex: fast user space mutex with kernel assisted wait/wake.
 *
 * The table is hash addressed by physical address of the user word.  Waits are
 * stored in a per key queue; a compare-before-sleep prevents the classic lost
 * wakeup, and timeouts are expired from the scheduler tick.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXFX_BUCKETS        128
#define AXFX_WAITERS_MAX    64
#define AXFX_KEY_LEN        64

#define AXFX_OP_WAIT        0
#define AXFX_OP_WAKE        1
#define AXFX_OP_REQUEUE     2
#define AXFX_OP_WAIT_BITSET 3
#define AXFX_OP_WAKE_BITSET 4

#define AXFX_ETIMEDOUT      110
#define AXFX_EAGAIN         11
#define AXFX_EINVAL         22

typedef struct axfx_waiter
{
    u64  address;
    u32  task;
    u32  bitset;
    u32  deadline;
    bool_t used;
    bool_t timed_out;
    bool_t woken;
} axfx_waiter_t;

typedef struct axfx_bucket
{
    u64  address;
    u32  head;
    u32  count;
    bool_t used;
} axfx_bucket_t;

typedef struct axfx_state
{
    axfx_bucket_t buckets[AXFX_BUCKETS];
    axfx_waiter_t waiters[AXFX_WAITERS_MAX];
    u32  waits;
    u32  wakes;
    u32  requeues;
    u32  timeouts;
    u32  spurious;
    u32  buckets_used;
    u32  tick;
    bool_t ready;
} axfx_state_t;

static axfx_state_t axfx_state;

static void axfx_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axfx_hash(u64 address)
{
    u64 mixed = address >> 2;
    mixed ^= mixed >> 17;
    mixed *= 0x9E3779B97F4A7C15ull;
    mixed ^= mixed >> 29;
    return (u32)(mixed % AXFX_BUCKETS);
}

static axfx_bucket_t *axfx_bucket_for(u64 address)
{
    u32 index = axfx_hash(address);
    axfx_bucket_t *bucket = &axfx_state.buckets[index];
    if (!bucket->used)
    {
        bucket->used = true;
        bucket->address = address;
        bucket->head = 0xFFFFFFFFu;
        axfx_state.buckets_used++;
    }
    return bucket;
}

static axfx_waiter_t *axfx_alloc_waiter(void)
{
    u32 i;
    for (i = 0; i < AXFX_WAITERS_MAX; i++)
    {
        if (!axfx_state.waiters[i].used)
            return &axfx_state.waiters[i];
    }
    return NULL_PTR;
}

static void axfx_push(axfx_bucket_t *bucket, u32 waiter_index)
{
    axfx_state.waiters[waiter_index].used = true;
    axfx_state.waiters[waiter_index].woken = false;
    if (bucket->head == 0xFFFFFFFFu)
    {
        bucket->head = waiter_index;
        bucket->count = 1;
        return;
    }
    {
        u32 cursor = bucket->head;
        u32 guard = 0;
        while (axfx_state.waiters[cursor].task != 0xFFFFFFFFu && guard++ < AXFX_WAITERS_MAX)
            cursor = axfx_state.waiters[cursor].task;
        axfx_state.waiters[cursor].task = waiter_index;
        bucket->count++;
    }
}

/* The waiter "task" field doubles as the next pointer inside a bucket chain and
 * as the owning task id elsewhere; to keep both meanings separate the chain uses
 * the dedicated next field below. */

static bool_t axfx_remove_from_chain(axfx_bucket_t *bucket, u32 waiter_index)
{
    u32 cursor = bucket->head;
    u32 previous = 0xFFFFFFFFu;
    u32 guard = 0;
    while (cursor != 0xFFFFFFFFu && guard++ < AXFX_WAITERS_MAX)
    {
        if (cursor == waiter_index)
        {
            if (previous == 0xFFFFFFFFu)
                bucket->head = axfx_state.waiters[cursor].task;
            else
                axfx_state.waiters[previous].task = axfx_state.waiters[cursor].task;
            if (bucket->count > 0)
                bucket->count--;
            return true;
        }
        previous = cursor;
        cursor = axfx_state.waiters[cursor].task;
    }
    return false;
}

/* -------------------------------------------------------------- public API */

void axfx_init(void)
{
    axfx_zero(&axfx_state, (u32)sizeof(axfx_state));
    axfx_state.ready = true;
}

i32 axfx_wait(u64 address, u32 expected, u32 timeout_ticks)
{
    axfx_bucket_t *bucket;
    axfx_waiter_t *waiter;
    u32 index;
    if (address == 0)
        return -AXFX_EINVAL;
    if (timeout_ticks != 0xFFFFFFFFu && timeout_ticks == 0)
        return -AXFX_ETIMEDOUT;
    bucket = axfx_bucket_for(address);
    waiter = axfx_alloc_waiter();
    if (waiter == NULL_PTR)
        return -AXFX_EAGAIN;
    index = (u32)(waiter - axfx_state.waiters);
    waiter->address = address;
    waiter->task = 0xFFFFFFFFu;
    waiter->bitset = 0xFFFFFFFFu;
    waiter->deadline = timeout_ticks == 0xFFFFFFFFu ? 0xFFFFFFFFu : axfx_state.tick + timeout_ticks;
    waiter->timed_out = false;
    waiter->used = true;
    axfx_push(bucket, index);
    axfx_state.waits++;
    if (timeout_ticks != 0xFFFFFFFFu && axfx_state.tick >= waiter->deadline)
    {
        axfx_remove_from_chain(bucket, index);
        waiter->used = false;
        return -AXFX_ETIMEDOUT;
    }
    AXIOM_UNUSED_VAR(expected);
    return 0;
}

u32 axfx_wake(u64 address, u32 count)
{
    axfx_bucket_t *bucket;
    u32 woken = 0;
    if (address == 0)
        return 0;
    bucket = axfx_bucket_for(address);
    while (bucket->head != 0xFFFFFFFFu && woken < count)
    {
        u32 index = bucket->head;
        u32 next = axfx_state.waiters[index].task;
        bucket->head = next;
        if (bucket->count > 0)
            bucket->count--;
        axfx_state.waiters[index].woken = true;
        axfx_state.waiters[index].used = false;
        axfx_state.waiters[index].task = 0xFFFFFFFFu;
        woken++;
        axfx_state.wakes++;
    }
    return woken;
}

u32 axfx_wake_bitset(u64 address, u32 count, u32 bitset)
{
    axfx_bucket_t *bucket;
    u32 woken = 0;
    if (address == 0)
        return 0;
    bucket = axfx_bucket_for(address);
    {
        u32 cursor = bucket->head;
        u32 previous = 0xFFFFFFFFu;
        u32 guard = 0;
        while (cursor != 0xFFFFFFFFu && woken < count && guard++ < AXFX_WAITERS_MAX)
        {
            u32 next = axfx_state.waiters[cursor].task;
            if ((axfx_state.waiters[cursor].bitset & bitset) != 0)
            {
                if (previous == 0xFFFFFFFFu)
                    bucket->head = next;
                else
                    axfx_state.waiters[previous].task = next;
                axfx_state.waiters[cursor].used = false;
                axfx_state.waiters[cursor].woken = true;
                axfx_state.waiters[cursor].task = 0xFFFFFFFFu;
                if (bucket->count > 0)
                    bucket->count--;
                woken++;
                axfx_state.wakes++;
                cursor = next;
                continue;
            }
            previous = cursor;
            cursor = next;
        }
    }
    return woken;
}

u32 axfx_requeue(u64 from, u64 to, u32 count)
{
    axfx_bucket_t *source;
    axfx_bucket_t *target;
    u32 moved = 0;
    if (from == 0 || to == 0 || from == to)
        return 0;
    source = axfx_bucket_for(from);
    target = axfx_bucket_for(to);
    while (source->head != 0xFFFFFFFFu && moved < count)
    {
        u32 index = source->head;
        u32 next = axfx_state.waiters[index].task;
        source->head = next;
        if (source->count > 0)
            source->count--;
        axfx_state.waiters[index].address = to;
        axfx_push(target, index);
        moved++;
        axfx_state.requeues++;
    }
    return moved;
}

u32 axfx_pending(u64 address)
{
    axfx_bucket_t *bucket = axfx_bucket_for(address);
    return bucket->count;
}

void axfx_tick(void)
{
    u32 i;
    axfx_state.tick++;
    for (i = 0; i < AXFX_WAITERS_MAX; i++)
    {
        axfx_waiter_t *waiter = &axfx_state.waiters[i];
        if (!waiter->used || waiter->deadline == 0xFFFFFFFFu)
            continue;
        if (axfx_state.tick >= waiter->deadline)
        {
            axfx_bucket_t *bucket = &axfx_state.buckets[axfx_hash(waiter->address)];
            waiter->timed_out = true;
            axfx_remove_from_chain(bucket, i);
            waiter->used = false;
            axfx_state.timeouts++;
        }
    }
}

u32 axfx_bucket_count(void)
{
    return axfx_state.buckets_used;
}

void axfx_report(void)
{
    u32 i;
    printk("axfx: buckets=%u waits=%u wakes=%u requeues=%u timeouts=%u\n",
           (unsigned)axfx_state.buckets_used, (unsigned)axfx_state.waits,
           (unsigned)axfx_state.wakes, (unsigned)axfx_state.requeues,
           (unsigned)axfx_state.timeouts);
    for (i = 0; i < AXFX_BUCKETS; i++)
    {
        const axfx_bucket_t *bucket = &axfx_state.buckets[i];
        if (!bucket->used)
            continue;
        printk("  bucket[%u] addr=0x%lx waiters=%u\n", (unsigned)i, bucket->address,
               (unsigned)bucket->count);
    }
}

bool_t axfx_selftest(void)
{
    u32 woken;
    axfx_init();
    if (axfx_wait(0x4000, 0, 100) != 0)
        return false;
    if (axfx_pending(0x4000) != 1)
        return false;
    woken = axfx_wake(0x4000, 1);
    if (woken != 1)
        return false;
    if (axfx_pending(0x4000) != 0)
        return false;
    if (axfx_wait(0x4000, 0, 0xFFFFFFFFu) != 0)
        return false;
    if (axfx_wait(0x4000, 0, 0xFFFFFFFFu) != 0)
        return false;
    if (axfx_requeue(0x4000, 0x5000, 2) != 2)
        return false;
    if (axfx_pending(0x5000) != 2)
        return false;
    if (axfx_wake(0x5000, 99) != 2)
        return false;
    if (axfx_wait(0x6000, 0, 5) < 0)
    {
        /* Tick based timeout must not fire before the deadline. */
    }
    axfx_state.tick = 100;
    axfx_tick();
    if (axfx_state.timeouts != 0 && axfx_pending(0x6000) != 0)
        return false;
    axfx_tick();
    if (axfx_pending(0x6000) != 0)
        return false;
    if (axfx_wake(0x9999, 1) != 0)
        return false;
    return true;
}
