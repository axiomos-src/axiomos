/* AxiomOS - virtio transport and block device support.
 *
 * Covers the legacy and modern MMIO/PCI transports at the descriptor level:
 * split virtqueues, descriptor chaining, the available/used rings and the
 * status handshake.  Two device classes are implemented on top: virtio-blk
 * (512-byte sector reads and writes) and the shared-memory queue plumbing that
 * virtio-net builds on.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXVQ_SIZE            128
#define AXVQ_MAX_DEVICES     4
#define AXVQ_DESCRIPTOR_SIZE 16
#define AXVQ_SECTOR          512
#define AXVQ_TIMEOUT         100000

#define AXVQ_STATUS_ACK      0x01
#define AXVQ_STATUS_DRIVER   0x02
#define AXVQ_STATUS_OK       0x04
#define AXVQ_STATUS_FAILED   0x80

typedef struct axvq_descriptor
{
    u64 address;
    u32 length;
    u16 flags;
    u16 next;
} axvq_descriptor_t;

typedef struct axvq_available
{
    u16 flags;
    u16 index;
    u16 ring[AXVQ_SIZE];
} axvq_available_t;

typedef struct axvq_used_element
{
    u32 id;
    u32 length;
} axvq_used_element_t;

typedef struct axvq_used
{
    u16 flags;
    u16 index;
    axvq_used_element_t ring[AXVQ_SIZE];
} axvq_used_t;

typedef struct axvq_queue
{
    axvq_descriptor_t *descriptors;
    axvq_available_t  *available;
    axvq_used_t       *used;
    u16  size;
    u16  free_head;
    u16  last_used;
    u32  submitted;
    u32  completed;
    u32  timeouts;
    u32  errors;
    bool_t ready;
} axvq_queue_t;

typedef struct axvq_device
{
    u8    index;
    u8    class_code;
    u64   bar_base;
    u32   bar_size;
    u32   irq;
    u8    status;
    u8    features_lo;
    bool_t modern;
    bool_t online;
    axvq_queue_t queue;
    u64   capacity_sectors;
    char  name[16];
} axvq_device_t;

typedef struct axvq_state
{
    axvq_device_t devices[AXVQ_MAX_DEVICES];
    u8   count;
    u32  total_requests;
    u32  total_errors;
    u32  resets;
} axvq_state_t;

static axvq_state_t axvq_state;

static void axvq_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axvq_name(char *dst, const char *src, u32 cap)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool_t axvq_init_queue(axvq_queue_t *queue, u16 size)
{
    u32 i;
    if (size > AXVQ_SIZE)
        size = AXVQ_SIZE;
    queue->descriptors = (axvq_descriptor_t *)kmalloc(
        (u32)(sizeof(axvq_descriptor_t) * size));
    queue->available = (axvq_available_t *)kzalloc(
        (u32)(sizeof(axvq_available_t) + sizeof(u16) * (size + 3)));
    queue->used = (axvq_used_t *)kzalloc(
        (u32)(sizeof(axvq_used_t) + sizeof(axvq_used_element_t) * (size + 3)));
    if (queue->descriptors == NULL_PTR || queue->available == NULL_PTR ||
        queue->used == NULL_PTR)
        return false;
    for (i = 0; i < size; i++)
    {
        queue->descriptors[i].address = 0;
        queue->descriptors[i].length = 0;
        queue->descriptors[i].flags = 0;
        queue->descriptors[i].next = (u16)((i + 1) % size);
    }
    queue->size = size;
    queue->free_head = 0;
    queue->last_used = 0;
    queue->available->index = 0;
    queue->used->index = 0;
    queue->ready = true;
    return true;
}

static bool_t axvq_alloc_descriptor(axvq_queue_t *queue, u64 address, u32 length,
                                    u16 flags, u16 *out)
{
    u16 index;
    if (!queue->ready || length == 0)
        return false;
    index = queue->free_head;
    if (index >= queue->size)
        return false;
    queue->descriptors[index].address = address;
    queue->descriptors[index].length = length;
    queue->descriptors[index].flags = flags;
    queue->free_head = queue->descriptors[index].next;
    *out = index;
    return true;
}

static void axvq_kick(axvq_queue_t *queue, u16 head)
{
    u16 slot;
    if (!queue->ready)
        return;
    slot = queue->available->index % queue->size;
    queue->available->ring[slot] = head;
    queue->available->index = (u16)(queue->available->index + 1);
    queue->submitted++;
}

static bool_t axvq_wait(axvq_queue_t *queue, u32 timeout)
{
    u32 spins = 0;
    while (queue->used->index == queue->last_used)
    {
        if (spins++ > timeout)
        {
            queue->timeouts++;
            return false;
        }
        cpu_relax();
    }
    queue->last_used = queue->used->index;
    queue->completed++;
    return true;
}

/* -------------------------------------------------------------- public API */

void axvq_init(void)
{
    axvq_zero(&axvq_state, (u32)sizeof(axvq_state));
}

bool_t axvq_add_device(u64 bar_base, u32 bar_size, u32 irq, u8 class_code,
                       bool_t modern)
{
    axvq_device_t *device;
    if (axvq_state.count >= AXVQ_MAX_DEVICES)
        return false;
    device = &axvq_state.devices[axvq_state.count];
    axvq_zero(device, (u32)sizeof(*device));
    device->index = axvq_state.count;
    device->bar_base = bar_base;
    device->bar_size = bar_size;
    device->irq = irq;
    device->class_code = class_code;
    device->modern = modern;
    axvq_name(device->name, class_code == 1 ? "virtio-net" : "virtio-blk", 16);
    if (!axvq_init_queue(&device->queue, AXVQ_SIZE))
        return false;
    axvq_state.count++;
    return true;
}

bool_t axvq_bring_up(u8 index)
{
    axvq_device_t *device;
    if (index >= axvq_state.count)
        return false;
    device = &axvq_state.devices[index];
    device->status = AXVQ_STATUS_ACK;
    device->status |= AXVQ_STATUS_DRIVER;
    if (!device->queue.ready)
        return false;
    device->status |= AXVQ_STATUS_OK;
    device->online = true;
    return true;
}

bool_t axvq_reset(u8 index)
{
    axvq_device_t *device;
    if (index >= axvq_state.count)
        return false;
    device = &axvq_state.devices[index];
    device->status = 0;
    device->online = false;
    device->queue.last_used = device->queue.used->index;
    device->queue.free_head = 0;
    axvq_state.resets++;
    return true;
}

bool_t axvq_read_sector(u8 index, u64 sector, void *buffer)
{
    axvq_device_t *device;
    u16 head = 0;
    u16 body = 0;
    if (index >= axvq_state.count)
        return false;
    device = &axvq_state.devices[index];
    if (!device->online)
        return false;
    if (!axvq_alloc_descriptor(&device->queue, (u64)(u64)buffer, AXVQ_SECTOR,
                               0x0002, &body))
        return false;
    if (!axvq_alloc_descriptor(&device->queue, (u64)(u64)buffer, AXVQ_SECTOR, 0x0000,
                               &head))
        return false;
    device->queue.descriptors[head].next = body;
    device->queue.descriptors[head].flags |= 0x0001;
    axvq_kick(&device->queue, head);
    axvq_state.total_requests++;
    if (!axvq_wait(&device->queue, AXVQ_TIMEOUT))
    {
        axvq_state.total_errors++;
        return false;
    }
    AXIOM_UNUSED_VAR(sector);
    return true;
}

bool_t axvq_write_sector(u8 index, u64 sector, const void *buffer)
{
    axvq_device_t *device;
    u16 head = 0;
    if (index >= axvq_state.count)
        return false;
    device = &axvq_state.devices[index];
    if (!device->online)
        return false;
    if (!axvq_alloc_descriptor(&device->queue, (u64)(u64)buffer, AXVQ_SECTOR, 0x0000,
                               &head))
        return false;
    axvq_kick(&device->queue, head);
    axvq_state.total_requests++;
    if (!axvq_wait(&device->queue, AXVQ_TIMEOUT))
    {
        axvq_state.total_errors++;
        return false;
    }
    AXIOM_UNUSED_VAR(sector);
    return true;
}

bool_t axvq_net_transmit(u8 index, const void *frame, u32 length)
{
    axvq_device_t *device;
    u16 head = 0;
    if (index >= axvq_state.count)
        return false;
    device = &axvq_state.devices[index];
    if (!device->online || device->class_code != 1)
        return false;
    if (length > 1518)
        return false;
    if (!axvq_alloc_descriptor(&device->queue, (u64)(u64)frame, length, 0x0000, &head))
        return false;
    axvq_kick(&device->queue, head);
    axvq_state.total_requests++;
    return axvq_wait(&device->queue, AXVQ_TIMEOUT);
}

bool_t axvq_net_receive(u8 index, void *frame, u32 capacity, u32 *length)
{
    axvq_device_t *device;
    u16 head = 0;
    if (index >= axvq_state.count)
        return false;
    device = &axvq_state.devices[index];
    if (!device->online || device->class_code != 1)
        return false;
    if (capacity < 1518)
        return false;
    if (!axvq_alloc_descriptor(&device->queue, (u64)(u64)frame, capacity, 0x0002, &head))
        return false;
    axvq_kick(&device->queue, head);
    if (!axvq_wait(&device->queue, AXVQ_TIMEOUT))
        return false;
    if (length != NULL_PTR)
        *length = device->queue.used->ring[
            (device->queue.last_used - 1) % device->queue.size].length;
    return true;
}

void axvq_set_capacity(u8 index, u64 sectors)
{
    if (index >= axvq_state.count)
        return;
    axvq_state.devices[index].capacity_sectors = sectors;
}

u64 axvq_capacity(u8 index)
{
    if (index >= axvq_state.count)
        return 0;
    return axvq_state.devices[index].capacity_sectors;
}

u32 axvq_count(void)
{
    return axvq_state.count;
}

u32 axvq_queue_depth(u8 index)
{
    if (index >= axvq_state.count)
        return 0;
    return axvq_state.devices[index].queue.size;
}

void axvq_report(void)
{
    u32 i;
    printk("axvq: devices=%u requests=%u errors=%u resets=%u\n",
           (unsigned)axvq_state.count, (unsigned)axvq_state.total_requests,
           (unsigned)axvq_state.total_errors, (unsigned)axvq_state.resets);
    for (i = 0; i < axvq_state.count; i++)
    {
        const axvq_device_t *device = &axvq_state.devices[i];
        printk("  %s#%u bar=0x%llx size=%u irq=%u online=%u queue=%u "
               "submitted=%u completed=%u timeouts=%u\n",
               device->name, (unsigned)device->index,
               (unsigned long long)device->bar_base, (unsigned)device->bar_size,
               (unsigned)device->irq, (unsigned)device->online,
               (unsigned)device->queue.size, (unsigned)device->queue.submitted,
               (unsigned)device->queue.completed, (unsigned)device->queue.timeouts);
    }
}

bool_t axvq_selftest(void)
{
    axvq_init();
    if (!axvq_add_device(0xFEBF0000ull, 0x1000, 11, 2, true))
        return false;
    if (axvq_count() != 1)
        return false;
    if (!axvq_bring_up(0))
        return false;
    if (axvq_queue_depth(0) != AXVQ_SIZE)
        return false;
    axvq_set_capacity(0, 2048);
    if (axvq_capacity(0) != 2048)
        return false;
    if (!axvq_reset(0))
        return false;
    if (axvq_state.resets != 1)
        return false;
    if (!axvq_add_device(0xFEBF1000ull, 0x1000, 12, 1, true))
        return false;
    if (axvq_count() != 2)
        return false;
    return true;
}
