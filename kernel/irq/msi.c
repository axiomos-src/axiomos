/* AxiomOS - MSI / MSI-X interrupt allocation.
 *
 * A message signalled interrupt is described by a destination APIC id, a vector
 * and a delivery mode.  The allocator hands out vectors from a fixed window and
 * keeps the per device table so that masking and migration stay independent.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXMI_MAX_DEVICES     16
#define AXMI_MAX_ENTRIES     32
#define AXMI_VECTOR_FIRST    0x40
#define AXMI_VECTOR_LAST     0xEF
#define AXMI_NO_VECTOR       0xFF

#define AXMI_FORMAT_MSI      0
#define AXMI_FORMAT_MSIX     1

#define AXMI_MODE_FIXED      0
#define AXMI_MODE_LOWEST     1
#define AXMI_MODE_SMI        2
#define AXMI_MODE_NMI        3
#define AXMI_MODE_INIT       4
#define AXMI_MODE_EXTINT     5

typedef struct axmi_entry
{
    u32  address_low;
    u32  address_high;
    u32  data;
    u32  mask_bits;
    u32  pending;
    u8   vector;
    u8   destination;
    u8   delivery;
    u8   format;
    bool_t masked;
    bool_t used;
} axmi_entry_t;

typedef struct axmi_device
{
    u32  bdf;
    u32  entry_base;
    u32  entry_count;
    u32  programmed;
    u32  fired;
    u32  masked_count;
    u8   format;
    u8   capability;
    bool_t used;
    bool_t enabled;
    axmi_entry_t entries[AXMI_MAX_ENTRIES];
} axmi_device_t;

typedef struct axmi_state
{
    axmi_device_t devices[AXMI_MAX_DEVICES];
    u32  device_count;
    u32  vectors_allocated;
    u32  next_vector;
    u32  migrations;
    u32  spurious;
    u64  address_base;
    bool_t ready;
} axmi_state_t;

static axmi_state_t axmi_state;

static void axmi_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u8 axmi_next_vector(void)
{
    u8 vector = (u8)(AXMI_VECTOR_FIRST + axmi_state.next_vector);
    if (vector > AXMI_VECTOR_LAST)
    {
        axmi_state.next_vector = 0;
        vector = AXMI_VECTOR_FIRST;
    }
    axmi_state.next_vector++;
    axmi_state.vectors_allocated++;
    return vector;
}

/* -------------------------------------------------------------- public API */

void axmi_init(u64 address_base)
{
    axmi_zero(&axmi_state, (u32)sizeof(axmi_state));
    axmi_state.address_base = address_base == 0 ? 0xFEE00000ull : address_base;
    axmi_state.ready = true;
}

u32 axmi_add_device(u32 bdf, u8 format, u8 capability, u32 entries)
{
    axmi_device_t *device;
    u32 i;
    if (axmi_state.device_count >= AXMI_MAX_DEVICES)
        return 0xFFFFFFFFu;
    if (entries == 0 || entries > AXMI_MAX_ENTRIES)
        return 0xFFFFFFFFu;
    device = &axmi_state.devices[axmi_state.device_count];
    axmi_zero(device, (u32)sizeof(*device));
    device->bdf = bdf;
    device->format = format;
    device->capability = capability;
    device->entry_count = entries;
    device->used = true;
    for (i = 0; i < entries; i++)
    {
        device->entries[i].vector = axmi_next_vector();
        device->entries[i].destination = 0;
        device->entries[i].delivery = AXMI_MODE_FIXED;
        device->entries[i].format = format;
        device->entries[i].used = true;
        device->entries[i].address_low = (u32)(axmi_state.address_base & 0xFFFFFFFFu);
        device->entries[i].address_high = (u32)(axmi_state.address_base >> 32);
    }
    axmi_state.device_count++;
    return axmi_state.device_count - 1;
}

bool_t axmi_program(u32 device_index, u32 entry, u8 destination, u8 delivery)
{
    axmi_device_t *device;
    axmi_entry_t *target;
    if (device_index >= axmi_state.device_count)
        return false;
    device = &axmi_state.devices[device_index];
    if (entry >= device->entry_count)
        return false;
    target = &device->entries[entry];
    target->destination = destination;
    target->delivery = delivery;
    target->data = ((u32)delivery << 8) | (u32)target->vector;
    device->programmed++;
    return true;
}

bool_t axmi_mask(u32 device_index, u32 entry)
{
    axmi_device_t *device;
    if (device_index >= axmi_state.device_count)
        return false;
    device = &axmi_state.devices[device_index];
    if (entry >= device->entry_count)
        return false;
    if (!device->entries[entry].masked)
    {
        device->entries[entry].masked = true;
        device->masked_count++;
    }
    device->entries[entry].mask_bits |= (1u << (entry % 32));
    return true;
}

bool_t axmi_unmask(u32 device_index, u32 entry)
{
    axmi_device_t *device;
    if (device_index >= axmi_state.device_count)
        return false;
    device = &axmi_state.devices[device_index];
    if (entry >= device->entry_count)
        return false;
    if (device->entries[entry].masked)
    {
        device->entries[entry].masked = false;
        if (device->masked_count > 0)
            device->masked_count--;
    }
    device->entries[entry].mask_bits &= ~(1u << (entry % 32));
    return true;
}

bool_t axmi_enable(u32 device_index, bool_t enable)
{
    if (device_index >= axmi_state.device_count)
        return false;
    axmi_state.devices[device_index].enabled = enable;
    return true;
}

bool_t axmi_migrate(u32 device_index, u8 destination)
{
    axmi_device_t *device;
    u32 i;
    if (device_index >= axmi_state.device_count)
        return false;
    device = &axmi_state.devices[device_index];
    for (i = 0; i < device->entry_count; i++)
    {
        device->entries[i].destination = destination;
        device->entries[i].data = ((u32)device->entries[i].delivery << 8) |
                                  (u32)device->entries[i].vector;
    }
    axmi_state.migrations++;
    return true;
}

u8 axmi_vector(u32 device_index, u32 entry)
{
    if (device_index >= axmi_state.device_count)
        return AXMI_NO_VECTOR;
    if (entry >= axmi_state.devices[device_index].entry_count)
        return AXMI_NO_VECTOR;
    if (axmi_state.devices[device_index].entries[entry].masked)
        return AXMI_NO_VECTOR;
    return axmi_state.devices[device_index].entries[entry].vector;
}

bool_t axmi_fire(u32 device_index, u32 entry)
{
    axmi_device_t *device;
    if (device_index >= axmi_state.device_count)
        return false;
    device = &axmi_state.devices[device_index];
    if (entry >= device->entry_count)
        return false;
    if (!device->enabled)
    {
        axmi_state.spurious++;
        return false;
    }
    if (device->entries[entry].masked)
    {
        device->entries[entry].pending++;
        return false;
    }
    device->fired++;
    return true;
}

u32 axmi_pending(u32 device_index, u32 entry)
{
    if (device_index >= axmi_state.device_count)
        return 0;
    if (entry >= axmi_state.devices[device_index].entry_count)
        return 0;
    return axmi_state.devices[device_index].entries[entry].pending;
}

bool_t axmi_flush_pending(u32 device_index)
{
    axmi_device_t *device;
    u32 i;
    u32 delivered = 0;
    if (device_index >= axmi_state.device_count)
        return false;
    device = &axmi_state.devices[device_index];
    for (i = 0; i < device->entry_count; i++)
    {
        if (device->entries[i].pending != 0 && !device->entries[i].masked)
        {
            delivered += device->entries[i].pending;
            device->entries[i].pending = 0;
        }
    }
    device->fired += delivered;
    return true;
}

void axmi_report(void)
{
    u32 i;
    printk("axmi: devices=%u vectors=%u base=0x%lx migrations=%u spurious=%u\n",
           (unsigned)axmi_state.device_count, (unsigned)axmi_state.vectors_allocated,
           (unsigned long)axmi_state.address_base, (unsigned)axmi_state.migrations,
           (unsigned)axmi_state.spurious);
    for (i = 0; i < axmi_state.device_count; i++)
    {
        const axmi_device_t *device = &axmi_state.devices[i];
        printk("  dev %u bdf=%x fmt=%u entries=%u programmed=%u fired=%u enabled=%u\n",
               (unsigned)i, (unsigned)device->bdf, (unsigned)device->format,
               (unsigned)device->entry_count, (unsigned)device->programmed,
               (unsigned)device->fired, (unsigned)device->enabled);
    }
}

bool_t axmi_selftest(void)
{
    u32 device;
    axmi_init(0);
    device = axmi_add_device(0x0200, AXMI_FORMAT_MSIX, 0x0B, 4);
    if (device != 0)
        return false;
    if (axmi_vector(device, 0) != AXMI_VECTOR_FIRST)
        return false;
    if (!axmi_program(device, 0, 2, AXMI_MODE_FIXED))
        return false;
    if (!axmi_enable(device, true))
        return false;
    if (!axmi_fire(device, 0))
        return false;
    if (!axmi_mask(device, 1))
        return false;
    if (axmi_vector(device, 1) != AXMI_NO_VECTOR)
        return false;
    if (!axmi_fire(device, 1))
        return false;
    if (axmi_pending(device, 1) != 1)
        return false;
    if (!axmi_unmask(device, 1))
        return false;
    if (!axmi_migrate(device, 3))
        return false;
    if (axmi_state.devices[0].entries[0].destination != 3)
        return false;
    if (!axmi_flush_pending(device))
        return false;
    if (axmi_state.devices[0].fired != 2)
        return false;
    return true;
}
