/* AxiomOS - eBPF ring buffer.
 *
 * Programs write fixed size records into a shared ring; user space reads them
 * back in order.  Producers reserve a slot, fill it, then publish by advancing
 * the write cursor.  If the consumer stalls the producer reports a drop instead
 * of overwriting unconsumed records.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXRB_CAPACITY      512
#define AXRB_RECORD_SIZE   48
#define AXRB_MAX_CONSUMERS  4
#define AXRB_LOST_SAMPLES   1

typedef struct axrb_record
{
    u32 id;
    u32 length;
    u64 timestamp;
    u8  payload[AXRB_RECORD_SIZE - 16];
} axrb_record_t;

typedef struct axrb_consumer
{
    u32  read_cursor;
    u32  dropped;
    u32  records;
    u64  bytes;
    bool_t active;
} axrb_consumer_t;

typedef struct axrb_ring
{
    axrb_record_t records[AXRB_CAPACITY];
    axrb_consumer_t consumers[AXRB_MAX_CONSUMERS];
    u32  write_cursor;
    u32  write_index;
    u32  produced;
    u32  dropped;
    u32  overwritten;
    u32  record_size;
    bool_t ready;
    bool_t overflow_mode;
} axrb_ring_t;

static axrb_ring_t axrb_ring;

static void axrb_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axrb_lag(const axrb_ring_t *ring)
{
    if (ring->write_index >= ring->write_cursor)
        return ring->write_index - ring->write_cursor;
    return AXRB_CAPACITY - ring->write_cursor + ring->write_index;
}

/* -------------------------------------------------------------- public API */

void axrb_init(void)
{
    axrb_zero(&axrb_ring, (u32)sizeof(axrb_ring));
    axrb_ring.record_size = AXRB_RECORD_SIZE;
    axrb_ring.ready = true;
    axrb_ring.overflow_mode = false;
}

u32 axrb_attach(void)
{
    u32 i;
    for (i = 0; i < AXRB_MAX_CONSUMERS; i++)
    {
        if (!axrb_ring.consumers[i].active)
        {
            axrb_zero(&axrb_ring.consumers[i], (u32)sizeof(axrb_ring.consumers[i]));
            axrb_ring.consumers[i].active = true;
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

void axrb_detach(u32 consumer)
{
    if (consumer >= AXRB_MAX_CONSUMERS)
        return;
    axrb_ring.consumers[consumer].active = false;
}

void *axrb_reserve(u32 *slot)
{
    if (!axrb_ring.ready)
        return NULL_PTR;
    if (axrb_lag(&axrb_ring) >= AXRB_CAPACITY)
    {
        if (!axrb_ring.overflow_mode)
        {
            axrb_ring.dropped++;
            return NULL_PTR;
        }
        axrb_ring.write_cursor = (axrb_ring.write_cursor + 1) % AXRB_CAPACITY;
        axrb_ring.overwritten++;
    }
    *slot = axrb_ring.write_index;
    return (void *)&axrb_ring.records[axrb_ring.write_index];
}

void axrb_submit(u32 slot)
{
    axrb_record_t *record;
    if (slot >= AXRB_CAPACITY)
        return;
    record = &axrb_ring.records[slot];
    if (record->id == 0)
        record->id = axrb_ring.produced + 1;
    record->timestamp = rdtsc();
    axrb_ring.produced++;
    axrb_ring.write_index = (axrb_ring.write_index + 1) % AXRB_CAPACITY;
}

bool_t axrb_push(const void *payload, u32 length)
{
    u32 slot;
    axrb_record_t *record = (axrb_record_t *)axrb_reserve(&slot);
    u32 i;
    if (record == NULL_PTR)
        return false;
    if (length > AXRB_RECORD_SIZE - 16)
        length = AXRB_RECORD_SIZE - 16;
    record->length = length;
    for (i = 0; i < length; i++)
        record->payload[i] = ((const u8 *)payload)[i];
    record->id = axrb_ring.produced + 1;
    axrb_ring.produced++;
    axrb_ring.write_index = (axrb_ring.write_index + 1) % AXRB_CAPACITY;
    return true;
}

const axrb_record_t *axrb_peek(u32 consumer, u32 *slot)
{
    axrb_consumer_t *state;
    if (consumer >= AXRB_MAX_CONSUMERS)
        return NULL_PTR;
    state = &axrb_ring.consumers[consumer];
    if (!state->active)
        return NULL_PTR;
    if (state->read_cursor == axrb_ring.write_index)
        return NULL_PTR;
    *slot = state->read_cursor;
    return &axrb_ring.records[state->read_cursor];
}

void axrb_consume(u32 consumer, u32 count)
{
    axrb_consumer_t *state;
    u32 i;
    if (consumer >= AXRB_MAX_CONSUMERS)
        return;
    state = &axrb_ring.consumers[consumer];
    for (i = 0; i < count; i++)
    {
        if (state->read_cursor == axrb_ring.write_index)
            break;
        state->read_cursor = (state->read_cursor + 1) % AXRB_CAPACITY;
        state->records++;
        state->bytes += axrb_ring.records[state->read_cursor].length;
    }
    if (axrb_ring.write_cursor == axrb_ring.write_index && state->dropped > 0)
        state->dropped = 0;
}

u32 axrb_available(u32 consumer)
{
    axrb_consumer_t *state;
    if (consumer >= AXRB_MAX_CONSUMERS)
        return 0;
    state = &axrb_ring.consumers[consumer];
    if (state->read_cursor == axrb_ring.write_index)
        return 0;
    if (axrb_ring.write_index > state->read_cursor)
        return axrb_ring.write_index - state->read_cursor;
    return AXRB_CAPACITY - state->read_cursor + axrb_ring.write_index;
}

void axrb_set_overflow(bool_t allow)
{
    axrb_ring.overflow_mode = allow;
}

u32 axrb_drops(void)
{
    return axrb_ring.dropped;
}

u32 axrb_produced(void)
{
    return axrb_ring.produced;
}

u32 axrb_capacity(void)
{
    return AXRB_CAPACITY;
}

u32 axrb_usage(void)
{
    return axrb_lag(&axrb_ring);
}

void axrb_report(void)
{
    u32 i;
    printk("axrb: capacity=%u produced=%u drops=%u overwritten=%u usage=%u\n",
           (unsigned)AXRB_CAPACITY, (unsigned)axrb_ring.produced,
           (unsigned)axrb_ring.dropped, (unsigned)axrb_ring.overwritten,
           (unsigned)axrb_usage());
    for (i = 0; i < AXRB_MAX_CONSUMERS; i++)
    {
        if (!axrb_ring.consumers[i].active)
            continue;
        printk("  consumer %u: cursor=%u records=%u bytes=%llu available=%u\n",
               (unsigned)i, (unsigned)axrb_ring.consumers[i].read_cursor,
               (unsigned)axrb_ring.consumers[i].records,
               (unsigned long long)axrb_ring.consumers[i].bytes,
               (unsigned)axrb_available(i));
    }
}

bool_t axrb_selftest(void)
{
    u32 consumer;
    u32 slot = 0;
    u32 payload[4];
    axrb_init();
    consumer = axrb_attach();
    if (consumer != 0)
        return false;
    payload[0] = 0xDEADBEEF;
    if (!axrb_push(payload, sizeof(payload)))
        return false;
    if (axrb_available(consumer) != 1)
        return false;
    {
        const axrb_record_t *record = axrb_peek(consumer, &slot);
        if (record == NULL_PTR)
            return false;
        if (record->length != sizeof(payload))
            return false;
        if (*(const u32 *)record->payload != 0xDEADBEEF)
            return false;
    }
    axrb_consume(consumer, 1);
    if (axrb_available(consumer) != 0)
        return false;
    axrb_detach(consumer);
    if (axrb_available(consumer) != 0)
        return false;
    if (axrb_capacity() != AXRB_CAPACITY)
        return false;
    return true;
}
