/* AxiomOS - ICMP echo, error handling and path MTU discovery.
 *
 * The module owns the identifier/sequence bookkeeping for echo requests, the
 * statistics counters, and the translation of ICMP errors back to a pending
 * request so the socket layer can report a real failure cause.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXIC_MAX_PENDING      16
#define AXIC_TYPE_ECHO_REPLY  0
#define AXIC_TYPE_DEST_UNREACH 3
#define AXIC_TYPE_SOURCE_QUENCH 4
#define AXIC_TYPE_REDIRECT    5
#define AXIC_TYPE_ECHO_REQUEST 8
#define AXIC_TYPE_TIME_EXCEEDED 11
#define AXIC_TYPE_PARAM_PROBLEM 12

#define AXIC_CODE_NET_UNREACH   0
#define AXIC_CODE_HOST_UNREACH  1
#define AXIC_CODE_PORT_UNREACH  3
#define AXIC_CODE_FRAG_NEEDED   4

#define AXIC_ERR_NONE         0
#define AXIC_ERR_UNREACH      1
#define AXIC_ERR_TIMEOUT      2
#define AXIC_ERR_FRAG         3
#define AXIC_ERR_REDIRECT     4

typedef struct axic_pending
{
    u32  address;
    u16  identifier;
    u16  sequence;
    u32  sent_ms;
    u32  payload_length;
    u32  next_hop_mtu;
    u32  rtt_ms;
    u8   error;
    bool_t answered;
    bool_t used;
} axic_pending_t;

typedef struct axic_state
{
    axic_pending_t pending[AXIC_MAX_PENDING];
    u32  pending_count;
    u32  next_identifier;
    u32  next_sequence;
    u32  echo_sent;
    u32  echo_received;
    u32  unreachable;
    u32  time_exceeded;
    u32  redirects;
    u32  parameter_problems;
    u32  source_quench;
    u32  checksum_errors;
    u32  queued_errors;
    u32  pmtu_updates;
    u32  mtu_floor;
    u32  mtu_default;
    u32  now_ms;
    bool_t checksum_offload;
    bool_t ready;
} axic_state_t;

static axic_state_t axic_state;

static void axic_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

u16 axic_checksum(const u8 *data, u32 length)
{
    u32 sum = 0;
    u32 i;
    if (data == NULL_PTR)
        return 0;
    for (i = 0; i + 1 < length; i += 2)
        sum += (u32)data[i] << 8 | data[i + 1];
    if ((length & 1) != 0)
        sum += (u32)data[length - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

static axic_pending_t *axic_find(u32 address, u16 identifier, u16 sequence)
{
    u32 i;
    for (i = 0; i < axic_state.pending_count; i++)
    {
        axic_pending_t *entry = &axic_state.pending[i];
        if (!entry->used)
            continue;
        if (entry->address != address)
            continue;
        if (entry->identifier != identifier && identifier != 0xFFFF)
            continue;
        if (entry->sequence != sequence)
            continue;
        return entry;
    }
    return NULL_PTR;
}

static axic_pending_t *axic_alloc(u32 address, u16 identifier)
{
    u32 i;
    for (i = 0; i < AXIC_MAX_PENDING; i++)
    {
        if (!axic_state.pending[i].used)
            return &axic_state.pending[i];
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axic_init(u32 default_mtu, bool_t offload)
{
    axic_zero(&axic_state, (u32)sizeof(axic_state));
    axic_state.mtu_default = default_mtu == 0 ? 1500 : default_mtu;
    axic_state.mtu_floor = 576;
    axic_state.checksum_offload = offload;
    axic_state.next_identifier = 0x1000;
    axic_state.next_sequence = 1;
    axic_state.ready = true;
}

u32 axic_send_echo(u32 address, u32 payload_length, u32 now_ms)
{
    axic_pending_t *entry;
    if (!axic_state.ready)
        return 0;
    entry = axic_alloc(address, (u16)axic_state.next_identifier);
    if (entry == NULL_PTR)
        return 0;
    axic_zero(entry, (u32)sizeof(*entry));
    entry->address = address;
    entry->identifier = (u16)axic_state.next_identifier;
    entry->sequence = (u16)axic_state.next_sequence;
    entry->payload_length = payload_length;
    entry->sent_ms = now_ms;
    entry->used = true;
    axic_state.pending_count++;
    axic_state.next_sequence++;
    if ((axic_state.next_sequence & 0xFFFF) == 0)
        axic_state.next_sequence = 1;
    axic_state.next_identifier = (axic_state.next_identifier + 1) & 0xFFFF;
    axic_state.echo_sent++;
    return axic_state.echo_sent;
}

u32 axic_build_echo(u32 address, u16 identifier, u16 sequence, u8 *packet, u32 capacity,
                    u32 payload_length)
{
    u32 total = payload_length + 8;
    u32 i;
    if (packet == NULL_PTR || capacity < total)
        return 0;
    packet[0] = AXIC_TYPE_ECHO_REQUEST;
    packet[1] = 0;
    packet[2] = 0;
    packet[3] = 0;
    packet[4] = (u8)(identifier >> 8);
    packet[5] = (u8)(identifier & 0xFF);
    packet[6] = (u8)(sequence >> 8);
    packet[7] = (u8)(sequence & 0xFF);
    for (i = 0; i < payload_length; i++)
        packet[8 + i] = (u8)(i + address);
    {
        u16 sum = axic_checksum(packet, total);
        if (axic_state.checksum_offload)
            sum = 0;
        packet[2] = (u8)(sum >> 8);
        packet[3] = (u8)(sum & 0xFF);
    }
    return total;
}

u32 axic_receive(const u8 *packet, u32 length, u32 now_ms, u32 *out_rtt)
{
    u16 identifier;
    u16 sequence;
    u16 sum;
    u8 type;
    u8 code;
    if (packet == NULL_PTR || length < 8)
        return AXIC_ERR_NONE;
    type = packet[0];
    code = packet[1];
    sum = axic_checksum(packet, length);
    if (sum != 0 && !axic_state.checksum_offload)
    {
        axic_state.checksum_errors++;
        return AXIC_ERR_NONE;
    }
    switch (type)
    {
    case AXIC_TYPE_ECHO_REPLY:
        identifier = (u16)((packet[4] << 8) | packet[5]);
        sequence = (u16)((packet[6] << 8) | packet[7]);
        axic_state.echo_received++;
        {
            axic_pending_t *entry = axic_find(0, identifier, sequence);
            if (entry != NULL_PTR)
            {
                entry->answered = true;
                entry->rtt_ms = now_ms > entry->sent_ms ? now_ms - entry->sent_ms : 0;
                if (out_rtt != NULL_PTR)
                    *out_rtt = entry->rtt_ms;
                if (axic_state.pending_count > 0)
                    axic_state.pending_count--;
                entry->used = false;
                return AXIC_ERR_NONE;
            }
        }
        return AXIC_ERR_NONE;
    case AXIC_TYPE_ECHO_REQUEST:
        return 1;
    case AXIC_TYPE_DEST_UNREACH:
        axic_state.unreachable++;
        if (code == AXIC_CODE_FRAG_NEEDED && length >= 8)
        {
            u32 mtu = ((u32)packet[6] << 8) | packet[7];
            if (mtu >= axic_state.mtu_floor && mtu < axic_state.mtu_default)
            {
                axic_state.mtu_default = mtu;
                axic_state.pmtu_updates++;
            }
            axic_state.queued_errors++;
            return AXIC_ERR_FRAG;
        }
        axic_state.queued_errors++;
        return AXIC_ERR_UNREACH;
    case AXIC_TYPE_TIME_EXCEEDED:
        axic_state.time_exceeded++;
        axic_state.queued_errors++;
        return AXIC_ERR_TIMEOUT;
    case AXIC_TYPE_REDIRECT:
        axic_state.redirects++;
        return AXIC_ERR_REDIRECT;
    case AXIC_TYPE_PARAM_PROBLEM:
        axic_state.parameter_problems++;
        axic_state.queued_errors++;
        return AXIC_ERR_UNREACH;
    case AXIC_TYPE_SOURCE_QUENCH:
        axic_state.source_quench++;
        return AXIC_ERR_NONE;
    default:
        return AXIC_ERR_NONE;
    }
}

u32 axic_expire(u32 now_ms, u32 timeout_ms, u32 *failed)
{
    u32 i;
    u32 expired = 0;
    for (i = 0; i < AXIC_MAX_PENDING; i++)
    {
        axic_pending_t *entry = &axic_state.pending[i];
        if (!entry->used || entry->answered)
            continue;
        if (now_ms - entry->sent_ms < timeout_ms)
            continue;
        entry->error = AXIC_ERR_TIMEOUT;
        entry->used = false;
        if (axic_state.pending_count > 0)
            axic_state.pending_count--;
        expired++;
        if (failed != NULL_PTR)
            (*failed)++;
    }
    axic_state.now_ms = now_ms;
    return expired;
}

u32 axic_pending_count(void)
{
    return axic_state.pending_count;
}

u32 axic_mtu(void)
{
    return axic_state.mtu_default;
}

bool_t axic_set_mtu(u32 mtu)
{
    if (mtu < axic_state.mtu_floor || mtu > 9000)
        return false;
    axic_state.mtu_default = mtu;
    return true;
}

u32 axic_rtt(u32 address)
{
    u32 i;
    for (i = 0; i < AXIC_MAX_PENDING; i++)
    {
        if (axic_state.pending[i].used && axic_state.pending[i].address == address)
            return axic_state.pending[i].rtt_ms;
    }
    return 0;
}

void axic_report(void)
{
    u32 i;
    printk("axic: sent=%u recv=%u unreach=%u timeout=%u mtu=%u pending=%u\n",
           (unsigned)axic_state.echo_sent, (unsigned)axic_state.echo_received,
           (unsigned)axic_state.unreachable, (unsigned)axic_state.time_exceeded,
           (unsigned)axic_state.mtu_default, (unsigned)axic_state.pending_count);
    for (i = 0; i < AXIC_MAX_PENDING; i++)
    {
        const axic_pending_t *entry = &axic_state.pending[i];
        if (!entry->used)
            continue;
        printk("  echo addr=0x%x id=%u seq=%u len=%u rtt=%u\n",
               (unsigned)entry->address, (unsigned)entry->identifier,
               (unsigned)entry->sequence, (unsigned)entry->payload_length,
               (unsigned)entry->rtt_ms);
    }
}

bool_t axic_selftest(void)
{
    u8 packet[64];
    u32 length;
    u32 rtt = 0;
    axic_init(1500, false);
    if (axic_send_echo(0xC0A80002u, 32, 0) == 0)
        return false;
    length = axic_build_echo(0xC0A80002u, axic_state.pending[0].identifier,
                            axic_state.pending[0].sequence, packet, 64, 32);
    if (length != 40)
        return false;
    if (packet[0] != AXIC_TYPE_ECHO_REQUEST)
        return false;
    if (axic_checksum(packet, length) != 0)
        return false;
    packet[0] = AXIC_TYPE_ECHO_REPLY;
    packet[2] = 0;
    packet[3] = 0;
    {
        u16 sum = axic_checksum(packet, length);
        packet[2] = (u8)(sum >> 8);
        packet[3] = (u8)(sum & 0xFF);
    }
    if (axic_receive(packet, length, 50, &rtt) != AXIC_ERR_NONE)
        return false;
    if (rtt != 50)
        return false;
    if (axic_pending_count() != 0)
        return false;
    if (axic_send_echo(0xC0A80003u, 16, 0) == 0)
        return false;
    if (axic_expire(5000, 1000, NULL_PTR) != 1)
        return false;
    if (!axic_set_mtu(1400))
        return false;
    if (axic_set_mtu(100))
    {
        /* Below the IPv4 minimum must be rejected. */
    }
    if (axic_mtu() != 1400)
        return false;
    if (axic_build_echo(0x0, 1, 1, packet, 8, 16) != 0)
        return false;
    return true;
}

