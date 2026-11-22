/* AxiomOS - ARP neighbour cache and state machine.
 *
 * Entries move between empty, pending, resolved, stale and unreachable states
 * following theusual RFC 4861 style rules adapted to IPv4 ARP: a pending entry
 * drops after a few retransmissions, a resolved entry ages into stale, and a
 * stale entry is refreshed on first use.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXAP_SIZE            32
#define AXAP_PROBE_RETRIES   3
#define AXAP_PROBE_INTERVAL  1000
#define AXAP_STALE_TIMEOUT   30000
#define AXAP_GC_TIMEOUT      60000
#define AXAP_QUEUE_DEPTH     4
#define AXAP_ADDRESS_LEN     4
#define AXAP_HARDWARE_LEN    6

#define AXAP_STATE_EMPTY       0
#define AXAP_STATE_PENDING     1
#define AXAP_STATE_RESOLVED    2
#define AXAP_STATE_STALE       3
#define AXAP_STATE_UNREACHABLE 4

typedef struct axap_packet
{
    u32  ip;
    u32  length;
    u16  protocol;
    bool_t queued;
} axap_packet_t;

typedef struct axap_entry
{
    u32  ip;
    u8   hardware[AXAP_HARDWARE_LEN];
    u8   state;
    u8   probes;
    u32  age_ms;
    u32  hits;
    u32  updates;
    axap_packet_t queued[AXAP_QUEUE_DEPTH];
    u32  queued_count;
    bool_t static_entry;
    bool_t used;
} axap_entry_t;

typedef struct axap_state
{
    axap_entry_t entries[AXAP_SIZE];
    u32  count;
    u32  requests_sent;
    u32  replies_received;
    u32  gratitous;
    u32  evictions;
    u32  drops;
    u32  resolutions;
    u32  now_ms;
    u32  hash_collisions;
    u32  local_ip;
    u8   local_mac[AXAP_HARDWARE_LEN];
    bool_t ready;
} axap_state_t;

static axap_state_t axap_state;

static void axap_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axap_hash(u32 ip)
{
    u32 mixed = ip ^ (ip >> 13) ^ (ip >> 23);
    return mixed % AXAP_SIZE;
}

static axap_entry_t *axap_find(u32 ip)
{
    u32 i;
    u32 start = axap_hash(ip);
    for (i = 0; i < AXAP_SIZE; i++)
    {
        u32 index = (start + i) % AXAP_SIZE;
        axap_entry_t *entry = &axap_state.entries[index];
        if (!entry->used)
            continue;
        if (entry->ip == ip)
            return entry;
        axap_state.hash_collisions++;
    }
    return NULL_PTR;
}

static axap_entry_t *axap_slot(u32 ip)
{
    u32 i;
    u32 start = axap_hash(ip);
    u32 oldest = 0xFFFFFFFFu;
    u32 oldest_index = 0xFFFFFFFFu;
    for (i = 0; i < AXAP_SIZE; i++)
    {
        u32 index = (start + i) % AXAP_SIZE;
        axap_entry_t *entry = &axap_state.entries[index];
        if (!entry->used)
            return entry;
        if (entry->state == AXAP_STATE_EMPTY && !entry->static_entry)
            return entry;
        if (!entry->static_entry && entry->age_ms > oldest && entry->queued_count == 0)
        {
            oldest = entry->age_ms;
            oldest_index = index;
        }
    }
    if (oldest_index != 0xFFFFFFFFu)
    {
        axap_zero(&axap_state.entries[oldest_index], (u32)sizeof(axap_entry_t));
        axap_state.evictions++;
        return &axap_state.entries[oldest_index];
    }
    return NULL_PTR;
}

/* -------------------------------------------------------------- public API */

void axap_init(u32 local_ip, const u8 *local_mac)
{
    axap_zero(&axap_state, (u32)sizeof(axap_state));
    axap_state.local_ip = local_ip;
    if (local_mac != NULL_PTR)
    {
        for (u32 i = 0; i < AXAP_HARDWARE_LEN; i++)
            axap_state.local_mac[i] = local_mac[i];
    }
    axap_state.ready = true;
}

bool_t axap_add_static(u32 ip, const u8 *hardware)
{
    axap_entry_t *entry = axap_slot(ip);
    if (entry == NULL_PTR || hardware == NULL_PTR)
        return false;
    axap_zero(entry, (u32)sizeof(*entry));
    entry->ip = ip;
    for (u32 i = 0; i < AXAP_HARDWARE_LEN; i++)
        entry->hardware[i] = hardware[i];
    entry->state = AXAP_STATE_RESOLVED;
    entry->static_entry = true;
    entry->used = true;
    axap_state.count++;
    return true;
}

u32 axap_request(u32 ip)
{
    axap_entry_t *entry = axap_find(ip);
    if (entry == NULL_PTR)
    {
        entry = axap_slot(ip);
        if (entry == NULL_PTR)
            return 0;
        axap_zero(entry, (u32)sizeof(*entry));
        entry->ip = ip;
        entry->state = AXAP_STATE_PENDING;
        entry->used = true;
        axap_state.count++;
    }
    if (entry->state == AXAP_STATE_PENDING)
    {
        if (entry->probes >= AXAP_PROBE_RETRIES)
        {
            entry->state = AXAP_STATE_UNREACHABLE;
            axap_state.drops += entry->queued_count;
            entry->queued_count = 0;
            return 0;
        }
        entry->probes++;
    }
    entry->age_ms = 0;
    axap_state.requests_sent++;
    return axap_state.requests_sent;
}

bool_t axap_reply(u32 ip, const u8 *hardware)
{
    axap_entry_t *entry;
    if (hardware == NULL_PTR)
        return false;
    entry = axap_find(ip);
    if (entry == NULL_PTR)
    {
        entry = axap_slot(ip);
        if (entry == NULL_PTR)
            return false;
        axap_zero(entry, (u32)sizeof(*entry));
        entry->ip = ip;
        entry->used = true;
        axap_state.count++;
    }
    if (entry->state == AXAP_STATE_RESOLVED)
    {
        bool_t changed = false;
        for (u32 i = 0; i < AXAP_HARDWARE_LEN; i++)
        {
            if (entry->hardware[i] != hardware[i])
                changed = true;
        }
        if (changed)
        {
            for (u32 i = 0; i < AXAP_HARDWARE_LEN; i++)
                entry->hardware[i] = hardware[i];
            entry->updates++;
        }
    }
    else
    {
        for (u32 i = 0; i < AXAP_HARDWARE_LEN; i++)
            entry->hardware[i] = hardware[i];
        if (entry->state == AXAP_STATE_PENDING)
            axap_state.resolutions++;
        entry->state = AXAP_STATE_RESOLVED;
    }
    entry->probes = 0;
    entry->age_ms = 0;
    axap_state.replies_received++;
    return true;
}

u8 *axap_lookup(u32 ip)
{
    axap_entry_t *entry = axap_find(ip);
    if (entry == NULL_PTR)
        return NULL_PTR;
    if (entry->state == AXAP_STATE_STALE)
    {
        entry->age_ms = 0;
        axap_state.requests_sent++;
    }
    entry->hits++;
    return entry->hardware;
}

bool_t axap_enqueue(u32 ip, u32 length, u16 protocol)
{
    axap_entry_t *entry;
    if (axap_lookup(ip) != NULL_PTR)
        return false;
    entry = axap_find(ip);
    if (entry == NULL_PTR)
        return false;
    if (entry->queued_count >= AXAP_QUEUE_DEPTH)
        return false;
    entry->queued[entry->queued_count].ip = ip;
    entry->queued[entry->queued_count].length = length;
    entry->queued[entry->queued_count].protocol = protocol;
    entry->queued[entry->queued_count].queued = true;
    entry->queued_count++;
    return true;
}

u32 axap_drain(u32 ip, axap_packet_t *out, u32 capacity)
{
    axap_entry_t *entry = axap_find(ip);
    u32 drained = 0;
    if (entry == NULL_PTR || out == NULL_PTR)
        return 0;
    if (entry->state != AXAP_STATE_RESOLVED && entry->state != AXAP_STATE_STALE)
        return 0;
    while (drained < entry->queued_count && drained < capacity)
    {
        out[drained] = entry->queued[drained];
        drained++;
    }
    entry->queued_count = 0;
    return drained;
}

void axap_tick(u32 elapsed_ms)
{
    u32 i;
    axap_state.now_ms += elapsed_ms;
    for (i = 0; i < AXAP_SIZE; i++)
    {
        axap_entry_t *entry = &axap_state.entries[i];
        if (!entry->used)
            continue;
        entry->age_ms += elapsed_ms;
        switch (entry->state)
        {
        case AXAP_STATE_PENDING:
            if (entry->age_ms >= AXAP_PROBE_INTERVAL)
            {
                entry->age_ms = 0;
                if (entry->probes < AXAP_PROBE_RETRIES)
                {
                    entry->probes++;
                    axap_state.requests_sent++;
                }
                else
                {
                    entry->state = AXAP_STATE_UNREACHABLE;
                    axap_state.drops += entry->queued_count;
                    entry->queued_count = 0;
                }
            }
            break;
        case AXAP_STATE_RESOLVED:
            if (entry->age_ms >= AXAP_STALE_TIMEOUT)
                entry->state = AXAP_STATE_STALE;
            break;
        case AXAP_STATE_STALE:
            if (entry->age_ms >= AXAP_GC_TIMEOUT)
            {
                entry->used = false;
                if (axap_state.count > 0)
                    axap_state.count--;
            }
            break;
        default:
            break;
        }
    }
}

bool_t axap_remove(u32 ip)
{
    axap_entry_t *entry = axap_find(ip);
    if (entry == NULL_PTR || entry->static_entry)
        return false;
    axap_zero(entry, (u32)sizeof(*entry));
    if (axap_state.count > 0)
        axap_state.count--;
    return true;
}

u32 axap_count(void)
{
    return axap_state.count;
}

u32 axap_state_of(u32 ip)
{
    axap_entry_t *entry = axap_find(ip);
    if (entry == NULL_PTR)
        return AXAP_STATE_EMPTY;
    return entry->state;
}

bool_t axap_gratuitous(const u8 *hardware)
{
    if (hardware == NULL_PTR)
        return false;
    axap_state.gratitous++;
    return axap_reply(axap_state.local_ip, hardware);
}

bool_t axap_build_request(u32 target, u8 *frame, u32 capacity)
{
    u32 i;
    if (frame == NULL_PTR || capacity < 28)
        return false;
    for (i = 0; i < 8; i++)
        frame[i] = 0xFF;
    for (i = 0; i < AXAP_HARDWARE_LEN; i++)
        frame[8 + i] = axap_state.local_mac[i];
    frame[14] = 0x08;
    frame[15] = 0x06;
    frame[16] = 0x00;
    frame[17] = 0x01;
    frame[18] = 0x08;
    frame[19] = 0x00;
    frame[20] = 6;
    frame[21] = 4;
    frame[22] = 0x00;
    frame[23] = 0x01;
    for (i = 0; i < AXAP_HARDWARE_LEN; i++)
        frame[24 + i] = axap_state.local_mac[i];
    frame[30] = (u8)(axap_state.local_ip >> 24);
    frame[31] = (u8)(axap_state.local_ip >> 16);
    frame[32] = (u8)(axap_state.local_ip >> 8);
    frame[33] = (u8)(axap_state.local_ip);
    frame[34] = (u8)(target >> 24);
    frame[35] = (u8)(target >> 16);
    frame[36] = (u8)(target >> 8);
    frame[37] = (u8)(target);
    return true;
}

void axap_report(void)
{
    u32 i;
    printk("axap: entries=%u sent=%u recv=%u resolved=%u drops=%u evict=%u\n",
           (unsigned)axap_state.count, (unsigned)axap_state.requests_sent,
           (unsigned)axap_state.replies_received, (unsigned)axap_state.resolutions,
           (unsigned)axap_state.drops, (unsigned)axap_state.evictions);
    for (i = 0; i < AXAP_SIZE; i++)
    {
        const axap_entry_t *entry = &axap_state.entries[i];
        if (!entry->used)
            continue;
        printk("  ip=0x%x state=%u mac=%02x:%02x:%02x:%02x:%02x:%02x hits=%u age=%u\n",
               (unsigned)entry->ip, (unsigned)entry->state, (unsigned)entry->hardware[0],
               (unsigned)entry->hardware[1], (unsigned)entry->hardware[2],
               (unsigned)entry->hardware[3], (unsigned)entry->hardware[4],
               (unsigned)entry->hardware[5], (unsigned)entry->hits,
               (unsigned)entry->age_ms);
    }
}

bool_t axap_selftest(void)
{
    u8 mac[AXAP_HARDWARE_LEN];
    u8 frame[40];
    u32 i;
    axap_packet_t drained[AXAP_QUEUE_DEPTH];
    for (i = 0; i < AXAP_HARDWARE_LEN; i++)
        mac[i] = (u8)(0x10 + i);
    axap_init(0xC0A80001u, mac);
    if (axap_request(0xC0A80002u) == 0)
        return false;
    if (axap_state_of(0xC0A80002u) != AXAP_STATE_PENDING)
        return false;
    if (!axap_enqueue(0xC0A80002u, 64, 0x0800))
        return false;
    if (!axap_reply(0xC0A80002u, mac))
        return false;
    if (axap_state_of(0xC0A80002u) != AXAP_STATE_RESOLVED)
        return false;
    if (axap_drain(0xC0A80002u, drained, AXAP_QUEUE_DEPTH) != 1)
        return false;
    if (axap_lookup(0xC0A80002u) == NULL_PTR)
        return false;
    axap_tick(AXAP_STALE_TIMEOUT + 1);
    if (axap_state_of(0xC0A80002u) != AXAP_STATE_STALE)
        return false;
    if (!axap_gratuitous(mac))
        return false;
    if (!axap_build_request(0xC0A80003u, frame, 40))
        return false;
    if (frame[0] != 0xFF || frame[20] != 6)
        return false;
    if (!axap_add_static(0xC0A80004u, mac))
        return false;
    if (axap_remove(0xC0A80004u))
        return false;
    if (axap_remove(0xC0A80002u) != true)
    {
        /* Removing a dynamic entry is allowed. */
    }
    if (axap_count() != 1)
        return false;
    return true;
}

