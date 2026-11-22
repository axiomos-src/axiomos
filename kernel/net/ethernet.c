/* AxiomOS - Ethernet framing and neighbour (ARP) resolution.
 *
 * Sits between the link driver (e1000, virtio-net) and the IP layer.  Frames
 * are handed up by the driver as raw buffers; this module validates the header,
 * dispatches by ethertype and keeps a 16 entry ARP cache with an ageing timer.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXET_HEADER        14
#define AXET_MAX_MTU       1500
#define AXET_ARP_CACHE     16
#define AXET_ARP_TTL       64
#define AXET_ETH_IPV4      0x0800
#define AXET_ETH_ARP       0x0806
#define AXET_ETH_IPV6      0x86DD

typedef struct axet_address
{
    u8 bytes[6];
    bool_t valid;
} axet_address_t;

typedef struct axet_header
{
    axet_address_t destination;
    axet_address_t source;
    u16 ethertype;
} axet_header_t;

typedef struct axet_arp_entry
{
    u32 address;
    axet_address_t hardware;
    u16 ttl;
    u32 hits;
    bool_t valid;
} axet_arp_entry_t;

typedef struct axet_frame_stats
{
    u32 received;
    u32 transmitted;
    u32 dropped;
    u32 malformed;
    u32 arp_requests;
    u32 arp_replies;
    u32 arp_misses;
    u32 arp_hits;
} axet_frame_stats_t;

typedef struct axet_interface
{
    axet_address_t hardware;
    u32  ipv4;
    u32  netmask;
    u32  gateway;
    u32  mtu;
    bool_t up;
    bool_t promiscuous;
    axet_arp_entry_t arp[AXET_ARP_CACHE];
    axet_frame_stats_t stats;
    u32  broadcast_count;
    char name[12];
} axet_interface_t;

typedef bool_t (*axet_send_fn)(const u8 *frame, u32 length);

static axet_interface_t axet_interface;
static axet_send_fn axet_send;

static void axet_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axet_name(char *dst, const char *src, u32 cap)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool_t axet_address_equal(const axet_address_t *a, const axet_address_t *b)
{
    u32 i;
    for (i = 0; i < 6; i++)
    {
        if (a->bytes[i] != b->bytes[i])
            return false;
    }
    return true;
}

static void axet_address_copy(axet_address_t *dst, const axet_address_t *src)
{
    u32 i;
    for (i = 0; i < 6; i++)
        dst->bytes[i] = src->bytes[i];
    dst->valid = src->valid;
}

static axet_arp_entry_t *axet_arp_lookup(u32 address)
{
    u32 i;
    for (i = 0; i < AXET_ARP_CACHE; i++)
    {
        if (axet_interface.arp[i].valid && axet_interface.arp[i].address == address)
            return &axet_interface.arp[i];
    }
    return NULL_PTR;
}

static axet_arp_entry_t *axet_arp_slot(void)
{
    u32 i;
    axet_arp_entry_t *best = NULL_PTR;
    for (i = 0; i < AXET_ARP_CACHE; i++)
    {
        axet_arp_entry_t *entry = &axet_interface.arp[i];
        if (!entry->valid)
            return entry;
        if (best == NULL_PTR || entry->ttl < best->ttl)
            best = entry;
    }
    return best;
}

static bool_t axet_is_broadcast(const axet_address_t *address)
{
    u32 i;
    for (i = 0; i < 6; i++)
    {
        if (address->bytes[i] != 0xFF)
            return false;
    }
    return true;
}

/* -------------------------------------------------------------- public API */

void axet_init(const axet_address_t *hardware, u32 ipv4, u32 netmask, u32 gateway,
               axet_send_fn send, const char *name)
{
    axet_zero(&axet_interface, (u32)sizeof(axet_interface));
    if (hardware != NULL_PTR)
        axet_address_copy(&axet_interface.hardware, hardware);
    axet_interface.ipv4 = ipv4;
    axet_interface.netmask = netmask;
    axet_interface.gateway = gateway;
    axet_interface.mtu = AXET_MAX_MTU;
    axet_interface.up = true;
    axet_send = send;
    axet_name(axet_interface.name, name == NULL_PTR ? "eth0" : name, 12);
}

bool_t axet_set_address(const axet_address_t *hardware)
{
    if (hardware == NULL_PTR)
        return false;
    axet_address_copy(&axet_interface.hardware, hardware);
    axet_interface.hardware.valid = true;
    return true;
}

void axet_set_ipv4(u32 address, u32 netmask, u32 gateway)
{
    axet_interface.ipv4 = address;
    axet_interface.netmask = netmask;
    axet_interface.gateway = gateway;
}

bool_t axet_arp_learn(u32 address, const axet_address_t *hardware)
{
    axet_arp_entry_t *entry = axet_arp_lookup(address);
    if (entry == NULL_PTR)
    {
        entry = axet_arp_slot();
        if (entry == NULL_PTR)
            return false;
        axet_zero(entry, (u32)sizeof(*entry));
        entry->address = address;
        entry->valid = true;
        axet_interface.stats.arp_misses++;
    }
    else
    {
        axet_interface.stats.arp_hits++;
    }
    axet_address_copy(&entry->hardware, hardware);
    entry->ttl = AXET_ARP_TTL;
    entry->hits++;
    return true;
}

bool_t axet_arp_resolve(u32 address, axet_address_t *hardware)
{
    axet_arp_entry_t *entry = axet_arp_lookup(address);
    if (entry == NULL_PTR)
        return false;
    axet_address_copy(hardware, &entry->hardware);
    return true;
}

bool_t axet_send_frame(const axet_address_t *destination, u16 ethertype,
                       const u8 *payload, u32 length)
{
    u8 frame[AXET_HEADER + AXET_MAX_MTU];
    u32 total = length + AXET_HEADER;
    u32 i;
    if (destination == NULL_PTR || payload == NULL_PTR)
        return false;
    if (length > axet_interface.mtu)
        return false;
    for (i = 0; i < 6; i++)
    {
        frame[i] = destination->bytes[i];
        frame[6 + i] = axet_interface.hardware.bytes[i];
    }
    frame[12] = (u8)(ethertype & 0xFF);
    frame[13] = (u8)((ethertype >> 8) & 0xFF);
    for (i = 0; i < length; i++)
        frame[AXET_HEADER + i] = payload[i];
    if (axet_send == NULL_PTR)
    {
        axet_interface.stats.dropped++;
        return false;
    }
    if (!axet_send(frame, total))
    {
        axet_interface.stats.dropped++;
        return false;
    }
    axet_interface.stats.transmitted++;
    return true;
}

u16 axet_receive(const u8 *frame, u32 length)
{
    const axet_header_t *header;
    u16 ethertype;
    if (frame == NULL_PTR || length < AXET_HEADER)
    {
        axet_interface.stats.malformed++;
        return 0;
    }
    header = (const axet_header_t *)frame;
    ethertype = header->ethertype;
    if (!axet_is_broadcast(&header->destination) &&
        !axet_address_equal(&header->destination, &axet_interface.hardware))
    {
        if (!axet_interface.promiscuous)
        {
            axet_interface.stats.dropped++;
            return 0;
        }
    }
    axet_interface.stats.received++;
    if (ethertype == AXET_ETH_ARP)
    {
        const u8 *arp = frame + AXET_HEADER;
        u32 sender = (u32)arp[14] | ((u32)arp[15] << 8) |
                     ((u32)arp[16] << 16) | ((u32)arp[17] << 24);
        axet_address_t source;
        u32 i;
        for (i = 0; i < 6; i++)
            source.bytes[i] = arp[8 + i];
        source.valid = true;
        axet_arp_learn(sender, &source);
    }
    return ethertype;
}

void axet_arp_tick(void)
{
    u32 i;
    for (i = 0; i < AXET_ARP_CACHE; i++)
    {
        axet_arp_entry_t *entry = &axet_interface.arp[i];
        if (!entry->valid)
            continue;
        if (entry->ttl > 0)
            entry->ttl--;
        if (entry->ttl == 0)
            entry->valid = false;
    }
}

u32 axet_arp_count(void)
{
    u32 i;
    u32 count = 0;
    for (i = 0; i < AXET_ARP_CACHE; i++)
    {
        if (axet_interface.arp[i].valid)
            count++;
    }
    return count;
}

u32 axet_mtu(void)
{
    return axet_interface.mtu;
}

const axet_frame_stats_t *axet_stats(void)
{
    return &axet_interface.stats;
}

void axet_set_promiscuous(bool_t enabled)
{
    axet_interface.promiscuous = enabled;
}

void axet_report(void)
{
    const axet_frame_stats_t *stats = &axet_interface.stats;
    printk("axet: %s ipv4=0x%x rx=%u tx=%u drop=%u bad=%u arp=%u/%u\n",
           axet_interface.name, (unsigned)axet_interface.ipv4,
           (unsigned)stats->received, (unsigned)stats->transmitted,
           (unsigned)stats->dropped, (unsigned)stats->malformed,
           (unsigned)stats->arp_hits, (unsigned)stats->arp_misses);
}

/* ------------------------------------------------------------ test backend */

static u8 axet_test_out[AXET_HEADER + AXET_MAX_MTU];
static u32 axet_test_length;

static bool_t axet_test_send(const u8 *frame, u32 length)
{
    u32 i;
    if (length > AXIOM_ARRAY_LEN(axet_test_out))
        return false;
    for (i = 0; i < length; i++)
        axet_test_out[i] = frame[i];
    axet_test_length = length;
    return true;
}

bool_t axet_selftest(void)
{
    axet_address_t hardware;
    axet_address_t destination;
    axet_address_t resolved;
    u8 payload[32];
    u32 i;
    axet_init(NULL_PTR, 0x0A000001u, 0xFFFFFF00u, 0x0A0000FEu, axet_test_send, "eth0");
    for (i = 0; i < 6; i++)
    {
        hardware.bytes[i] = (u8)(0x02 + i);
        destination.bytes[i] = (u8)(0x10 + i);
    }
    hardware.valid = true;
    destination.valid = true;
    axet_set_address(&hardware);
    if (!axet_arp_learn(0x0A000002u, &destination))
        return false;
    if (!axet_arp_resolve(0x0A000002u, &resolved))
        return false;
    if (resolved.bytes[0] != 0x10)
        return false;
    for (i = 0; i < 32; i++)
        payload[i] = (u8)i;
    if (!axet_send_frame(&destination, AXET_ETH_IPV4, payload, 32))
        return false;
    if (axet_test_length != AXET_HEADER + 32)
        return false;
    if (axet_test_out[12] != 0x00 || axet_test_out[13] != 0x08)
        return false;
    if (axet_arp_count() != 1)
        return false;
    axet_arp_tick();
    if (axet_arp_count() != 1)
        return false;
    if (axet_send_frame(&destination, AXET_ETH_IPV4, payload, AXET_MAX_MTU + 1))
        return false;
    return true;
}
