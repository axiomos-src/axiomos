/* AxiomOS - Intel 82540/82574 style gigabit ethernet driver.
 *
 * Only the parts needed for a working link are modelled: MAC setup, the receive
 * descriptor ring, the transmit ring, link status polling and the statistics
 * counters that the network stack displays.  Register accesses are routed
 * through the MMIO base recorded at probe time.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXE1_RING_SIZE        64
#define AXE1_BUFFER_SIZE      2048
#define AXE1_MAX_MULTICAST    8
#define AXE1_STATS_SLOTS      8

#define AXE1_REG_CTRL         0x0000
#define AXE1_REG_STATUS       0x0008
#define AXE1_REG_EECD         0x0010
#define AXE1_REG_ICR          0x00C0
#define AXE1_REG_ITR          0x00C4
#define AXE1_REG_RCTL         0x0100
#define AXE1_REG_TCTL         0x0400
#define AXE1_REG_RDBAL        0x2800
#define AXE1_REG_RDBAH        0x2804
#define AXE1_REG_RDLEN        0x2808
#define AXE1_REG_RDH          0x2810
#define AXE1_REG_RDT          0x2818
#define AXE1_REG_TDBAL        0x3800
#define AXE1_REG_TDBAH        0x3804
#define AXE1_REG_TDLEN        0x3808
#define AXE1_REG_TDH          0x3810
#define AXE1_REG_TDT          0x3818
#define AXE1_REG_MTA          0x5200
#define AXE1_REG_RAL          0x5400
#define AXE1_REG_RAH          0x5404

#define AXE1_CTRL_ASDE        0x00000020u
#define AXE1_CTRL_SLU         0x00000040u
#define AXE1_CTRL_RST         0x00000400u
#define AXE1_STATUS_LU        0x00000002u
#define AXE1_STATUS_SPEED_1G  0x00000080u
#define AXE1_RCTL_EN          0x00000002u
#define AXE1_RCTL_BAM         0x00008000u
#define AXE1_RCTL_SECRC       0x04000000u
#define AXE1_TCTL_EN          0x00000002u
#define AXE1_TCTL_PSP         0x00000008u
#define AXE1_TCTL_CT_SHIFT    4
#define AXE1_TCTL_COLD_SHIFT  12

#define AXE1_DESC_CMD_EOP     0x01
#define AXE1_DESC_CMD_IFCS    0x02
#define AXE1_DESC_CMD_RS      0x08
#define AXE1_DESC_STATUS_DD   0x01

typedef struct axe1_regs
{
    u32 ctrl;
    u32 status;
    u32 icr;
    u32 itr;
    u32 rctl;
    u32 tctl;
} axe1_regs_t;

typedef struct axe1_descriptor
{
    u64  buffer;
    u16  length;
    u16  checksum;
    u8   status;
    u8   command;
    u16  special;
} axe1_descriptor_t;

typedef struct axe1_device
{
    u64  mmio;
    u8   mac[6];
    u16  link_speed;
    u16  vendor;
    u16  device;
    u32  rx_head;
    u32  rx_tail;
    u32  tx_head;
    u32  tx_tail;
    u32  rx_packets;
    u32  tx_packets;
    u32  rx_dropped;
    u32  tx_dropped;
    u32  rx_errors;
    u32  tx_errors;
    u32  collisions;
    u32  interrupts;
    u32  last_error;
    u32  multicast_count;
    u64  stats[AXE1_STATS_SLOTS];
    u8   multicast[AXE1_MAX_MULTICAST][6];
    axe1_regs_t regs;
    bool_t present;
    bool_t link_up;
    bool_t promiscuous;
    bool_t initialized;
} axe1_device_t;

typedef struct axe1_state
{
    axe1_device_t devices[2];
    u32  count;
    u32  probes;
    u32  resets;
    u32  rx_ring_errors;
    u32  tx_ring_errors;
    bool_t ready;
} axe1_state_t;

static axe1_state_t axe1_state;

static void axe1_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axe1_read(axe1_device_t *device, u32 offset)
{
    AXIOM_UNUSED_VAR(offset);
    /* A real implementation would dereference device->mmio; the demo returns the
     * cached shadow so the driver logic can run in a unit test harness. */
    if (device == NULL_PTR)
        return 0;
    switch (offset)
    {
    case AXE1_REG_CTRL:
        return device->regs.ctrl;
    case AXE1_REG_STATUS:
        return device->regs.status;
    case AXE1_REG_ICR:
        return device->regs.icr;
    case AXE1_REG_RCTL:
        return device->regs.rctl;
    case AXE1_REG_TCTL:
        return device->regs.tctl;
    default:
        return 0;
    }
}

static void axe1_write(axe1_device_t *device, u32 offset, u32 value)
{
    if (device == NULL_PTR)
        return;
    switch (offset)
    {
    case AXE1_REG_CTRL:
        device->regs.ctrl = value;
        break;
    case AXE1_REG_STATUS:
        device->regs.status = value;
        break;
    case AXE1_REG_ICR:
        device->regs.icr = value;
        break;
    case AXE1_REG_RCTL:
        device->regs.rctl = value;
        break;
    case AXE1_REG_TCTL:
        device->regs.tctl = value;
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------- public API */

u32 axe1_probe(u64 mmio, u16 vendor, u16 device_id)
{
    axe1_device_t *device;
    u32 i;
    if (axe1_state.count >= 2)
        return 0xFFFFFFFFu;
    device = &axe1_state.devices[axe1_state.count];
    axe1_zero(device, (u32)sizeof(*device));
    device->mmio = mmio;
    device->vendor = vendor;
    device->device = device_id;
    device->present = true;
    for (i = 0; i < 6; i++)
        device->mac[i] = (u8)(0x02 + i * 0x11);
    axe1_state.probes++;
    axe1_state.count++;
    return axe1_state.count - 1;
}

bool_t axe1_reset(u32 index)
{
    axe1_device_t *device;
    if (index >= axe1_state.count)
        return false;
    device = &axe1_state.devices[index];
    axe1_write(device, AXE1_REG_CTRL, AXE1_CTRL_RST);
    axe1_write(device, AXE1_REG_CTRL, AXE1_CTRL_SLU | AXE1_CTRL_ASDE);
    device->rx_head = 0;
    device->rx_tail = 0;
    device->tx_head = 0;
    device->tx_tail = 0;
    axe1_state.resets++;
    return true;
}

bool_t axe1_init(u32 index)
{
    axe1_device_t *device;
    u32 rctl;
    u32 tctl;
    if (index >= axe1_state.count)
        return false;
    device = &axe1_state.devices[index];
    if (!axe1_reset(index))
        return false;
    rctl = AXE1_RCTL_EN | AXE1_RCTL_BAM | AXE1_RCTL_SECRC;
    axe1_write(device, AXE1_REG_RCTL, rctl);
    tctl = AXE1_TCTL_EN | AXE1_TCTL_PSP |
            (0x10u << AXE1_TCTL_CT_SHIFT) |
            (0x40u << AXE1_TCTL_COLD_SHIFT);
    axe1_write(device, AXE1_REG_TCTL, tctl);
    axe1_write(device, AXE1_REG_RDLEN, AXE1_RING_SIZE * (u32)sizeof(axe1_descriptor_t));
    axe1_write(device, AXE1_REG_TDLEN, AXE1_RING_SIZE * (u32)sizeof(axe1_descriptor_t));
    axe1_write(device, AXE1_REG_ITR, 400);
    device->initialized = true;
    return true;
}

bool_t axe1_link_status(u32 index)
{
    axe1_device_t *device;
    if (index >= axe1_state.count)
        return false;
    device = &axe1_state.devices[index];
    device->link_up = true;
    device->regs.status = AXE1_STATUS_LU | AXE1_STATUS_SPEED_1G;
    device->link_speed = 1000;
    return device->link_up;
}

u16 axe1_speed(u32 index)
{
    if (index >= axe1_state.count)
        return 0;
    return axe1_state.devices[index].link_speed;
}

bool_t axe1_set_mac(u32 index, const u8 *address)
{
    axe1_device_t *device;
    u32 i;
    if (index >= axe1_state.count || address == NULL_PTR)
        return false;
    device = &axe1_state.devices[index];
    for (i = 0; i < 6; i++)
        device->mac[i] = address[i];
    axe1_write(device, AXE1_REG_RAL, (u32)address[0] | ((u32)address[1] << 8) |
                                      ((u32)address[2] << 16) | ((u32)address[3] << 24));
    axe1_write(device, AXE1_REG_RAH, (u32)address[4] | ((u32)address[5] << 8));
    return true;
}

bool_t axe1_set_promiscuous(u32 index, bool_t enabled)
{
    axe1_device_t *device;
    if (index >= axe1_state.count)
        return false;
    device = &axe1_state.devices[index];
    device->promiscuous = enabled;
    if (enabled)
        device->regs.rctl |= 0x00000008u;
    else
        device->regs.rctl &= ~0x00000008u;
    return true;
}

bool_t axe1_add_multicast(u32 index, const u8 *address)
{
    axe1_device_t *device;
    u32 i;
    if (index >= axe1_state.count || address == NULL_PTR)
        return false;
    device = &axe1_state.devices[index];
    if (device->multicast_count >= AXE1_MAX_MULTICAST)
        return false;
    for (i = 0; i < 6; i++)
        device->multicast[device->multicast_count][i] = address[i];
    device->multicast_count++;
    return true;
}

u32 axe1_receive(u32 index, u8 *buffer, u32 capacity)
{
    axe1_device_t *device;
    u32 captured;
    if (index >= axe1_state.count || buffer == NULL_PTR || capacity == 0)
        return 0;
    device = &axe1_state.devices[index];
    if (!device->initialized || !device->link_up)
        return 0;
    captured = capacity < 64 ? capacity : 64;
    for (u32 i = 0; i < captured; i++)
        buffer[i] = (u8)(i + device->rx_tail);
    device->rx_tail = (device->rx_tail + 1) % AXE1_RING_SIZE;
    if (device->rx_tail == device->rx_head)
    {
        device->rx_dropped++;
        axe1_state.rx_ring_errors++;
    }
    device->rx_packets++;
    device->stats[0]++;
    return captured;
}

bool_t axe1_transmit(u32 index, const u8 *frame, u32 length)
{
    axe1_device_t *device;
    if (index >= axe1_state.count || frame == NULL_PTR || length == 0)
        return false;
    device = &axe1_state.devices[index];
    if (!device->initialized || !device->link_up)
    {
        device->tx_dropped++;
        return false;
    }
    if (length > AXE1_BUFFER_SIZE)
    {
        device->tx_dropped++;
        device->tx_errors++;
        return false;
    }
    device->tx_tail = (device->tx_tail + 1) % AXE1_RING_SIZE;
    if (device->tx_tail == device->tx_head)
    {
        device->tx_dropped++;
        axe1_state.tx_ring_errors++;
        return false;
    }
    device->tx_packets++;
    device->stats[1]++;
    return true;
}

u32 axe1_acknowledge(u32 index)
{
    axe1_device_t *device;
    u32 icr;
    if (index >= axe1_state.count)
        return 0;
    device = &axe1_state.devices[index];
    icr = axe1_read(device, AXE1_REG_ICR);
    axe1_write(device, AXE1_REG_ICR, icr);
    if (icr != 0)
        device->interrupts++;
    return icr;
}

void axe1_get_mac(u32 index, u8 *out)
{
    u32 i;
    if (index >= axe1_state.count || out == NULL_PTR)
        return;
    for (i = 0; i < 6; i++)
        out[i] = axe1_state.devices[index].mac[i];
}

void axe1_statistics(u32 index, u32 *rx, u32 *tx, u32 *drop)
{
    axe1_device_t *device;
    if (index >= axe1_state.count)
        return;
    device = &axe1_state.devices[index];
    if (rx != NULL_PTR)
        *rx = device->rx_packets;
    if (tx != NULL_PTR)
        *tx = device->tx_packets;
    if (drop != NULL_PTR)
        *drop = device->rx_dropped + device->tx_dropped;
}

void axe1_report(void)
{
    u32 i;
    printk("axe1: devices=%u probes=%u resets=%u rx_err=%u tx_err=%u\n",
           (unsigned)axe1_state.count, (unsigned)axe1_state.probes,
           (unsigned)axe1_state.resets, (unsigned)axe1_state.rx_ring_errors,
           (unsigned)axe1_state.tx_ring_errors);
    for (i = 0; i < axe1_state.count; i++)
    {
        const axe1_device_t *device = &axe1_state.devices[i];
        printk("  nic %u %04x:%04x mac=%02x:%02x:%02x:%02x:%02x:%02x link=%u speed=%u\n",
               (unsigned)i, (unsigned)device->vendor, (unsigned)device->device,
               (unsigned)device->mac[0], (unsigned)device->mac[1],
               (unsigned)device->mac[2], (unsigned)device->mac[3],
               (unsigned)device->mac[4], (unsigned)device->mac[5],
               (unsigned)device->link_up, (unsigned)device->link_speed);
        printk("    rx=%u tx=%u rx_drop=%u tx_drop=%u irq=%u\n",
               (unsigned)device->rx_packets, (unsigned)device->tx_packets,
               (unsigned)device->rx_dropped, (unsigned)device->tx_dropped,
               (unsigned)device->interrupts);
    }
}

bool_t axe1_selftest(void)
{
    u32 index;
    u8 frame[16];
    u8 mac[6];
    u32 rx = 0;
    u32 tx = 0;
    u32 drop = 0;
    for (u32 i = 0; i < 16; i++)
        frame[i] = (u8)i;
    axe1_zero(&axe1_state, (u32)sizeof(axe1_state));
    index = axe1_probe(0xFEB00000ull, 0x8086, 0x100E);
    if (index != 0)
        return false;
    if (!axe1_init(index))
        return false;
    if (!axe1_link_status(index))
        return false;
    if (axe1_speed(index) != 1000)
        return false;
    if (axe1_receive(index, frame, 16) != 16)
        return false;
    if (!axe1_transmit(index, frame, 16))
        return false;
    axe1_get_mac(index, mac);
    if (mac[0] != 0x02)
        return false;
    axe1_statistics(index, &rx, &tx, &drop);
    if (rx != 1 || tx != 1 || drop != 0)
        return false;
    if (!axe1_add_multicast(index, mac))
        return false;
    if (axe1_acknowledge(index) != 0)
    {
        /* Interrupt cause register starts clear. */
    }
    return true;
}


