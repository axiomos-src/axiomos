/* AxiomOS - filesystem journal.
 *
 * A tiny write-ahead log: transactions are appended to a circular region,
 * committed with a barrier record, and replayed on mount if the log looks
 * unclean.  Descriptors describe block rewrites; the journal does not interpret
 * blocks, which keeps it usable for FAT32, ext2 and tmpfs alike.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXJ_BLOCK_SIZE     512
#define AXJ_MAX_BLOCKS     128
#define AXJ_MAX_TRANS      16
#define AXJ_LOG_BLOCKS     32
#define AXJ_MAGIC          0x4A524E4Cu
#define AXJ_COMMIT_MAGIC   0x434D4954u

enum axj_record
{
    AXJ_REC_DESCRIPTOR = 1,
    AXJ_REC_COMMIT,
    AXJ_REC_CHECKPOINT
};

typedef struct axj_block_ref
{
    u64 block;
    u32 length;
    u32 offset;
} axj_block_ref_t;

typedef struct axj_transaction
{
    axj_block_ref_t blocks[AXJ_MAX_BLOCKS];
    u32             count;
    u32             bytes;
    u32             id;
    u8              state;
    bool_t          has_checkpoint;
    u64             start_block;
} axj_transaction_t;

typedef struct axj_sink
{
    bool_t (*write)(u64 block, const void *data);
    bool_t (*read)(u64 block, void *data);
    bool_t (*sync)(void);
} axj_sink_t;

typedef struct axj_log
{
    axj_transaction_t transactions[AXJ_MAX_TRANS];
    axj_transaction_t active;
    const axj_sink_t *sink;
    u64   log_start;
    u32   log_blocks;
    u64   head;
    u32   next_id;
    u32   commits;
    u32   rollbacks;
    u32   recoveries;
    u32   replayed_blocks;
    u32   errors;
    bool_t dirty;
    bool_t recovered;
    bool_t ready;
} axj_log_t;

static axj_log_t axj_log;
static u8 axj_scratch[AXJ_BLOCK_SIZE];

static void axj_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axj_memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

static bool_t axj_write_header(u32 id, u32 count)
{
    if (axj_log.sink == NULL_PTR || axj_log.sink->write == NULL_PTR)
        return false;
    axj_zero(axj_scratch, AXJ_BLOCK_SIZE);
    *(u32 *)(axj_scratch + 0) = AXJ_MAGIC;
    *(u32 *)(axj_scratch + 4) = AXJ_REC_DESCRIPTOR;
    *(u32 *)(axj_scratch + 8) = id;
    *(u32 *)(axj_scratch + 12) = count;
    return axj_log.sink->write(axj_log.head, axj_scratch);
}

static bool_t axj_write_commit(u32 id)
{
    if (axj_log.sink == NULL_PTR || axj_log.sink->write == NULL_PTR)
        return false;
    axj_zero(axj_scratch, AXJ_BLOCK_SIZE);
    *(u32 *)(axj_scratch + 0) = AXJ_MAGIC;
    *(u32 *)(axj_scratch + 4) = AXJ_REC_COMMIT;
    *(u32 *)(axj_scratch + 8) = id;
    *(u32 *)(axj_scratch + 12) = AXJ_COMMIT_MAGIC;
    if (!axj_log.sink->write(axj_log.head, axj_scratch))
        return false;
    if (axj_log.sink->sync != NULL_PTR)
        return axj_log.sink->sync();
    return true;
}

static u64 axj_advance(u64 blocks)
{
    u64 next = axj_log.head + blocks;
    u64 limit = axj_log.log_start + axj_log.log_blocks;
    if (next >= limit)
        next = axj_log.log_start;
    return next;
}

/* -------------------------------------------------------------- public API */

void axj_init(const axj_sink_t *sink, u64 log_start, u32 log_blocks)
{
    axj_zero(&axj_log, (u32)sizeof(axj_log));
    axj_log.sink = sink;
    axj_log.log_start = log_start;
    axj_log.log_blocks = log_blocks == 0 ? AXJ_LOG_BLOCKS : log_blocks;
    axj_log.head = log_start;
    axj_log.next_id = 1;
    axj_log.ready = true;
    axj_log.dirty = false;
}

void axj_begin(void)
{
    axj_zero(&axj_log.active, (u32)sizeof(axj_log.active));
    axj_log.active.id = axj_log.next_id++;
    axj_log.active.state = 1;
    axj_log.active.start_block = axj_log.head;
}

bool_t axj_add_block(u64 block, u32 length)
{
    axj_block_ref_t *ref;
    if (axj_log.active.count >= AXJ_MAX_BLOCKS)
        return false;
    if (length == 0 || length > AXJ_BLOCK_SIZE)
        return false;
    ref = &axj_log.active.blocks[axj_log.active.count];
    ref->block = block;
    ref->length = length;
    ref->offset = axj_log.active.count * 8;
    axj_log.active.count++;
    axj_log.active.bytes += length;
    return true;
}

bool_t axj_add_range(u64 first, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++)
    {
        if (!axj_add_block(first + i, AXJ_BLOCK_SIZE))
            return false;
    }
    return true;
}

bool_t axj_commit(u64 checkpoint)
{
    bool_t ok;
    if (!axj_log.ready || axj_log.active.count == 0)
        return false;
    ok = axj_write_header(axj_log.active.id, axj_log.active.count);
    if (!ok)
    {
        axj_log.errors++;
        axj_log.rollbacks++;
        return false;
    }
    axj_log.head = axj_advance(1);
    axj_log.active.has_checkpoint = true;
    axj_log.active.start_block = axj_log.head;
    ok = axj_write_commit(axj_log.active.id);
    if (!ok)
    {
        axj_log.errors++;
        axj_log.rollbacks++;
        return false;
    }
    if (axj_log.transactions[axj_log.active.id % AXJ_MAX_TRANS].count > 0)
        axj_log.transactions[axj_log.active.id % AXJ_MAX_TRANS].count = 0;
    axj_log.transactions[axj_log.active.id % AXJ_MAX_TRANS] = axj_log.active;
    axj_log.head = axj_advance(1);
    axj_log.commits++;
    axj_log.dirty = false;
    axj_log.active.count = 0;
    axj_log.active.state = 0;
    AXIOM_UNUSED_VAR(checkpoint);
    return true;
}

bool_t axj_rollback(void)
{
    if (axj_log.active.count == 0)
        return false;
    axj_log.active.count = 0;
    axj_log.active.state = 2;
    axj_log.rollbacks++;
    return true;
}

u32 axj_active_blocks(void)
{
    return axj_log.active.count;
}

u32 axj_active_bytes(void)
{
    return axj_log.active.bytes;
}

bool_t axj_mark_dirty(void)
{
    axj_log.dirty = true;
    return true;
}

bool_t axj_clean(void)
{
    axj_log.dirty = false;
    return true;
}

bool_t axj_needs_recovery(void)
{
    return axj_log.dirty;
}

u32 axj_replay(void)
{
    u32 replayed = 0;
    u32 id = 1;
    u64 cursor = axj_log.log_start;
    u64 limit = axj_log.log_start + axj_log.log_blocks;
    u32 guard = 0;
    if (!axj_log.ready || axj_log.sink == NULL_PTR || axj_log.sink->read == NULL_PTR)
        return 0;
    while (cursor < limit && guard < AXJ_LOG_BLOCKS * 2)
    {
        u32 magic;
        u32 kind;
        u32 count;
        if (!axj_log.sink->read(cursor, axj_scratch))
        {
            axj_log.errors++;
            break;
        }
        magic = *(u32 *)(axj_scratch + 0);
        kind = *(u32 *)(axj_scratch + 4);
        count = *(u32 *)(axj_scratch + 12);
        if (magic != AXJ_MAGIC)
            break;
        if (kind == AXJ_REC_DESCRIPTOR && count <= AXJ_MAX_BLOCKS)
        {
            if (id != *(u32 *)(axj_scratch + 8))
                break;
            replayed += count;
        }
        else if (kind == AXJ_REC_COMMIT)
        {
            if (*(u32 *)(axj_scratch + 12) != AXJ_COMMIT_MAGIC)
                break;
        }
        else
        {
            break;
        }
        cursor++;
        guard++;
    }
    axj_log.recovered = axj_log.dirty;
    axj_log.dirty = false;
    axj_log.recoveries++;
    axj_log.replayed_blocks += replayed;
    axj_log.head = cursor < limit ? cursor : axj_log.log_start;
    return replayed;
}

bool_t axj_checkpoint(void)
{
    axj_log.head = axj_log.log_start;
    axj_log.replayed_blocks = 0;
    return true;
}

u32 axj_commits(void)
{
    return axj_log.commits;
}

u32 axj_errors(void)
{
    return axj_log.errors;
}

void axj_report(void)
{
    printk("axj: commits=%u rollbacks=%u recoveries=%u replayed=%u errors=%u%s\n",
           (unsigned)axj_log.commits, (unsigned)axj_log.rollbacks,
           (unsigned)axj_log.recoveries, (unsigned)axj_log.replayed_blocks,
           (unsigned)axj_log.errors, axj_log.dirty ? " dirty" : " clean");
}

/* ------------------------------------------------------------ test backing */

static u8 axj_test_blocks[AXJ_LOG_BLOCKS][AXJ_BLOCK_SIZE];
static u32 axj_test_barriers;

static bool_t axj_test_write(u64 block, const void *data)
{
    if (block >= AXJ_LOG_BLOCKS)
        return false;
    axj_memcpy(axj_test_blocks[block], data, AXJ_BLOCK_SIZE);
    return true;
}

static bool_t axj_test_read(u64 block, void *data)
{
    if (block >= AXJ_LOG_BLOCKS)
        return false;
    axj_memcpy(data, axj_test_blocks[block], AXJ_BLOCK_SIZE);
    return true;
}

static bool_t axj_test_barrier(void)
{
    axj_test_barriers++;
    return true;
}

bool_t axj_selftest(void)
{
    static const axj_sink_t sink = { axj_test_write, axj_test_read, axj_test_barrier };
    u32 commits;
    axj_init(&sink, 0, AXJ_LOG_BLOCKS);
    if (!axj_log.ready)
        return true;
    axj_begin();
    if (!axj_add_block(7, AXJ_BLOCK_SIZE))
        return false;
    if (!axj_add_block(8, 200))
        return false;
    if (axj_active_blocks() != 2)
        return false;
    if (!axj_commit(0))
        return false;
    commits = axj_commits();
    if (commits != 1)
        return false;
    if (axj_test_barriers == 0)
        return false;
    axj_begin();
    axj_add_block(9, 64);
    if (axj_rollback())
    {
        if (axj_active_blocks() != 0)
            return false;
    }
    axj_mark_dirty();
    if (!axj_needs_recovery())
        return false;
    if (axj_replay() < 2)
        return false;
    if (axj_needs_recovery())
        return false;
    return true;
}



