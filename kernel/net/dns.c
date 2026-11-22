/* AxiomOS - DNS stub resolver with a positive and negative cache.
 *
 * The resolver manipulates wire format records only; name compression pointers
 * are followed with a hop limit to stop loops.  Caching keeps TTL in seconds and
 * expires entries lazily on lookup, so no timer is required.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXDNS_CACHE_SLOTS     32
#define AXDNS_NAME_LEN        64
#define AXDNS_MAX_LABELS      8
#define AXDNS_MAX_HOPS        16
#define AXDNS_TTL_DEFAULT     300
#define AXDNS_TTL_MAX         86400

#define AXDNS_TYPE_A          1
#define AXDNS_TYPE_NS         2
#define AXDNS_TYPE_CNAME      5
#define AXDNS_TYPE_SOA        6
#define AXDNS_TYPE_PTR        12
#define AXDNS_TYPE_AAAA       28
#define AXDNS_TYPE_SRV        33
#define AXDNS_RCODE_NOERROR   0
#define AXDNS_RCODE_FORMERR   1
#define AXDNS_RCODE_NXDOMAIN  3
#define AXDNS_RCODE_SERVFAIL  2
#define AXDNS_RCODE_REFUSED   5

#define AXDNS_FLAG_RD         0x0100
#define AXDNS_FLAG_RA         0x0080
#define AXDNS_FLAG_AA         0x0400
#define AXDNS_FLAG_TC         0x0200
#define AXDNS_FLAG_QR         0x8000

typedef struct axdns_answer
{
    char name[AXDNS_NAME_LEN];
    u32  type;
    u32  ttl;
    u32  address;
    u32  expires_at;
    u32  hits;
    bool_t negative;
    bool_t used;
} axdns_answer_t;

typedef struct axdns_state
{
    axdns_answer_t cache[AXDNS_CACHE_SLOTS];
    u32  count;
    u32  queries;
    u32  cache_hits;
    u32  cache_misses;
    u32  failures;
    u32  nxdomain;
    u32  truncations;
    u32  compressed_labels;
    u32  malformed;
    u32  now;
    u32  nameserver;
    u32  max_retries;
    u32  retry_count;
    bool_t recursion_desired;
    bool_t ready;
} axdns_state_t;

static axdns_state_t axdns_state;

static void axdns_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axdns_copy_string(char *dst, const char *src, u32 limit)
{
    u32 i = 0;
    if (dst == NULL_PTR || limit == 0)
        return;
    while (src != NULL_PTR && src[i] != 0 && i + 1 < limit)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool_t axdns_name_equal(const char *a, const char *b)
{
    u32 i = 0;
    if (a == NULL_PTR || b == NULL_PTR)
        return false;
    while (a[i] != 0 && b[i] != 0)
    {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return false;
        i++;
    }
    return a[i] == b[i];
}

static axdns_answer_t *axdns_lookup(const char *name)
{
    u32 i;
    for (i = 0; i < AXDNS_CACHE_SLOTS; i++)
    {
        axdns_answer_t *entry = &axdns_state.cache[i];
        if (!entry->used)
            continue;
        if (!axdns_name_equal(entry->name, name))
            continue;
        if (entry->expires_at != 0 && axdns_state.now > entry->expires_at)
            continue;
        return entry;
    }
    return NULL_PTR;
}

static axdns_answer_t *axdns_slot(const char *name, u32 type)
{
    u32 i;
    u32 oldest = 0xFFFFFFFFu;
    u32 sequence = 0xFFFFFFFFu;
    for (i = 0; i < AXDNS_CACHE_SLOTS; i++)
    {
        axdns_answer_t *entry = &axdns_state.cache[i];
        if (entry->used && axdns_name_equal(entry->name, name) && entry->type == type)
            return entry;
        if (!entry->used)
            return entry;
        if (entry->hits < sequence)
        {
            sequence = entry->hits;
            oldest = i;
        }
    }
    if (oldest != 0xFFFFFFFFu)
        return &axdns_state.cache[oldest];
    return NULL_PTR;
}

bool_t axdns_cache_store(const char *name, u32 type, u32 ttl, u32 address, bool_t negative);

static u32 axdns_encode_name(const char *name, u8 *out, u32 capacity)
{
    u32 written = 0;
    u32 label = 0;
    if (name == NULL_PTR || out == NULL_PTR)
        return 0;
    while (name[label] != 0 && written + 2 < capacity)
    {
        u32 length = 0;
        u32 start = written + 1;
        while (name[label] != 0 && name[label] != '.')
        {
            if (start + length >= capacity)
                return 0;
            out[start + length] = (u8)name[label];
            length++;
            label++;
        }
        if (length == 0)
            return 0;
        out[written] = (u8)length;
        written = start + length;
        if (name[label] == '.')
            label++;
    }
    if (written < capacity)
        out[written++] = 0;
    return written;
}

static u32 axdns_decode_name(const u8 *packet, u32 length, u32 offset, char *out, u32 capacity)
{
    u32 hops = 0;
    u32 written = 0;
    u32 cursor = offset;
    while (cursor < length && hops < AXDNS_MAX_HOPS)
    {
        u8 label_length = packet[cursor];
        if (label_length == 0)
        {
            cursor++;
            break;
        }
        if ((label_length & 0xC0) == 0xC0)
        {
            u16 pointer;
            if (cursor + 1 >= length)
                return 0;
            pointer = (u16)(((label_length & 0x3F) << 8) | packet[cursor + 1]);
            cursor = pointer;
            hops++;
            axdns_state.compressed_labels++;
            continue;
        }
        if (label_length > 63 || cursor + 1 + label_length > length)
        {
            axdns_state.malformed++;
            return 0;
        }
        if (written + label_length + 2 >= capacity)
            return 0;
        for (u32 i = 0; i < label_length; i++)
            out[written++] = (char)packet[cursor + 1 + i];
        out[written++] = '.';
        cursor += 1 + label_length;
    }
    if (written == 0)
    {
        out[0] = 0;
        return cursor;
    }
    out[written - 1] = 0;
    return cursor;
}

/* -------------------------------------------------------------- public API */

void axdns_init(u32 nameserver, u32 now)
{
    axdns_zero(&axdns_state, (u32)sizeof(axdns_state));
    axdns_state.nameserver = nameserver;
    axdns_state.now = now;
    axdns_state.recursion_desired = true;
    axdns_state.max_retries = 2;
    axdns_state.ready = true;
}

u32 axdns_build_query(const char *name, u32 type, u8 *packet, u32 capacity)
{
    u32 name_length;
    u32 total;
    if (packet == NULL_PTR || capacity < 12)
        return 0;
    packet[0] = (u8)(axdns_state.queries & 0xFF);
    packet[1] = (u8)((axdns_state.queries >> 8) & 0x03);
    packet[2] = axdns_state.recursion_desired ? 0x01 : 0x00;
    packet[3] = 0x00;
    packet[4] = 0x00;
    packet[5] = 0x01;
    packet[6] = 0;
    packet[7] = 0;
    packet[8] = 0;
    packet[9] = 0;
    packet[10] = 0;
    packet[11] = 0;
    name_length = axdns_encode_name(name, packet + 12, capacity - 12);
    if (name_length == 0)
        return 0;
    total = 12 + name_length;
    if (total + 4 > capacity)
        return 0;
    packet[total] = (u8)(type >> 8);
    packet[total + 1] = (u8)(type & 0xFF);
    packet[total + 2] = 0;
    packet[total + 3] = 1;
    axdns_state.queries++;
    return total + 4;
}

u32 axdns_parse_response(const u8 *packet, u32 length, u32 *out_address, u32 *out_ttl)
{
    u16 flags;
    u16 questions;
    u16 answers;
    u32 offset = 12;
    u32 count = 0;
    char name[AXDNS_NAME_LEN];
    if (packet == NULL_PTR || length < 12)
    {
        axdns_state.malformed++;
        return 0;
    }
    flags = (u16)((packet[2] << 8) | packet[3]);
    questions = (u16)((packet[4] << 8) | packet[5]);
    answers = (u16)((packet[6] << 8) | packet[7]);
    if ((flags & AXDNS_FLAG_QR) == 0)
    {
        axdns_state.malformed++;
        return 0;
    }
    if (flags & AXDNS_FLAG_TC)
        axdns_state.truncations++;
    switch (flags & 0x0F)
    {
    case AXDNS_RCODE_NOERROR:
        break;
    case AXDNS_RCODE_NXDOMAIN:
        axdns_state.nxdomain++;
        return 0;
    case AXDNS_RCODE_SERVFAIL:
    case AXDNS_RCODE_REFUSED:
    case AXDNS_RCODE_FORMERR:
    default:
        axdns_state.failures++;
        return 0;
    }
    for (u32 i = 0; i < questions; i++)
    {
        offset = axdns_decode_name(packet, length, offset, name, AXDNS_NAME_LEN);
        if (offset == 0)
            return 0;
        offset += 4;
    }
    for (u32 i = 0; i < answers && offset + 12 <= length; i++)
    {
        u16 type;
        u16 class_code;
        u32 ttl;
        u16 data_length;
        offset = axdns_decode_name(packet, length, offset, name, AXDNS_NAME_LEN);
        if (offset == 0 || offset + 10 > length)
            return 0;
        type = (u16)((packet[offset] << 8) | packet[offset + 1]);
        class_code = (u16)((packet[offset + 2] << 8) | packet[offset + 3]);
        ttl = ((u32)packet[offset + 4] << 24) | ((u32)packet[offset + 5] << 16) |
              ((u32)packet[offset + 6] << 8) | (u32)packet[offset + 7];
        data_length = (u16)((packet[offset + 8] << 8) | packet[offset + 9]);
        offset += 10;
        if (type == AXDNS_TYPE_A && data_length == 4 && offset + 4 <= length)
        {
            u32 address = ((u32)packet[offset] << 24) | ((u32)packet[offset + 1] << 16) |
                          ((u32)packet[offset + 2] << 8) | (u32)packet[offset + 3];
            if (out_address != NULL_PTR && count == 0)
                *out_address = address;
            if (out_ttl != NULL_PTR && count == 0)
                *out_ttl = ttl;
            axdns_cache_store(name, AXDNS_TYPE_A, ttl, address, false);
            count++;
        }
        else if (type == AXDNS_TYPE_CNAME)
        {
            char target[AXDNS_NAME_LEN];
            axdns_decode_name(packet, length, offset, target, AXDNS_NAME_LEN);
            axdns_cache_store(name, AXDNS_TYPE_CNAME, ttl, 0, false);
            AXIOM_UNUSED_VAR(target);
        }
        if (class_code != 1)
            count += 0;
        offset += data_length;
    }
    return count;
}

bool_t axdns_cache_store(const char *name, u32 type, u32 ttl, u32 address, bool_t negative)
{
    axdns_answer_t *entry = axdns_slot(name, type);
    if (entry == NULL_PTR || name == NULL_PTR)
        return false;
    if (!entry->used)
        axdns_state.count++;
    axdns_copy_string(entry->name, name, AXDNS_NAME_LEN);
    entry->type = type;
    entry->ttl = ttl > AXDNS_TTL_MAX ? AXDNS_TTL_MAX : ttl;
    entry->address = address;
    entry->negative = negative;
    entry->expires_at = axdns_state.now + entry->ttl;
    entry->hits = 0;
    entry->used = true;
    return true;
}

bool_t axdns_resolve_cached(const char *name, u32 *address)
{
    axdns_answer_t *entry = axdns_lookup(name);
    if (entry == NULL_PTR)
    {
        axdns_state.cache_misses++;
        return false;
    }
    if (entry->negative)
    {
        axdns_state.cache_hits++;
        return false;
    }
    entry->hits++;
    axdns_state.cache_hits++;
    if (address != NULL_PTR)
        *address = entry->address;
    return true;
}

bool_t axdns_flush(const char *name)
{
    u32 i;
    u32 removed = 0;
    for (i = 0; i < AXDNS_CACHE_SLOTS; i++)
    {
        axdns_answer_t *entry = &axdns_state.cache[i];
        if (!entry->used)
            continue;
        if (name != NULL_PTR && !axdns_name_equal(entry->name, name))
            continue;
        axdns_zero(entry, (u32)sizeof(*entry));
        removed++;
    }
    if (axdns_state.count > removed)
        axdns_state.count -= removed;
    else
        axdns_state.count = 0;
    return removed != 0;
}

void axdns_advance(u32 seconds)
{
    axdns_state.now += seconds;
}

u32 axdns_entry_count(void)
{
    return axdns_state.count;
}

u32 axdns_expire(void)
{
    u32 i;
    u32 expired = 0;
    for (i = 0; i < AXDNS_CACHE_SLOTS; i++)
    {
        axdns_answer_t *entry = &axdns_state.cache[i];
        if (!entry->used)
            continue;
        if (entry->expires_at != 0 && axdns_state.now > entry->expires_at)
        {
            axdns_zero(entry, (u32)sizeof(*entry));
            expired++;
        }
    }
    if (axdns_state.count > expired)
        axdns_state.count -= expired;
    else
        axdns_state.count = 0;
    return expired;
}

u32 axdns_type_name(u32 type)
{
    switch (type)
    {
    case AXDNS_TYPE_A:    return 'A';
    case AXDNS_TYPE_NS:   return 'N';
    case AXDNS_TYPE_CNAME:return 'C';
    case AXDNS_TYPE_SOA:  return 'S';
    case AXDNS_TYPE_PTR:  return 'P';
    case AXDNS_TYPE_AAAA: return '6';
    case AXDNS_TYPE_SRV:  return 'V';
    default:              return '?';
    }
}

void axdns_report(void)
{
    u32 i;
    printk("axdns: entries=%u queries=%u hits=%u misses=%u nx=%u fail=%u trunc=%u\n",
           (unsigned)axdns_state.count, (unsigned)axdns_state.queries,
           (unsigned)axdns_state.cache_hits, (unsigned)axdns_state.cache_misses,
           (unsigned)axdns_state.nxdomain, (unsigned)axdns_state.failures,
           (unsigned)axdns_state.truncations);
    for (i = 0; i < AXDNS_CACHE_SLOTS; i++)
    {
        const axdns_answer_t *entry = &axdns_state.cache[i];
        if (!entry->used)
            continue;
        printk("  %s type=%c ttl=%u addr=0x%x hits=%u expired=%u\n", entry->name,
               (char)axdns_type_name(entry->type), (unsigned)entry->ttl,
               (unsigned)entry->address, (unsigned)entry->hits,
               (unsigned)(axdns_state.now > entry->expires_at));
    }
}

bool_t axdns_selftest(void)
{
    u8 packet[256];
    u32 length;
    u32 address = 0;
    u32 ttl = 0;
    u32 count;
    axdns_init(0x0A000001u, 1000);
    length = axdns_build_query("axiomos.local", AXDNS_TYPE_A, packet, 256);
    if (length < 12)
        return false;
    packet[2] |= 0x80;
    packet[3] |= 0x80;
    packet[6] = 0;
    packet[7] = 1;
    packet[length] = 0xC0;
    packet[length + 1] = 0x0C;
    packet[length + 2] = 0x00;
    packet[length + 3] = 0x01;
    packet[length + 4] = 0x00;
    packet[length + 5] = 0x01;
    packet[length + 6] = 0x00;
    packet[length + 7] = 0x00;
    packet[length + 8] = 0x00;
    packet[length + 9] = 0x3C;
    packet[length + 10] = 0x00;
    packet[length + 11] = 0x04;
    packet[length + 12] = 0x7F;
    packet[length + 13] = 0x00;
    packet[length + 14] = 0x00;
    packet[length + 15] = 0x01;
    count = axdns_parse_response(packet, length + 16, &address, &ttl);
    if (count != 1)
        return false;
    if (address != 0x7F000001u)
        return false;
    if (ttl != 60)
        return false;
    if (axdns_entry_count() == 0)
        return false;
    if (!axdns_resolve_cached("axiomos.local", &address))
        return false;
    if (address != 0x7F000001u)
        return false;
    if (!axdns_cache_store("missing.local", AXDNS_TYPE_A, 10, 0, true))
        return false;
    if (axdns_resolve_cached("missing.local", &address))
        return false;
    axdns_advance(70);
    if (axdns_expire() == 0)
        return false;
    if (!axdns_flush(NULL_PTR))
    {
        /* Cache may already be empty after expiry. */
    }
    if (axdns_state.malformed != 0)
        return false;
    return true;
}


