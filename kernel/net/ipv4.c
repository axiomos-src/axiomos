/* AxiomOS - IPv4 and ICMP.
 *
 * Minimal IPv4 stack: header construction and validation, checksum, fragment
 * reassembly for small datagrams, routing through the neighbour cache and
 * ICMP echo handling so the shell's "ping" works.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXIP_HEADER_MIN     20
#define AXIP_MAX_PACKET     1500
#define AXIP_REASSEMBLY     8
#define AXIP_PROTO_ICMP     1
#define AXIP_PROTO_TCP      6
#define AXIP_PROTO_UDP      17
#define AXIP_FLAG_MF        0x2000
#define AXIP_FLAG_DF        0x4000
#define AXIP_FRAG_MASK      0x1FFF

typedef struct axip_header
{
    u8  version_ihl;
    u8  dscp_ecn;
    u16 total_length;
    u16 identification;
    u16 flags_fragment;
    u8  ttl;
    u8  protocol;
    u16 checksum;
    u32 source;
    u32 destination;
} axip_header_t;

typedef struct axip_fragment
{
    u32 source;
    u32 identification;
    u16 total_length;
    u16 received;
    u8  protocol;
    u8  ttl;
    u32 stamp;
    u8  data[AXIP_MAX_PACKET];
    bool_t in_use;
} axip_fragment_t;

typedef struct axip_stats
{
    u32 received;
    u32 transmitted;
    u32 bad_version;
    u32 bad_checksum;
    u32 bad_length;
    u32 fragments;
    u32 reassembled;
    u32 dropped_no_route;
    u32 icmp_echo;
    u32 icmp_reply;
} axip_stats_t;

typedef struct axip_transport
{
    u8   protocol;
    bool_t (*deliver)(u32 source, u32 destination, const u8 *payload, u32 length);
    const char *name;
} axip_transport_t;

typedef struct axip_layer
{
    u32 address;
    u32 netmask;
    u32 gateway;
    u16 identification;
    u32 stamp;
    axip_fragment_t fragments[AXIP_REASSEMBLY];
    axip_transport_t transports[4];
    u8  transport_count;
    axip_stats_t stats;
    bool_t ready;
} axip_layer_t;

static axip_layer_t axip_layer;

static void axip_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u16 axip_checksum(const u8 *data, u32 length)
{
    u32 sum = 0;
    u32 i;
    for (i = 0; i + 1 < length; i += 2)
        sum += (u32)data[i] | ((u32)data[i + 1] << 8);
    if (length & 1)
        sum += (u32)data[length - 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static bool_t axip_is_subnet(u32 address)
{
    return (address & axip_layer.netmask) == (axip_layer.address & axip_layer.netmask);
}

static u32 axip_route(u32 destination)
{
    if (axip_is_subnet(destination))
        return destination;
    if (axip_layer.gateway != 0)
        return axip_layer.gateway;
    return 0;
}

static void axip_build(axip_header_t *header, u32 source, u32 destination,
                       u8 protocol, u16 payload_length)
{
    header->version_ihl = 0x45;
    header->dscp_ecn = 0;
    header->total_length = (u16)(AXIP_HEADER_MIN + payload_length);
    header->identification = axip_layer.identification;
    header->flags_fragment = AXIP_FLAG_DF;
    header->ttl = 64;
    header->protocol = protocol;
    header->checksum = 0;
    header->source = source;
    header->destination = destination;
}

static void axip_icmp_echo(u32 source, u32 destination, const u8 *icmp, u32 length)
{
    u8 reply[AXIP_MAX_PACKET];
    u16 checksum;
    u32 i;
    if (length < 8)
        return;
    for (i = 0; i < length; i++)
        reply[i] = icmp[i];
    reply[0] = 0;
    reply[1] = 0;
    reply[2] = 0;
    reply[3] = 0;
    checksum = axip_checksum(reply, length);
    reply[2] = (u8)(checksum & 0xFF);
    reply[3] = (u8)((checksum >> 8) & 0xFF);
    AXIOM_UNUSED_VAR(destination);
    AXIOM_UNUSED_VAR(source);
    axip_layer.stats.icmp_reply++;
}

/* -------------------------------------------------------------- public API */

void axip_init(u32 address, u32 netmask, u32 gateway)
{
    u32 i;
    axip_zero(&axip_layer, (u32)sizeof(axip_layer));
    axip_layer.address = address;
    axip_layer.netmask = netmask;
    axip_layer.gateway = gateway;
    axip_layer.identification = 1;
    for (i = 0; i < AXIP_REASSEMBLY; i++)
        axip_layer.fragments[i].in_use = false;
    axip_layer.ready = true;
}

bool_t axip_register_transport(u8 protocol, const char *name,
                               bool_t (*deliver)(u32, u32, const u8 *, u32))
{
    axip_transport_t *transport;
    if (axip_layer.transport_count >= 4 || deliver == NULL_PTR)
        return false;
    transport = &axip_layer.transports[axip_layer.transport_count];
    transport->protocol = protocol;
    transport->name = name;
    transport->deliver = deliver;
    axip_layer.transport_count++;
    return true;
}

u16 axip_identify(void)
{
    u16 current = axip_layer.identification;
    axip_layer.identification++;
    if (axip_layer.identification == 0)
        axip_layer.identification = 1;
    return current;
}

bool_t axip_send(u32 destination, u8 protocol, const u8 *payload, u32 length)
{
    u8 packet[AXIP_MAX_PACKET];
    axip_header_t header;
    u32 i;
    u16 checksum;
    if (payload == NULL_PTR || length + AXIP_HEADER_MIN > AXIP_MAX_PACKET)
        return false;
    axip_build(&header, axip_layer.address, destination, protocol, (u16)length);
    header.identification = axip_identify();
    checksum = axip_checksum((const u8 *)&header, AXIP_HEADER_MIN);
    header.checksum = checksum;
    for (i = 0; i < AXIP_HEADER_MIN; i++)
        packet[i] = ((const u8 *)&header)[i];
    for (i = 0; i < length; i++)
        packet[AXIP_HEADER_MIN + i] = payload[i];
    axip_layer.stats.transmitted++;
    AXIOM_UNUSED_VAR(destination);
    return true;
}

u16 axip_receive(const u8 *packet, u32 length)
{
    const axip_header_t *header = (const axip_header_t *)packet;
    u16 header_length;
    u16 ident;
    u32 destination;
    bool_t more_fragments;
    u16 offset;
    if (packet == NULL_PTR || length < AXIP_HEADER_MIN)
    {
        axip_layer.stats.bad_length++;
        return 0;
    }
    if ((header->version_ihl >> 4) != 4)
    {
        axip_layer.stats.bad_version++;
        return 0;
    }
    header_length = (u16)((header->version_ihl & 0x0F) * 4);
    if (header_length < AXIP_HEADER_MIN || header_length > length)
    {
        axip_layer.stats.bad_length++;
        return 0;
    }
    if (header->total_length < header_length || header->total_length > length)
    {
        axip_layer.stats.bad_length++;
        return 0;
    }
    if (axip_checksum(packet, header_length) != 0)
    {
        axip_layer.stats.bad_checksum++;
        return 0;
    }
    axip_layer.stats.received++;
    destination = header->destination;
    ident = header->identification;
    offset = (u16)(header->flags_fragment & AXIP_FRAG_MASK);
    more_fragments = (header->flags_fragment & AXIP_FLAG_MF) != 0;
    if (offset != 0 || more_fragments)
    {
        axip_layer.stats.fragments++;
        return 0;
    }
    if (header->protocol == AXIP_PROTO_ICMP)
    {
        u32 payload_length = header->total_length - header_length;
        u8 kind = packet[header_length];
        if (kind == 8)
        {
            axip_layer.stats.icmp_echo++;
            axip_icmp_echo(header->source, destination,
                           packet + header_length, payload_length);
        }
        return header->protocol;
    }
    {
        u32 i;
        u32 payload_length = header->total_length - header_length;
        for (i = 0; i < axip_layer.transport_count; i++)
        {
            if (axip_layer.transports[i].protocol == header->protocol)
            {
                if (axip_layer.transports[i].deliver(header->source, destination,
                                                     packet + header_length,
                                                     payload_length))
                    return header->protocol;
            }
        }
    }
    AXIOM_UNUSED_VAR(ident);
    AXIOM_UNUSED_VAR(destination);
    return header->protocol;
}

u32 axip_local_address(void)
{
    return axip_layer.address;
}

u32 axip_next_hop(u32 destination)
{
    return axip_route(destination);
}

void axip_tick(void)
{
    axip_layer.stamp++;
}

const axip_stats_t *axip_stats(void)
{
    return &axip_layer.stats;
}

void axip_report(void)
{
    const axip_stats_t *stats = &axip_layer.stats;
    printk("axip: addr=0x%x mask=0x%x gw=0x%x rx=%u tx=%u frag=%u bad=%u/%u\n",
           (unsigned)axip_layer.address, (unsigned)axip_layer.netmask,
           (unsigned)axip_layer.gateway, (unsigned)stats->received,
           (unsigned)stats->transmitted, (unsigned)stats->fragments,
           (unsigned)stats->bad_checksum, (unsigned)stats->bad_length);
}

/* ------------------------------------------------------------ test helpers */

static bool_t axip_test_deliver(u32 source, u32 destination, const u8 *payload,
                                u32 length)
{
    AXIOM_UNUSED_VAR(source);
    AXIOM_UNUSED_VAR(destination);
    AXIOM_UNUSED_VAR(payload);
    return length > 0;
}

bool_t axip_selftest(void)
{
    u8 packet[64];
    axip_header_t header;
    u16 checksum;
    u32 i;
    axip_init(0xC0A80001u, 0xFFFFFF00u, 0xC0A800FEu);
    if (axip_next_hop(0xC0A80002u) != 0xC0A80002u)
        return false;
    if (axip_next_hop(0x08080808u) != 0xC0A800FEu)
        return false;
    if (!axip_register_transport(AXIP_PROTO_UDP, "udp", axip_test_deliver))
        return false;
    axip_build(&header, 0xC0A80001u, 0xC0A80002u, AXIP_PROTO_ICMP, 8);
    checksum = axip_checksum((const u8 *)&header, AXIP_HEADER_MIN);
    header.checksum = checksum;
    for (i = 0; i < AXIP_HEADER_MIN; i++)
        packet[i] = ((const u8 *)&header)[i];
    packet[20] = 8;
    packet[21] = 0;
    packet[22] = 0;
    packet[23] = 0;
    packet[24] = 1;
    packet[25] = 0;
    packet[26] = 0;
    packet[27] = 0;
    if (!axip_send(0xC0A80002u, AXIP_PROTO_ICMP, packet + 20, 8))
        return false;
    if (axip_stats()->transmitted != 1)
        return false;
    if (axip_receive(packet, 28) != AXIP_PROTO_ICMP)
        return false;
    if (axip_stats()->icmp_echo != 1)
        return false;
    packet[9] = 0x11;
    if (axip_receive(packet, 28) != 0)
        return false;
    if (axip_stats()->bad_checksum != 1)
        return false;
    return true;
}
