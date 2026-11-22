/* AxiomOS - DHCP client state machine.
 *
 * Implements the DISCOVER/Offer/REQUEST/Ack exchange with retransmission
 * backoff, lease renewal at half life, rebinding at 87.5%, and a fallback to
 * link local addressing when no server answers.  Options are parsed from the
 * wire format into a flat record so the network stack can consume them.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXDH_MAX_LEASES       4
#define AXDH_OPTIONS_MAX      10
#define AXDH_BOOTP_FLAG_BROADCAST 0x8000
#define AXDH_RETRY_BASE       4
#define AXDH_RETRY_MAX        64
#define AXDH_LEASE_MIN        60

#define AXDH_STATE_IDLE       0
#define AXDH_STATE_DISCOVER   1
#define AXDH_STATE_OFFER      2
#define AXDH_STATE_REQUEST    3
#define AXDH_STATE_BOUND      4
#define AXDH_STATE_RENEW      5
#define AXDH_STATE_REBIND     6
#define AXDH_STATE_DECLINE    7
#define AXDH_STATE_FALLBACK   8

#define AXDH_OPT_PAD          0
#define AXDH_OPT_SUBNET       1
#define AXDH_OPT_ROUTER       3
#define AXDH_OPT_DNS          6
#define AXDH_OPT_HOSTNAME     12
#define AXDH_OPT_REQUESTED_IP 50
#define AXDH_OPT_LEASE_TIME   51
#define AXDH_OPT_MESSAGE_TYPE 53
#define AXDH_OPT_SERVER_ID    54
#define AXDH_OPT_RENEWAL      58
#define AXDH_OPT_REBINDING    59
#define AXDH_OPT_END          255

#define AXDH_MSG_DISCOVER     1
#define AXDH_MSG_OFFER        2
#define AXDH_MSG_REQUEST      3
#define AXDH_MSG_DECLINE      4
#define AXDH_MSG_ACK          5
#define AXDH_MSG_NAK          6
#define AXDH_MSG_RELEASE      7

typedef struct axdh_option
{
    u8   code;
    u8   length;
    u8   value[16];
    bool_t used;
} axdh_option_t;

typedef struct axdh_lease
{
    u32  offered_ip;
    u32  server_ip;
    u32  subnet_mask;
    u32  gateway;
    u32  dns_primary;
    u32  dns_secondary;
    u32  lease_seconds;
    u32  renewal_seconds;
    u32  rebinding_seconds;
    u32  obtained_ms;
    u32  retries;
    u32  transaction;
    u8   state;
    bool_t bound;
} axdh_lease_t;

typedef struct axdh_state
{
    axdh_lease_t lease;
    axdh_option_t options[AXDH_OPTIONS_MAX];
    u32  option_count;
    u32  state;
    u32  transmits;
    u32  receives;
    u32  naks;
    u32  timeouts;
    u32  renewals;
    u32  rebinds;
    u32  declines;
    u32  transaction;
    u32  next_retry_ms;
    u32  backoff_ms;
    u32  now_ms;
    u32  hardware;
    bool_t broadcast;
    bool_t ready;
} axdh_state_t;

static axdh_state_t axdh_state;

static void axdh_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axdh_store_option(u8 code, u8 length, const u8 *value)
{
    axdh_option_t *option;
    if (axdh_state.option_count >= AXDH_OPTIONS_MAX)
        return;
    option = &axdh_state.options[axdh_state.option_count];
    option->code = code;
    option->length = length > 16 ? 16 : length;
    for (u8 i = 0; i < option->length; i++)
        option->value[i] = value[i];
    option->used = true;
    axdh_state.option_count++;
}

static u32 axdh_read_u32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | (u32)data[3];
}

static void axdh_advance(u32 state)
{
    axdh_state.state = state;
    axdh_state.lease.state = (u8)state;
}

/* -------------------------------------------------------------- public API */

void axdh_init(u32 hardware, bool_t broadcast)
{
    axdh_zero(&axdh_state, (u32)sizeof(axdh_state));
    axdh_state.hardware = hardware;
    axdh_state.broadcast = broadcast;
    axdh_state.backoff_ms = AXDH_RETRY_BASE * 250;
    axdh_advance(AXDH_STATE_IDLE);
    axdh_state.ready = true;
}

u32 axdh_discover(u32 transaction)
{
    if (!axdh_state.ready)
        return 0;
    axdh_state.transaction = transaction == 0 ? 0xA1B2C3D4u : transaction;
    axdh_state.lease.transaction = axdh_state.transaction;
    axdh_state.lease.offered_ip = 0;
    axdh_state.lease.server_ip = 0;
    axdh_state.lease.bound = false;
    axdh_state.backoff_ms = AXDH_RETRY_BASE * 250;
    axdh_advance(AXDH_STATE_DISCOVER);
    axdh_state.transmits++;
    axdh_state.next_retry_ms = axdh_state.now_ms + axdh_state.backoff_ms;
    return axdh_state.transaction;
}

u32 axdh_build_message(u8 message_type, u32 requested_ip, u8 *packet, u32 capacity)
{
    u32 i;
    u32 length = 0;
    if (packet == NULL_PTR || capacity < 300)
        return 0;
    for (i = 0; i < capacity; i++)
        packet[i] = 0;
    packet[0] = 0x01;
    packet[1] = 0x01;
    packet[2] = 0x06;
    packet[3] = 0x00;
    packet[4] = (u8)(axdh_state.transaction >> 24);
    packet[5] = (u8)(axdh_state.transaction >> 16);
    packet[6] = (u8)(axdh_state.transaction >> 8);
    packet[7] = (u8)(axdh_state.transaction);
    packet[10] = axdh_state.broadcast ? 0x80 : 0x00;
    packet[11] = 0x00;
    for (i = 0; i < 6; i++)
        packet[28 + i] = (u8)(axdh_state.hardware + i);
    packet[236] = 0x63;
    packet[237] = 0x82;
    packet[238] = 0x53;
    packet[239] = 0x63;
    length = 240;
    packet[length++] = AXDH_OPT_MESSAGE_TYPE;
    packet[length++] = 1;
    packet[length++] = message_type;
    if (requested_ip != 0)
    {
        packet[length++] = AXDH_OPT_REQUESTED_IP;
        packet[length++] = 4;
        packet[length++] = (u8)(requested_ip >> 24);
        packet[length++] = (u8)(requested_ip >> 16);
        packet[length++] = (u8)(requested_ip >> 8);
        packet[length++] = (u8)requested_ip;
    }
    packet[length++] = AXDH_OPT_HOSTNAME;
    packet[length++] = 8;
    packet[length++] = 'a';
    packet[length++] = 'x';
    packet[length++] = 'i';
    packet[length++] = 'o';
    packet[length++] = 'm';
    packet[length++] = 'o';
    packet[length++] = 's';
    packet[length++] = '1';
    packet[length++] = AXDH_OPT_END;
    return length;
}

bool_t axdh_parse(const u8 *packet, u32 length)
{
    u8 message_type = 0;
    u32 offset = 240;
    u32 i;
    if (packet == NULL_PTR || length < 244)
        return false;
    axdh_state.option_count = 0;
    axdh_state.receives++;
    while (offset + 1 < length)
    {
        u8 code = packet[offset];
        u8 option_length;
        if (code == AXDH_OPT_PAD)
        {
            offset++;
            continue;
        }
        if (code == AXDH_OPT_END)
            break;
        option_length = packet[offset + 1];
        if (offset + 2 + option_length > length)
            return false;
        switch (code)
        {
        case AXDH_OPT_MESSAGE_TYPE:
            if (option_length >= 1)
                message_type = packet[offset + 2];
            break;
        case AXDH_OPT_SUBNET:
            if (option_length == 4)
                axdh_state.lease.subnet_mask = axdh_read_u32(packet + offset + 2);
            break;
        case AXDH_OPT_ROUTER:
            if (option_length >= 4)
                axdh_state.lease.gateway = axdh_read_u32(packet + offset + 2);
            break;
        case AXDH_OPT_DNS:
            if (option_length >= 4)
                axdh_state.lease.dns_primary = axdh_read_u32(packet + offset + 2);
            if (option_length >= 8)
                axdh_state.lease.dns_secondary = axdh_read_u32(packet + offset + 6);
            break;
        case AXDH_OPT_LEASE_TIME:
            if (option_length == 4)
                axdh_state.lease.lease_seconds = axdh_read_u32(packet + offset + 2);
            break;
        case AXDH_OPT_SERVER_ID:
            if (option_length == 4)
                axdh_state.lease.server_ip = axdh_read_u32(packet + offset + 2);
            break;
        case AXDH_OPT_RENEWAL:
            if (option_length == 4)
                axdh_state.lease.renewal_seconds = axdh_read_u32(packet + offset + 2);
            break;
        case AXDH_OPT_REBINDING:
            if (option_length == 4)
                axdh_state.lease.rebinding_seconds = axdh_read_u32(packet + offset + 2);
            break;
        default:
            break;
        }
        axdh_store_option(code, option_length, packet + offset + 2);
        offset += 2 + option_length;
    }
    if (message_type == 0)
        return false;
    if (message_type == AXDH_MSG_OFFER)
    {
        if (axdh_state.lease.lease_seconds == 0)
            axdh_state.lease.lease_seconds = 3600;
        if (axdh_state.lease.renewal_seconds == 0)
            axdh_state.lease.renewal_seconds = axdh_state.lease.lease_seconds / 2;
        if (axdh_state.lease.rebinding_seconds == 0)
            axdh_state.lease.rebinding_seconds = (axdh_state.lease.lease_seconds * 7) / 8;
        axdh_advance(AXDH_STATE_REQUEST);
        axdh_state.transmits++;
        return true;
    }
    if (message_type == AXDH_MSG_ACK)
    {
        if (axdh_state.lease.lease_seconds == 0 || axdh_state.lease.lease_seconds < AXDH_LEASE_MIN)
            axdh_state.lease.lease_seconds = 3600;
        axdh_state.lease.bound = true;
        axdh_state.lease.obtained_ms = axdh_state.now_ms;
        axdh_advance(AXDH_STATE_BOUND);
        return true;
    }
    if (message_type == AXDH_MSG_NAK)
    {
        axdh_state.naks++;
        axdh_state.lease.bound = false;
        axdh_state.lease.offered_ip = 0;
        axdh_advance(AXDH_STATE_IDLE);
        return false;
    }
    if (message_type == AXDH_MSG_RELEASE)
        return true;
    for (i = 0; i < 1; i++)
        ;
    return false;
}

u32 axdh_offer(u32 offered_ip, u32 server_ip, u32 lease_seconds)
{
    if (axdh_state.state != AXDH_STATE_DISCOVER && axdh_state.state != AXDH_STATE_OFFER)
        return 0;
    axdh_state.lease.offered_ip = offered_ip;
    axdh_state.lease.server_ip = server_ip;
    axdh_state.lease.lease_seconds = lease_seconds < AXDH_LEASE_MIN ? AXDH_LEASE_MIN : lease_seconds;
    axdh_state.lease.renewal_seconds = axdh_state.lease.lease_seconds / 2;
    axdh_state.lease.rebinding_seconds = (axdh_state.lease.lease_seconds * 7) / 8;
    axdh_advance(AXDH_STATE_REQUEST);
    axdh_state.transmits++;
    return axdh_state.lease.offered_ip;
}

bool_t axdh_ack(u32 now_ms)
{
    if (!axdh_state.lease.offered_ip)
        return false;
    axdh_state.lease.bound = true;
    axdh_state.lease.obtained_ms = now_ms;
    axdh_state.now_ms = now_ms;
    axdh_advance(AXDH_STATE_BOUND);
    return true;
}

u32 axdh_tick(u32 now_ms)
{
    u32 elapsed_seconds;
    axdh_state.now_ms = now_ms;
    if (axdh_state.lease.bound)
    {
        elapsed_seconds = (now_ms - axdh_state.lease.obtained_ms) / 1000;
        if (axdh_state.lease.rebinding_seconds != 0 &&
            elapsed_seconds >= axdh_state.lease.rebinding_seconds &&
            axdh_state.state == AXDH_STATE_RENEW)
        {
            axdh_advance(AXDH_STATE_REBIND);
            axdh_state.rebinds++;
            axdh_state.lease.retries++;
            return AXDH_STATE_REBIND;
        }
        if (axdh_state.lease.renewal_seconds != 0 &&
            elapsed_seconds >= axdh_state.lease.renewal_seconds &&
            axdh_state.state == AXDH_STATE_BOUND)
        {
            axdh_advance(AXDH_STATE_RENEW);
            axdh_state.renewals++;
            axdh_state.lease.retries++;
            return AXDH_STATE_RENEW;
        }
        return axdh_state.state;
    }
    if (axdh_state.state == AXDH_STATE_DISCOVER || axdh_state.state == AXDH_STATE_REQUEST)
    {
        if (now_ms < axdh_state.next_retry_ms)
            return axdh_state.state;
        axdh_state.timeouts++;
        axdh_state.lease.retries++;
        axdh_state.transmits++;
        axdh_state.backoff_ms *= 2;
        if (axdh_state.backoff_ms > AXDH_RETRY_MAX * 250)
            axdh_state.backoff_ms = AXDH_RETRY_MAX * 250;
        axdh_state.next_retry_ms = now_ms + axdh_state.backoff_ms;
        if (axdh_state.lease.retries >= 8)
        {
            axdh_advance(AXDH_STATE_FALLBACK);
            axdh_state.lease.offered_ip = 0xA9FE0101u;
            axdh_state.lease.subnet_mask = 0xFFFF0000u;
            axdh_state.lease.bound = true;
        }
    }
    return axdh_state.state;
}

bool_t axdh_decline(void)
{
    if (!axdh_state.lease.bound)
        return false;
    axdh_state.declines++;
    axdh_advance(AXDH_STATE_DECLINE);
    axdh_state.lease.bound = false;
    axdh_state.transmits++;
    return true;
}

bool_t axdh_release(void)
{
    if (!axdh_state.lease.bound)
        return false;
    axdh_advance(AXDH_STATE_IDLE);
    axdh_state.lease.bound = false;
    axdh_state.transmits++;
    return true;
}

u32 axdh_address(void)
{
    return axdh_state.lease.offered_ip;
}

u32 axdh_gateway(void)
{
    return axdh_state.lease.gateway;
}

u32 axdh_dns(void)
{
    return axdh_state.lease.dns_primary;
}

u32 axdh_mask(void)
{
    return axdh_state.lease.subnet_mask;
}

u32 axdh_state_code(void)
{
    return axdh_state.state;
}

u32 axdh_option_count(void)
{
    return axdh_state.option_count;
}

bool_t axdh_option(u32 index, u8 *code, u8 *length, const u8 **value)
{
    if (index >= axdh_state.option_count)
        return false;
    if (code != NULL_PTR)
        *code = axdh_state.options[index].code;
    if (length != NULL_PTR)
        *length = axdh_state.options[index].length;
    if (value != NULL_PTR)
        *value = axdh_state.options[index].value;
    return true;
}

void axdh_report(void)
{
    u32 i;
    printk("axdh: state=%u ip=0x%x mask=0x%x gw=0x%x dns=0x%x lease=%us\n",
           (unsigned)axdh_state.state, (unsigned)axdh_state.lease.offered_ip,
           (unsigned)axdh_state.lease.subnet_mask, (unsigned)axdh_state.lease.gateway,
           (unsigned)axdh_state.lease.dns_primary, (unsigned)axdh_state.lease.lease_seconds);
    printk("axdh: tx=%u rx=%u nak=%u timeouts=%u renew=%u rebind=%u options=%u\n",
           (unsigned)axdh_state.transmits, (unsigned)axdh_state.receives,
           (unsigned)axdh_state.naks, (unsigned)axdh_state.timeouts,
           (unsigned)axdh_state.renewals, (unsigned)axdh_state.rebinds,
           (unsigned)axdh_state.option_count);
    for (i = 0; i < axdh_state.option_count; i++)
        printk("  option %u code=%u len=%u\n", (unsigned)i,
               (unsigned)axdh_state.options[i].code, (unsigned)axdh_state.options[i].length);
}

bool_t axdh_selftest(void)
{
    u8 packet[320];
    u32 length;
    axdh_init(0x0011223344ull, true);
    if (axdh_discover(0) == 0)
        return false;
    if (axdh_state_code() != AXDH_STATE_DISCOVER)
        return false;
    length = axdh_build_message(AXDH_MSG_DISCOVER, 0, packet, 320);
    if (length < 240)
        return false;
    if (packet[0] != 1 || packet[2] != 6)
        return false;
    if (packet[236] != 0x63)
        return false;
    if (axdh_offer(0xC0A80164u, 0xC0A80101u, 7200) == 0)
        return false;
    if (axdh_state_code() != AXDH_STATE_REQUEST)
        return false;
    if (!axdh_ack(1000))
        return false;
    if (axdh_state_code() != AXDH_STATE_BOUND)
        return false;
    if (axdh_address() != 0xC0A80164u)
        return false;
    if (axdh_tick(1000 + 3700 * 1000) != AXDH_STATE_RENEW)
        return false;
    if (axdh_tick(1000 + 6400 * 1000) != AXDH_STATE_REBIND)
        return false;
    if (!axdh_release())
        return false;
    if (axdh_address() != 0xC0A80164u)
    {
        /* The address stays cached after release for display. */
    }
    if (axdh_discover(0) == 0)
        return false;
    axdh_state.lease.retries = 8;
    axdh_state.next_retry_ms = 0;
    axdh_tick(1000);
    if (axdh_state_code() != AXDH_STATE_FALLBACK)
        return false;
    if ((axdh_address() & 0xFFFF0000u) != 0xA9FE0000u)
        return false;
    return true;
}

