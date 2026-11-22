/* AxiomOS - buffer cache for the block layer.
 *
 * Filesystems read and write through 512-byte buffers.  This cache keeps a
 * small LRU of clean and dirty buffers so that directory scans and inode
 * updates do not hammer the disk.  Write-back is explicit: a dirty buffer is
 * only handed to the device when the caller flushes, which lets the journal
 * order writes correctly.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXBC_MAX_BUFFERS   64
#define AXBC_BLOCK_SIZE    512
#define AXBC_MAX_DEVICES   8
#define AXBC_DIRTY_LIMIT   48

typedef struct axbc_ops
{
    bool_t (*read)(u8 device, u64 block, void *buffer);
    bool_t (*write)(u8 device, u64 block, const void *buffer);
    bool_t (*flush)(u8 device);
} axbc_ops_t;

typedef struct axbc_buffer
{
    u8   data[AXBC_BLOCK_SIZE];
    u64  block;
    u8   device;
    u8   refs;
    u32  stamp;
    bool_t valid;
    bool_t dirty;
    bool_t pinned;
    bool_t writing;
} axbc_buffer_t;

typedef struct axbc_device
{
    u8   index;
    u64  block_count;
    u32  reads;
    u32  writes;
    u32  hits;
    u32  misses;
    bool_t present;
    bool_t readonly;
} axbc_device_t;

typedef struct axbc_cache
{
    axbc_buffer_t buffers[AXBC_MAX_BUFFERS];
    axbc_device_t devices[AXBC_MAX_DEVICES];
    const axbc_ops_t *ops;
    u32  clock;
    u32  dirty_count;
    u32  evictions;
    u32  writebacks;
    u32  errors;
    bool_t ready;
} axbc_cache_t;

static axbc_cache_t axbc_cache;

static void axbc_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axbc_memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

static axbc_buffer_t *axbc_find(u8 device, u64 block)
{
    u32 i;
    for (i = 0; i < AXBC_MAX_BUFFERS; i++)
    {
        axbc_buffer_t *buffer = &axbc_cache.buffers[i];
        if (buffer->valid && buffer->device == device && buffer->block == block)
            return buffer;
    }
    return NULL_PTR;
}

static axbc_buffer_t *axbc_slot(void)
{
    u32 i;
    axbc_buffer_t *best = NULL_PTR;
    for (i = 0; i < AXBC_MAX_BUFFERS; i++)
    {
        axbc_buffer_t *buffer = &axbc_cache.buffers[i];
        if (!buffer->valid && !buffer->pinned)
            return buffer;
    }
    for (i = 0; i < AXBC_MAX_BUFFERS; i++)
    {
        axbc_buffer_t *buffer = &axbc_cache.buffers[i];
        if (buffer->pinned || buffer->refs > 0)
            continue;
        if (buffer->dirty)
            continue;
        if (best == NULL_PTR || buffer->stamp < best->stamp)
            best = buffer;
    }
    if (best != NULL_PTR)
        axbc_cache.evictions++;
    return best;
}

static bool_t axbc_flush(u8 device);
static void axbc_memcpy(void *dst, const void *src, u32 n);
static bool_t axbc_load(axbc_buffer_t *buffer, u8 device, u64 block)
{
    if (axbc_cache.ops == NULL_PTR || axbc_cache.ops->read == NULL_PTR)
        return false;
    if (!axbc_cache.ops->read(device, block, buffer->data))
    {
        axbc_cache.errors++;
        return false;
    }
    buffer->device = device;
    buffer->block = block;
    buffer->valid = true;
    buffer->dirty = false;
    buffer->refs = 0;
    buffer->stamp = axbc_cache.clock++;
    axbc_cache.devices[device].reads++;
    return true;
}

static bool_t axbc_store(axbc_buffer_t *buffer)
{
    if (axbc_cache.ops == NULL_PTR || axbc_cache.ops->write == NULL_PTR)
        return false;
    if (axbc_cache.devices[buffer->device].readonly)
        return false;
    if (!axbc_cache.ops->write(buffer->device, buffer->block, buffer->data))
    {
        axbc_cache.errors++;
        return false;
    }
    axbc_cache.devices[buffer->device].writes++;
    axbc_cache.writebacks++;
    buffer->dirty = false;
    if (axbc_cache.dirty_count > 0)
        axbc_cache.dirty_count--;
    return true;
}

/* -------------------------------------------------------------- public API */

void axbc_init(const axbc_ops_t *ops)
{
    axbc_zero(&axbc_cache, (u32)sizeof(axbc_cache));
    axbc_cache.ops = ops;
    axbc_cache.clock = 1;
    axbc_cache.ready = true;
}

bool_t axbc_register_device(u8 device, u64 blocks, bool_t readonly)
{
    if (device >= AXBC_MAX_DEVICES)
        return false;
    axbc_cache.devices[device].index = device;
    axbc_cache.devices[device].block_count = blocks;
    axbc_cache.devices[device].readonly = readonly;
    axbc_cache.devices[device].present = true;
    return true;
}

u8 *axbc_get(u8 device, u64 block)
{
    axbc_buffer_t *buffer;
    if (!axbc_cache.ready)
        return NULL_PTR;
    if (device >= AXBC_MAX_DEVICES || !axbc_cache.devices[device].present)
        return NULL_PTR;
    buffer = axbc_find(device, block);
    if (buffer != NULL_PTR)
    {
        axbc_cache.devices[device].hits++;
        buffer->stamp = axbc_cache.clock++;
        if (buffer->refs < 255)
            buffer->refs++;
        return buffer->data;
    }
    axbc_cache.devices[device].misses++;
    buffer = axbc_slot();
    if (buffer == NULL_PTR)
        return NULL_PTR;
    if (buffer->valid && buffer->dirty)
    {
        if (!axbc_store(buffer))
            return NULL_PTR;
    }
    if (!axbc_load(buffer, device, block))
        return NULL_PTR;
    buffer->refs = 1;
    return buffer->data;
}

bool_t axbc_mark_dirty(u8 device, u64 block)
{
    axbc_buffer_t *buffer = axbc_find(device, block);
    if (buffer == NULL_PTR)
        return false;
    if (!buffer->dirty)
    {
        if (axbc_cache.dirty_count >= AXBC_DIRTY_LIMIT)
            axbc_flush(device);
        buffer->dirty = true;
        axbc_cache.dirty_count++;
    }
    return true;
}

u8 *axbc_get_dirty(u8 device, u64 block)
{
    u8 *data;
    if (!axbc_mark_dirty(device, block))
    {
        data = axbc_get(device, block);
        if (data == NULL_PTR)
            return NULL_PTR;
        if (!axbc_mark_dirty(device, block))
            return NULL_PTR;
        return data;
    }
    return axbc_get(device, block);
}

void axbc_put(u8 device, u64 block)
{
    axbc_buffer_t *buffer = axbc_find(device, block);
    if (buffer != NULL_PTR && buffer->refs > 0)
        buffer->refs--;
}

bool_t axbc_flush(u8 device)
{
    u32 i;
    u32 written = 0;
    for (i = 0; i < AXBC_MAX_BUFFERS; i++)
    {
        axbc_buffer_t *buffer = &axbc_cache.buffers[i];
        if (!buffer->valid || !buffer->dirty)
            continue;
        if (buffer->device != device && device != 0xFF)
            continue;
        if (!axbc_store(buffer))
            return false;
        written++;
    }
    if (axbc_cache.ops != NULL_PTR && axbc_cache.ops->flush != NULL_PTR)
    {
        if (!axbc_cache.ops->flush(device))
            return false;
    }
    AXIOM_UNUSED_VAR(written);
    return true;
}

bool_t axbc_sync(void)
{
    return axbc_flush(0xFF);
}

void axbc_invalidate(u8 device, u64 block)
{
    axbc_buffer_t *buffer = axbc_find(device, block);
    if (buffer == NULL_PTR)
        return;
    if (buffer->dirty)
        axbc_store(buffer);
    buffer->valid = false;
    buffer->refs = 0;
    buffer->dirty = false;
}

void axbc_invalidate_device(u8 device)
{
    u32 i;
    for (i = 0; i < AXBC_MAX_BUFFERS; i++)
    {
        if (axbc_cache.buffers[i].valid && axbc_cache.buffers[i].device == device)
            axbc_invalidate(device, axbc_cache.buffers[i].block);
    }
}

u32 axbc_dirty(void)
{
    return axbc_cache.dirty_count;
}

u32 axbc_usage(void)
{
    u32 i;
    u32 count = 0;
    for (i = 0; i < AXBC_MAX_BUFFERS; i++)
    {
        if (axbc_cache.buffers[i].valid)
            count++;
    }
    return count;
}

void axbc_stats(u8 device, u32 *hits, u32 *misses, u32 *writes)
{
    if (device >= AXBC_MAX_DEVICES)
        return;
    if (hits != NULL_PTR)
        *hits = axbc_cache.devices[device].hits;
    if (misses != NULL_PTR)
        *misses = axbc_cache.devices[device].misses;
    if (writes != NULL_PTR)
        *writes = axbc_cache.devices[device].writes;
}

void axbc_report(void)
{
    u32 i;
    printk("axbc: buffers=%u dirty=%u evictions=%u writebacks=%u errors=%u\n",
           (unsigned)axbc_usage(), (unsigned)axbc_cache.dirty_count,
           (unsigned)axbc_cache.evictions, (unsigned)axbc_cache.writebacks,
           (unsigned)axbc_cache.errors);
    for (i = 0; i < AXBC_MAX_DEVICES; i++)
    {
        const axbc_device_t *device = &axbc_cache.devices[i];
        if (!device->present)
            continue;
        printk("  dev %u: blocks=%llu hit=%u miss=%u write=%u%s\n",
               (unsigned)device->index, (unsigned long long)device->block_count,
               (unsigned)device->hits, (unsigned)device->misses,
               (unsigned)device->writes, device->readonly ? " ro" : "");
    }
}

/* ------------------------------------------------------------ test backing */

static u8 axbc_test_storage[AXBC_BLOCK_SIZE * 16];

static bool_t axbc_test_read(u8 device, u64 block, void *buffer)
{
    AXIOM_UNUSED_VAR(device);
    if (block >= 16)
        return false;
    axbc_memcpy(buffer, axbc_test_storage + block * AXBC_BLOCK_SIZE, AXBC_BLOCK_SIZE);
    return true;
}

static bool_t axbc_test_write(u8 device, u64 block, const void *buffer)
{
    AXIOM_UNUSED_VAR(device);
    if (block >= 16)
        return false;
    axbc_memcpy(axbc_test_storage + block * AXBC_BLOCK_SIZE, buffer, AXBC_BLOCK_SIZE);
    return true;
}

static bool_t axbc_test_flush(u8 device)
{
    AXIOM_UNUSED_VAR(device);
    return true;
}

bool_t axbc_selftest(void)
{
    static const axbc_ops_t ops = { axbc_test_read, axbc_test_write, axbc_test_flush };
    u8 *data;
    u32 hits = 0;
    u32 misses = 0;
    u32 writes = 0;
    axbc_zero(axbc_test_storage, AXIOM_ARRAY_LEN(axbc_test_storage) * 0);
    axbc_init(&ops);
    if (!axbc_register_device(0, 16, false))
        return false;
    data = axbc_get(0, 3);
    if (data == NULL_PTR)
        return false;
    data[0] = 0x5A;
    if (!axbc_mark_dirty(0, 3))
        return false;
    axbc_put(0, 3);
    if (axbc_dirty() != 1)
        return false;
    if (!axbc_flush(0))
        return false;
    if (axbc_dirty() != 0)
        return false;
    axbc_stats(0, &hits, &misses, &writes);
    if (hits != 0 || misses != 1 || writes != 1)
        return false;
    data = axbc_get(0, 3);
    if (data == NULL_PTR || data[0] != 0x5A)
        return false;
    axbc_invalidate(0, 3);
    if (axbc_usage() != 0)
        return false;
    return true;
}


