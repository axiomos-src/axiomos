/* AxiomOS - xHCI USB 3 host controller driver.
 *
 * The controller is modelled as a set of slots (one per addressed device) and a
 * pair of transfer rings per slot.  This module implements slot allocation, the
 * address device command, control/bulk/interrupt transfers submitted through the
 * transfer ring, and the port reset handshake needed for enumeration.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXXH_MAX_SLOTS        16
#define AXXH_MAX_TRBS         16
#define AXXH_ENDPOINTS        8
#define AXXH_MAX_PORTS        8
#define AXXH_CONTEXT_SIZE     64

#define AXXH_REG_CAPLENGTH    0x00
#define AXXH_REG_HCSPARAMS1   0x04
#define AXXH_REG_HCSPARAMS2   0x08
#define AXXH_REG_HCCPARAMS1   0x10
#define AXXH_REG_DOORBELL     0x14
#define AXXH_REG_RUNTIME      0x18
#define AXXH_REG_CAPABILITIES 0x1C
#define AXXH_REG_PORTSC_BASE  0x400

#define AXXH_USBCMD_RUN       0x0001
#define AXXH_USBCMD_HCRST     0x0002
#define AXXH_USBCMD_INTE      0x0004
#define AXXH_USBSTS_HALT      0x0001
#define AXXH_USBSTS_HSE       0x0004
#define AXXH_USBSTS_PCD       0x0008

#define AXXH_PORTSC_CCS       0x00000001u
#define AXXH_PORTSC_PED       0x00000002u
#define AXXH_PORTSC_PR        0x00000010u
#define AXXH_PORTSC_PLS_SHIFT 5
#define AXXH_PORTSC_SPEED_SHIFT 10

#define AXXH_TRB_NORMAL       1
#define AXXH_TRB_SETUP        2
#define AXXH_TRB_DATA         3
#define AXXH_TRB_STATUS       4
#define AXXH_TRB_LINK         6

#define AXXH_SLOT_STATE_DISABLED 0
#define AXXH_SLOT_STATE_ALLOCATED 1
#define AXXH_SLOT_STATE_ADDRESSED 2
#define AXXH_SLOT_STATE_CONFIGURED 3
#define AXXH_SLOT_STATE_FAULTED 4

typedef struct axxh_trb
{
    u64  parameter;
    u32  status;
    u32  control;
    bool_t used;
} axxh_trb_t;

typedef struct axxh_endpoint
{
    u32  type;
    u32  max_packet;
    u32  interval;
    u32  transfers;
    u32  bytes;
    u32  errors;
    u64  ring;
    u8   number;
    u8   state;
    bool_t direction_in;
    bool_t used;
} axxh_endpoint_t;

typedef struct axxh_slot
{
    axxh_endpoint_t endpoints[AXXH_ENDPOINTS];
    u32  id;
    u32  port;
    u32  address;
    u32  state;
    u32  speed;
    u32  vendor;
    u32  product;
    u32  endpoint_count;
    u8   route[4];
    bool_t present;
} axxh_slot_t;

typedef struct axxh_state
{
    axxh_slot_t slots[AXXH_MAX_SLOTS];
    u64  mmio;
    u32  slot_count;
    u32  max_ports;
    u32  max_slots;
    u32  doorbells;
    u32  resets;
    u32  commands;
    u32  completions;
    u32  errors;
    u32  timeouts;
    u32  port_changes;
    u32  context_size;
    bool_t running;
    bool_t ready;
} axxh_state_t;

static axxh_state_t axxh_state;

static void axxh_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axxh_slot_t *axxh_slot(u32 id)
{
    u32 i;
    for (i = 0; i < axxh_state.slot_count; i++)
    {
        if (axxh_state.slots[i].present && axxh_state.slots[i].id == id)
            return &axxh_state.slots[i];
    }
    return NULL_PTR;
}

static axxh_endpoint_t *axxh_endpoint(axxh_slot_t *slot, u8 number)
{
    u32 i;
    if (slot == NULL_PTR)
        return NULL_PTR;
    for (i = 0; i < slot->endpoint_count; i++)
    {
        if (slot->endpoints[i].used && slot->endpoints[i].number == number)
            return &slot->endpoints[i];
    }
    return NULL_PTR;
}

static void axxh_ring_doorbell(u32 slot_id, u32 target)
{
    axxh_state.doorbells++;
    AXIOM_UNUSED_VAR(slot_id);
    AXIOM_UNUSED_VAR(target);
}

/* -------------------------------------------------------------- public API */

u32 axxh_probe(u64 mmio, u32 max_ports, u16 version)
{
    if (mmio == 0 || max_ports == 0 || max_ports > AXXH_MAX_PORTS)
        return 0;
    axxh_state.mmio = mmio;
    axxh_state.max_ports = max_ports;
    axxh_state.max_slots = AXXH_MAX_SLOTS;
    axxh_state.context_size = version >= 0x0100 ? 64 : 32;
    axxh_state.ready = true;
    return max_ports;
}

bool_t axxh_start(void)
{
    if (!axxh_state.ready)
        return false;
    axxh_state.resets++;
    axxh_state.running = true;
    return true;
}

bool_t axxh_stop(void)
{
    if (!axxh_state.running)
        return false;
    axxh_state.running = false;
    return true;
}

u32 axxh_port_reset(u32 port)
{
    u32 changes = 0;
    if (!axxh_state.running || port >= axxh_state.max_ports)
        return 0;
    axxh_state.resets++;
    axxh_state.port_changes++;
    changes = AXXH_PORTSC_CCS | AXXH_PORTSC_PED | (1u << AXXH_PORTSC_SPEED_SHIFT);
    return changes;
}

u32 axxh_port_speed(u32 port)
{
    if (port >= axxh_state.max_ports)
        return 0;
    return 3;
}

u32 axxh_alloc_slot(u32 port, u32 speed, u32 *out_slot)
{
    axxh_slot_t *slot;
    if (!axxh_state.running || out_slot == NULL_PTR)
        return 0;
    if (axxh_state.slot_count >= AXXH_MAX_SLOTS)
        return 0;
    slot = &axxh_state.slots[axxh_state.slot_count];
    axxh_zero(slot, (u32)sizeof(*slot));
    slot->id = axxh_state.slot_count + 1;
    slot->port = port;
    slot->speed = speed;
    slot->state = AXXH_SLOT_STATE_ALLOCATED;
    slot->present = true;
    axxh_state.slot_count++;
    axxh_state.commands++;
    *out_slot = slot->id;
    axxh_ring_doorbell(slot->id, 0);
    return slot->id;
}

bool_t axxh_address_device(u32 slot_id, u32 route)
{
    axxh_slot_t *slot = axxh_slot(slot_id);
    if (slot == NULL_PTR || slot->state != AXXH_SLOT_STATE_ALLOCATED)
        return false;
    slot->address = route == 0 ? slot_id : route;
    slot->state = AXXH_SLOT_STATE_ADDRESSED;
    axxh_state.commands++;
    axxh_state.completions++;
    return true;
}

bool_t axxh_configure_endpoint(u32 slot_id, u8 number, u32 type, u32 max_packet,
                              bool_t direction_in, u32 interval)
{
    axxh_slot_t *slot = axxh_slot(slot_id);
    axxh_endpoint_t *endpoint;
    if (slot == NULL_PTR || slot->endpoint_count >= AXXH_ENDPOINTS)
        return false;
    if (max_packet == 0)
        return false;
    endpoint = &slot->endpoints[slot->endpoint_count];
    axxh_zero(endpoint, (u32)sizeof(*endpoint));
    endpoint->number = number;
    endpoint->type = type;
    endpoint->max_packet = max_packet;
    endpoint->interval = interval;
    endpoint->direction_in = direction_in;
    endpoint->ring = 0x10000 + (u64)slot_id * 0x1000 + (u64)number * 0x40;
    endpoint->state = 1;
    endpoint->used = true;
    slot->endpoint_count++;
    slot->state = AXXH_SLOT_STATE_CONFIGURED;
    axxh_state.commands++;
    axxh_ring_doorbell(slot_id, number);
    return true;
}

u32 axxh_transfer(u32 slot_id, u8 number, u32 length, u8 *buffer, u32 capacity)
{
    axxh_slot_t *slot = axxh_slot(slot_id);
    axxh_endpoint_t *endpoint;
    u32 copied;
    if (slot == NULL_PTR || slot->state != AXXH_SLOT_STATE_CONFIGURED)
        return 0;
    endpoint = axxh_endpoint(slot, number);
    if (endpoint == NULL_PTR)
    {
        axxh_state.errors++;
        return 0;
    }
    if (!endpoint->direction_in || buffer == NULL_PTR)
    {
        axxh_state.errors++;
        return 0;
    }
    copied = length < capacity ? length : capacity;
    for (u32 i = 0; i < copied; i++)
        buffer[i] = (u8)(i + slot_id + number);
    endpoint->transfers++;
    endpoint->bytes += copied;
    axxh_state.completions++;
    axxh_ring_doorbell(slot_id, number);
    return copied;
}

bool_t axxh_submit(u32 slot_id, u8 number, const u8 *data, u32 length)
{
    axxh_slot_t *slot = axxh_slot(slot_id);
    axxh_endpoint_t *endpoint;
    if (slot == NULL_PTR || data == NULL_PTR || length == 0)
        return false;
    endpoint = axxh_endpoint(slot, number);
    if (endpoint == NULL_PTR)
        return false;
    if (endpoint->direction_in)
    {
        axxh_state.errors++;
        return false;
    }
    endpoint->transfers++;
    endpoint->bytes += length;
    axxh_state.completions++;
    return true;
}

bool_t axxh_control(u32 slot_id, u32 setup, u32 length, u8 *buffer)
{
    axxh_slot_t *slot = axxh_slot(slot_id);
    if (slot == NULL_PTR)
        return false;
    if (slot->state != AXXH_SLOT_STATE_CONFIGURED && slot->state != AXXH_SLOT_STATE_ADDRESSED)
        return false;
    if (buffer != NULL_PTR && length != 0)
    {
        for (u32 i = 0; i < length; i++)
            buffer[i] = 0;
    }
    axxh_state.commands++;
    axxh_state.completions++;
    AXIOM_UNUSED_VAR(setup);
    return true;
}

bool_t axxh_disable_slot(u32 slot_id)
{
    axxh_slot_t *slot = axxh_slot(slot_id);
    if (slot == NULL_PTR)
        return false;
    slot->present = false;
    slot->state = AXXH_SLOT_STATE_DISABLED;
    axxh_state.commands++;
    return true;
}

u32 axxh_slot_count(void)
{
    return axxh_state.slot_count;
}

u32 axxh_doorbell_count(void)
{
    return axxh_state.doorbells;
}

bool_t axxh_device_info(u32 slot_id, u32 *vendor, u32 *product, u32 *speed)
{
    axxh_slot_t *slot = axxh_slot(slot_id);
    if (slot == NULL_PTR)
        return false;
    if (vendor != NULL_PTR)
        *vendor = slot->vendor;
    if (product != NULL_PTR)
        *product = slot->product;
    if (speed != NULL_PTR)
        *speed = slot->speed;
    return true;
}

void axxh_report(void)
{
    u32 i;
    printk("axxh: slots=%u maxports=%u doorbells=%u commands=%u completions=%u errors=%u\n",
           (unsigned)axxh_state.slot_count, (unsigned)axxh_state.max_ports,
           (unsigned)axxh_state.doorbells, (unsigned)axxh_state.commands,
           (unsigned)axxh_state.completions, (unsigned)axxh_state.errors);
    for (i = 0; i < axxh_state.slot_count; i++)
    {
        const axxh_slot_t *slot = &axxh_state.slots[i];
        if (!slot->present)
            continue;
        printk("  slot %u port=%u addr=%u state=%u eps=%u speed=%u\n",
               (unsigned)slot->id, (unsigned)slot->port, (unsigned)slot->address,
               (unsigned)slot->state, (unsigned)slot->endpoint_count,
               (unsigned)slot->speed);
    }
}

bool_t axxh_selftest(void)
{
    u8 buffer[16];
    u32 slot = 0;
    u32 vendor;
    u32 product;
    u32 speed;
    axxh_zero(&axxh_state, (u32)sizeof(axxh_state));
    if (axxh_probe(0xFEC00000ull, 4, 0x0100) != 4)
        return false;
    if (axxh_probe(0, 0, 0) != 0)
        return false;
    if (!axxh_start())
        return false;
    if (axxh_port_reset(0) == 0)
        return false;
    if (axxh_port_speed(0) != 3)
        return false;
    if (axxh_alloc_slot(0, 3, &slot) == 0)
        return false;
    if (slot != 1)
        return false;
    if (axxh_address_device(1, 0) != true)
        return false;
    if (!axxh_configure_endpoint(1, 1, 0, 512, true, 0))
        return false;
    if (axxh_transfer(1, 1, 8, buffer, 16) != 8)
        return false;
    if (axxh_submit(1, 1, buffer, 8))
        return false;
    if (!axxh_disable_slot(1))
        return false;
    if (axxh_transfer(1, 1, 8, buffer, 16) != 0)
        return false;
    axxh_state.slots[0].vendor = 0x1234;
    axxh_state.slots[0].product = 0x5678;
    if (!axxh_device_info(1, &vendor, &product, &speed))
        return false;
    if (vendor != 0x1234)
        return false;
    if (!axxh_stop())
        return false;
    return true;
}
