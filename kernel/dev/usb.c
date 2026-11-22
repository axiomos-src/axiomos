/* AxiomOS - USB host controller core.
 *
 * Implements the transport independent part of USB support: device slots,
 * control/bulk/interrupt transfer requests, endpoint state and the port
 * enumeration state machine.  The controller backend (xHCI or EHCI) supplies
 * the actual ring programming through a small ops vtable.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXUS_MAX_PORTS       16
#define AXUS_MAX_DEVICES     16
#define AXUS_MAX_ENDPOINTS   8
#define AXUS_MAX_REQUESTS    32
#define AXUS_DESCRIPTOR_MAX  256

enum axus_speed
{
    AXUS_SPEED_UNKNOWN = 0,
    AXUS_SPEED_LOW,
    AXUS_SPEED_FULL,
    AXUS_SPEED_HIGH,
    AXUS_SPEED_SUPER
};

enum axus_state
{
    AXUS_STATE_EMPTY = 0,
    AXUS_STATE_DISABLED,
    AXUS_STATE_RESETTING,
    AXUS_STATE_ENABLED,
    AXUS_STATE_ADDRESSED,
    AXUS_STATE_CONFIGURED,
    AXUS_STATE_ERROR
};

typedef struct axus_endpoint
{
    u8   address;
    u8   type;       /* 0 control, 1 isochronous, 2 bulk, 3 interrupt */
    u8   max_packet;
    u8   interval;
    u16  transfers;
    u32  bytes;
    bool_t halted;
    bool_t open;
} axus_endpoint_t;

typedef struct axus_device
{
    u8   slot;
    u8   port;
    u8   speed;
    u8   state;
    u8   address;
    u16  vendor_id;
    u16  product_id;
    u8   device_class;
    u8   configurations;
    axus_endpoint_t endpoints[AXUS_MAX_ENDPOINTS];
    u8   endpoint_count;
    u32  errors;
    u32  retries;
    bool_t present;
} axus_device_t;

typedef struct axus_port
{
    u8   index;
    u8   state;      /* 0 empty, 1 connected, 2 enabled, 3 disabled */
    u8   speed;
    u32  changes;
    bool_t power;
} axus_port_t;

typedef struct axus_ops
{
    bool_t (*reset_port)(u8 port);
    bool_t (*submit)(u8 slot, u8 endpoint, void *buffer, u32 length, bool_t in);
} axus_ops_t;

typedef struct axus_controller
{
    axus_ops_t  ops;
    axus_port_t ports[AXUS_MAX_PORTS];
    u8          port_count;
    axus_device_t devices[AXUS_MAX_DEVICES];
    u8          device_count;
    u32         submitted;
    u32         completed;
    u32         timeouts;
    u32         resets;
    bool_t      ready;
} axus_controller_t;

static axus_controller_t axus_controller;

static void axus_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axus_device_t *axus_device(u8 slot)
{
    u32 i;
    for (i = 0; i < AXUS_MAX_DEVICES; i++)
    {
        if (axus_controller.devices[i].present &&
            axus_controller.devices[i].slot == slot)
            return &axus_controller.devices[i];
    }
    return NULL_PTR;
}

static const char *axus_state_name(u8 state)
{
    switch (state)
    {
    case AXUS_STATE_DISABLED: return "disabled";
    case AXUS_STATE_RESETTING: return "resetting";
    case AXUS_STATE_ENABLED: return "enabled";
    case AXUS_STATE_ADDRESSED: return "addressed";
    case AXUS_STATE_CONFIGURED: return "configured";
    case AXUS_STATE_ERROR: return "error";
    default: return "empty";
    }
}

/* -------------------------------------------------------------- public API */

void axus_init(const axus_ops_t *ops, u8 ports)
{
    u8 i;
    axus_zero(&axus_controller, (u32)sizeof(axus_controller));
    if (ops != NULL_PTR)
        axus_controller.ops = *ops;
    if (ports > AXUS_MAX_PORTS)
        ports = AXUS_MAX_PORTS;
    axus_controller.port_count = ports;
    for (i = 0; i < ports; i++)
    {
        axus_controller.ports[i].index = i;
        axus_controller.ports[i].power = true;
    }
    axus_controller.ready = true;
}

u32 axus_port_connect(u8 port)
{
    if (port >= axus_controller.port_count)
        return 0;
    axus_controller.ports[port].state = 1;
    axus_controller.ports[port].changes++;
    return (u32)1 << port;
}

u32 axus_port_disconnect(u8 port)
{
    if (port >= axus_controller.port_count)
        return 0;
    axus_controller.ports[port].state = 0;
    axus_controller.ports[port].changes++;
    return (u32)1 << port;
}

bool_t axus_port_reset(u8 port, u8 speed)
{
    if (port >= axus_controller.port_count)
        return false;
    axus_controller.resets++;
    if (axus_controller.ops.reset_port != NULL_PTR)
    {
        if (!axus_controller.ops.reset_port(port))
            return false;
    }
    axus_controller.ports[port].speed = speed;
    axus_controller.ports[port].state = 2;
    return true;
}

u8 axus_enumerate(u8 port)
{
    axus_device_t *device;
    if (port >= axus_controller.port_count)
        return 0xFF;
    if (axus_controller.ports[port].state != 2)
        return 0xFF;
    if (axus_controller.device_count >= AXUS_MAX_DEVICES)
        return 0xFF;
    device = &axus_controller.devices[axus_controller.device_count];
    axus_zero(device, (u32)sizeof(*device));
    device->slot = (u8)axus_controller.device_count + 1;
    device->port = port;
    device->speed = axus_controller.ports[port].speed;
    device->state = AXUS_STATE_ENABLED;
    device->present = true;
    device->address = (u8)(device->slot + 1);
    axus_controller.device_count++;
    return device->slot;
}

bool_t axus_set_address(u8 slot, u8 address)
{
    axus_device_t *device = axus_device(slot);
    if (device == NULL_PTR)
        return false;
    device->address = address;
    device->state = AXUS_STATE_ADDRESSED;
    return true;
}

bool_t axus_set_configuration(u8 slot, u8 configuration)
{
    axus_device_t *device = axus_device(slot);
    if (device == NULL_PTR || configuration == 0)
        return false;
    device->configurations = configuration;
    device->state = AXUS_STATE_CONFIGURED;
    return true;
}

void axus_set_identity(u8 slot, u16 vendor_id, u16 product_id, u8 device_class)
{
    axus_device_t *device = axus_device(slot);
    if (device == NULL_PTR)
        return;
    device->vendor_id = vendor_id;
    device->product_id = product_id;
    device->device_class = device_class;
}

bool_t axus_open_endpoint(u8 slot, u8 address, u8 type, u8 max_packet, u8 interval)
{
    axus_device_t *device = axus_device(slot);
    axus_endpoint_t *endpoint;
    if (device == NULL_PTR || device->endpoint_count >= AXUS_MAX_ENDPOINTS)
        return false;
    endpoint = &device->endpoints[device->endpoint_count];
    axus_zero(endpoint, (u32)sizeof(*endpoint));
    endpoint->address = address;
    endpoint->type = type;
    endpoint->max_packet = max_packet;
    endpoint->interval = interval;
    endpoint->open = true;
    device->endpoint_count++;
    return true;
}

bool_t axus_transfer(u8 slot, u8 endpoint_address, void *buffer, u32 length,
                     bool_t in)
{
    axus_device_t *device = axus_device(slot);
    axus_endpoint_t *endpoint = NULL_PTR;
    u32 i;
    if (device == NULL_PTR || buffer == NULL_PTR)
        return false;
    for (i = 0; i < device->endpoint_count; i++)
    {
        if (device->endpoints[i].address == endpoint_address)
        {
            endpoint = &device->endpoints[i];
            break;
        }
    }
    if (endpoint == NULL_PTR || endpoint->halted)
        return false;
    if (axus_controller.ops.submit != NULL_PTR)
    {
        if (!axus_controller.ops.submit(device->slot, endpoint_address, buffer,
                                        length, in))
        {
            device->errors++;
            axus_controller.timeouts++;
            return false;
        }
    }
    endpoint->transfers++;
    endpoint->bytes += length;
    axus_controller.submitted++;
    axus_controller.completed++;
    return true;
}

bool_t axus_halt_endpoint(u8 slot, u8 endpoint_address, bool_t halt)
{
    axus_device_t *device = axus_device(slot);
    u32 i;
    if (device == NULL_PTR)
        return false;
    for (i = 0; i < device->endpoint_count; i++)
    {
        if (device->endpoints[i].address == endpoint_address)
        {
            device->endpoints[i].halted = halt;
            return true;
        }
    }
    return false;
}

bool_t axus_remove(u8 slot)
{
    u32 i;
    for (i = 0; i < AXUS_MAX_DEVICES; i++)
    {
        if (axus_controller.devices[i].present && axus_controller.devices[i].slot == slot)
        {
            axus_controller.devices[i].present = false;
            axus_controller.devices[i].state = AXUS_STATE_EMPTY;
            if (axus_controller.device_count > 0)
                axus_controller.device_count--;
            return true;
        }
    }
    return false;
}

u8 axus_device_count(void)
{
    return axus_controller.device_count;
}

u8 axus_device_state(u8 slot)
{
    axus_device_t *device = axus_device(slot);
    return device == NULL_PTR ? AXUS_STATE_EMPTY : device->state;
}

const char *axus_state_text(u8 state)
{
    return axus_state_name(state);
}

void axus_report(void)
{
    u32 i;
    printk("axus: ports=%u devices=%u submitted=%u timeouts=%u resets=%u\n",
           (unsigned)axus_controller.port_count,
           (unsigned)axus_controller.device_count,
           (unsigned)axus_controller.submitted,
           (unsigned)axus_controller.timeouts, (unsigned)axus_controller.resets);
    for (i = 0; i < AXUS_MAX_DEVICES; i++)
    {
        const axus_device_t *device = &axus_controller.devices[i];
        if (!device->present)
            continue;
        printk("  slot %u port=%u speed=%u state=%s vid=0x%x pid=0x%x ep=%u errors=%u\n",
               (unsigned)device->slot, (unsigned)device->port,
               (unsigned)device->speed, axus_state_name(device->state),
               (unsigned)device->vendor_id, (unsigned)device->product_id,
               (unsigned)device->endpoint_count, (unsigned)device->errors);
    }
}

/* ------------------------------------------------------------ test backend */

static u32 axus_test_resets;

static bool_t axus_test_reset(u8 port)
{
    AXIOM_UNUSED_VAR(port);
    axus_test_resets++;
    return true;
}

static bool_t axus_test_submit(u8 slot, u8 endpoint, void *buffer, u32 length,
                               bool_t in)
{
    AXIOM_UNUSED_VAR(slot);
    AXIOM_UNUSED_VAR(endpoint);
    AXIOM_UNUSED_VAR(buffer);
    AXIOM_UNUSED_VAR(length);
    AXIOM_UNUSED_VAR(in);
    return true;
}

bool_t axus_selftest(void)
{
    static const axus_ops_t ops = { axus_test_reset, axus_test_submit };
    u8 slot;
    u8 data[8];
    axus_init(&ops, 4);
    if (axus_controller.port_count != 4)
        return false;
    if (axus_port_connect(2) == 0)
        return false;
    if (!axus_port_reset(2, AXUS_SPEED_HIGH))
        return false;
    if (axus_test_resets != 1)
        return false;
    slot = axus_enumerate(2);
    if (slot == 0xFF)
        return false;
    if (axus_device_state(slot) != AXUS_STATE_ENABLED)
        return false;
    if (!axus_set_address(slot, 9))
        return false;
    if (axus_device_state(slot) != AXUS_STATE_ADDRESSED)
        return false;
    axus_set_identity(slot, 0x046D, 0xC52B, 0x03);
    if (!axus_open_endpoint(slot, 0x81, 3, 8, 1))
        return false;
    if (!axus_transfer(slot, 0x81, data, 8, true))
        return false;
    if (axus_open_endpoint(slot, 0x83, 3, 8, 1))
    {
        if (!axus_transfer(slot, 0x83, data, 8, true))
            return false;
    }
    if (!axus_halt_endpoint(slot, 0x81, true))
        return false;
    if (axus_transfer(slot, 0x81, data, 8, true))
        return false;
    if (!axus_remove(slot))
        return false;
    if (axus_device_count() != 0)
        return false;
    return true;
}
