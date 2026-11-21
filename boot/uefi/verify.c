/* AxiomOS UEFI loader - image verification and measurement.
 *
 * The loader refuses to hand control to a kernel image whose digest does not
 * match the manifest shipped alongside it.  Digests are computed with a compact
 * SHA-256 implementation kept local to the boot path (no kernel dependencies),
 * and the manifest is parsed from a plain text file so a developer build can be
 * pointed at new binaries without rebuilding the loader.
 *
 * The same routine records a measurement chain: each stage appends the digest
 * of the previous stage so the kernel can display what it was loaded from.
 */

#include "loader.h"

#define AXVR_MAGIC_MANIFEST    "axiomos-manifest"
#define AXVR_MAX_ENTRIES       16
#define AXVR_NAME_LENGTH       48
#define AXVR_DIGEST_LENGTH     32
#define AXVR_MAX_FILE_BYTES    (32u * 1024u * 1024u)
#define AXVR_BLOCK_BYTES       4096

#define AXVR_RESULT_OK         0
#define AXVR_RESULT_MISMATCH   1
#define AXVR_RESULT_ABSENT     2
#define AXVR_RESULT_TOO_LARGE  3
#define AXVR_RESULT_BAD_FORMAT 4
#define AXVR_RESULT_READ_ERROR 5

struct axvr_sha256
{
    u32 state[8];
    u32 total;
    u32 used;
    u8  block[64];
};

struct axvr_entry
{
    char name[AXVR_NAME_LENGTH];
    u8   digest[AXVR_DIGEST_LENGTH];
    u32  size;
    int  strict;
    int  checked;
    int  result;
};

struct axvr_state
{
    struct axvr_entry entries[AXVR_MAX_ENTRIES];
    u8   chain[AXVR_DIGEST_LENGTH];
    u32  entry_count;
    u32  passed;
    u32  failed;
    u32  skipped;
    u32  bytes_hashed;
    u32  manifest_lines;
    u32  chain_updates;
    int  strict_mode;
    int  manifest_found;
};

static struct axvr_state axvr_state;

static const u32 axvr_round_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcFu, 0xe9b5dba5u,
    0x3956c25Bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static u32 axvr_rotr(u32 value, u32 count)
{
    return (value >> count) | (value << (32 - count));
}

static void axvr_sha256_init(struct axvr_sha256 *context)
{
    context->state[0] = 0x6a09e667u;
    context->state[1] = 0xbb67ae85u;
    context->state[2] = 0x3c6ef372u;
    context->state[3] = 0xa54ff53au;
    context->state[4] = 0x510e527fu;
    context->state[5] = 0x9b05688cu;
    context->state[6] = 0x1f83d9abu;
    context->state[7] = 0x5be0cd19u;
    context->total = 0;
    context->used = 0;
}

static void axvr_sha256_block(struct axvr_sha256 *context, const u8 *block)
{
    u32 schedule[64];
    u32 a;
    u32 b;
    u32 c;
    u32 d;
    u32 e;
    u32 f;
    u32 g;
    u32 h;
    for (u32 i = 0; i < 16; i++)
    {
        schedule[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16) |
                      ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
    }
    for (u32 i = 16; i < 64; i++)
    {
        u32 s0 = axvr_rotr(schedule[i - 15], 7) ^ axvr_rotr(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        u32 s1 = axvr_rotr(schedule[i - 2], 17) ^ axvr_rotr(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (u32 i = 0; i < 64; i++)
    {
        u32 s1 = axvr_rotr(e, 6) ^ axvr_rotr(e, 11) ^ axvr_rotr(e, 25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 temp1 = h + s1 + ch + axvr_round_constants[i] + schedule[i];
        u32 s0 = axvr_rotr(a, 2) ^ axvr_rotr(a, 13) ^ axvr_rotr(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
    context->used = 0;
}

static void axvr_sha256_update(struct axvr_sha256 *context, const u8 *data, u32 length)
{
    context->total += length;
    while (length > 0)
    {
        u32 room = 64 - context->used;
        u32 chunk = length < room ? length : room;
        for (u32 i = 0; i < chunk; i++)
            context->block[context->used + i] = data[i];
        context->used += chunk;
        data += chunk;
        length -= chunk;
        if (context->used == 64)
            axvr_sha256_block(context, context->block);
    }
}

static void axvr_sha256_final(struct axvr_sha256 *context, u8 *digest)
{
    u32 total = context->total;
    u32 used = context->used;
    u8 padding[64];
    for (u32 i = 0; i < 64; i++)
        padding[i] = 0;
    padding[0] = 0x80;
    if (used < 56)
        axvr_sha256_update(context, padding, 56 - used);
    else
        axvr_sha256_update(context, padding, 120 - used);
    u32 high = total >> 29;
    u32 low = total << 3;
    u8 tail[8];
    tail[0] = (u8)(high >> 24);
    tail[1] = (u8)(high >> 16);
    tail[2] = (u8)(high >> 8);
    tail[3] = (u8)high;
    tail[4] = (u8)(low >> 24);
    tail[5] = (u8)(low >> 16);
    tail[6] = (u8)(low >> 8);
    tail[7] = (u8)low;
    axvr_sha256_update(context, tail, 8);
    for (u32 i = 0; i < 8; i++)
    {
        digest[i * 4 + 0] = (u8)(context->state[i] >> 24);
        digest[i * 4 + 1] = (u8)(context->state[i] >> 16);
        digest[i * 4 + 2] = (u8)(context->state[i] >> 8);
        digest[i * 4 + 3] = (u8)context->state[i];
    }
}

u32 axvr_sha256(const u8 *data, u32 length, u8 *digest)
{
    struct axvr_sha256 context;
    axvr_sha256_init(&context);
    axvr_sha256_update(&context, data, length);
    axvr_sha256_final(&context, digest);
    return AXVR_DIGEST_LENGTH;
}

/* --------------------------------------------------------------- manifest */

static int axvr_hex_digit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static void axvr_copy_name(char *target, const char *source, u32 capacity)
{
    u32 i = 0;
    for (; i + 1 < capacity && source[i] != 0; i++)
        target[i] = source[i];
    target[i] = 0;
}

static int axvr_parse_digest(const char *text, u32 length, u8 *digest)
{
    if (length < AXVR_DIGEST_LENGTH * 2)
        return 0;
    for (u32 i = 0; i < AXVR_DIGEST_LENGTH; i++)
    {
        int high = axvr_hex_digit(text[i * 2]);
        int low = axvr_hex_digit(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return 0;
        digest[i] = (u8)((high << 4) | low);
    }
    return 1;
}

u32 axvr_parse_manifest(const char *text, u32 length)
{
    u32 offset = 0;
    u32 line = 0;
    axvr_state.entry_count = 0;
    axvr_state.passed = 0;
    axvr_state.failed = 0;
    axvr_state.skipped = 0;
    axvr_state.manifest_lines = 0;
    while (offset < length && axvr_state.entry_count < AXVR_MAX_ENTRIES)
    {
        u32 start = offset;
        u32 columns[4];
        u32 column_count = 0;
        while (offset < length && text[offset] != '\n')
            offset++;
        u32 end = offset;
        if (offset < length)
            offset++;
        line++;
        if (end == start || text[start] == '#')
            continue;
        if (axvr_state.manifest_lines == 0)
        {
            /* First content line must be the magic token. */
            u32 magic = 0;
            while (magic < end - start && magic < 16 && text[start + magic] == AXVR_MAGIC_MANIFEST[magic])
                magic++;
            if (magic < 16)
                return 0;
            axvr_state.manifest_lines = 1;
            continue;
        }
        for (u32 i = start; i < end && column_count < 4; i++)
        {
            if (text[i] == ' ' || text[i] == '\t' || text[i] == '=')
            {
                if (i > start && column_count < 4)
                    columns[column_count++] = i;
            }
        }
        /* Expect: name <space> digest <space> size */
        u32 name_end = start;
        while (name_end < end && text[name_end] != ' ' && text[name_end] != '\t')
            name_end++;
        u32 digest_start = name_end;
        while (digest_start < end && (text[digest_start] == ' ' || text[digest_start] == '\t'))
            digest_start++;
        u32 digest_end = digest_start;
        while (digest_end < end && text[digest_end] != ' ' && text[digest_end] != '\t')
            digest_end++;
        if (digest_end - digest_start < AXVR_DIGEST_LENGTH * 2)
            continue;
        struct axvr_entry *entry = &axvr_state.entries[axvr_state.entry_count];
        for (u32 i = 0; i < (u32)sizeof(*entry); i++)
            ((u8 *)entry)[i] = 0;
        axvr_copy_name(entry->name, text + start, name_end - start + 1);
        if (!axvr_parse_digest(text + digest_start, digest_end - digest_start, entry->digest))
            continue;
        entry->size = 0;
        entry->strict = 1;
        axvr_state.entry_count++;
        axvr_state.manifest_lines++;
        (void)columns;
        (void)line;
    }
    axvr_state.manifest_found = 1;
    return axvr_state.entry_count;
}

void axvr_chain_extend(const u8 *digest);

/* -------------------------------------------------------------- verdicts */

int axvr_check(const char *name, const u8 *data, u32 length)
{
    u8 computed[AXVR_DIGEST_LENGTH];
    struct axvr_entry *entry = 0;
    if (name == 0 || data == 0)
        return AXVR_RESULT_READ_ERROR;
    for (u32 i = 0; i < axvr_state.entry_count; i++)
    {
        struct axvr_entry *candidate = &axvr_state.entries[i];
        u32 equal = 1;
        for (u32 c = 0; candidate->name[c] != 0 || name[c] != 0; c++)
        {
            if (candidate->name[c] != name[c])
            {
                equal = 0;
                break;
            }
        }
        if (equal)
        {
            entry = candidate;
            break;
        }
    }
    if (entry == 0)
    {
        axvr_state.skipped++;
        return AXVR_RESULT_ABSENT;
    }
    if (length > AXVR_MAX_FILE_BYTES)
    {
        entry->checked = 1;
        entry->result = AXVR_RESULT_TOO_LARGE;
        axvr_state.failed++;
        return AXVR_RESULT_TOO_LARGE;
    }
    axvr_sha256(data, length, computed);
    axvr_state.bytes_hashed += length;
    entry->checked = 1;
    for (u32 i = 0; i < AXVR_DIGEST_LENGTH; i++)
    {
        if (computed[i] != entry->digest[i])
        {
            entry->result = AXVR_RESULT_MISMATCH;
            axvr_state.failed++;
            return AXVR_RESULT_MISMATCH;
        }
    }
    entry->result = AXVR_RESULT_OK;
    axvr_state.passed++;
    axvr_chain_extend(computed);
    return AXVR_RESULT_OK;
}

void axvr_chain_extend(const u8 *digest)
{
    u8 merged[64];
    struct axvr_sha256 context;
    for (u32 i = 0; i < AXVR_DIGEST_LENGTH; i++)
    {
        merged[i] = axvr_state.chain[i];
        merged[AXVR_DIGEST_LENGTH + i] = digest[i];
    }
    axvr_sha256_init(&context);
    axvr_sha256_update(&context, merged, 64);
    axvr_sha256_final(&context, axvr_state.chain);
    axvr_state.chain_updates++;
}

void axvr_chain_reset(void)
{
    for (u32 i = 0; i < AXVR_DIGEST_LENGTH; i++)
        axvr_state.chain[i] = 0;
    axvr_state.chain_updates = 0;
}

const u8 *axvr_chain(void)
{
    return axvr_state.chain;
}

int axvr_strict(void)
{
    if (axvr_state.failed != 0)
        return 0;
    if (axvr_state.strict_mode && axvr_state.skipped != 0)
        return 0;
    return 1;
}

void axvr_set_strict(int strict)
{
    axvr_state.strict_mode = strict ? 1 : 0;
}

u32 axvr_passed(void)
{
    return axvr_state.passed;
}

u32 axvr_failed(void)
{
    return axvr_state.failed;
}

u32 axvr_entry_count(void)
{
    return axvr_state.entry_count;
}

void axvr_report(void)
{
    con_puts("verify: entries=");
    con_dec(axvr_state.entry_count);
    con_puts(" passed=");
    con_dec(axvr_state.passed);
    con_puts(" failed=");
    con_dec(axvr_state.failed);
    con_puts(" skipped=");
    con_dec(axvr_state.skipped);
    con_puts(" bytes=");
    con_dec(axvr_state.bytes_hashed);
    con_puts(" strict=");
    con_dec(axvr_state.strict_mode);
    con_puts("\r\n");
    for (u32 i = 0; i < axvr_state.entry_count; i++)
    {
        struct axvr_entry *entry = &axvr_state.entries[i];
        con_puts("  ");
        con_puts(entry->name);
        con_puts(" -> ");
        con_dec((u32)entry->result);
        con_puts("\r\n");
    }
    con_puts("  chain=");
    for (u32 i = 0; i < 8; i++)
        con_hex(axvr_state.chain[i]);
    con_puts("...\r\n");
}

int axvr_selftest(void)
{
    u8 digest[AXVR_DIGEST_LENGTH];
    const char *empty = "";
    axvr_chain_reset();
    axvr_set_strict(1);
    /* SHA-256 of the empty string. */
    axvr_sha256((const u8 *)empty, 0, digest);
    if (digest[0] != 0xE3 || digest[1] != 0xB0 || digest[2] != 0xC4 || digest[3] != 0x42)
        return 0;
    axvr_sha256((const u8 *)"abc", 3, digest);
    if (digest[0] != 0xBA || digest[1] != 0x78 || digest[2] != 0x16 || digest[3] != 0xBF)
        return 0;
    if (axvr_hex_digit('f') != 15)
        return 0;
    if (axvr_hex_digit('G') != -1)
        return 0;
    if (axvr_parse_manifest("bad magic\n", 10) != 0)
        return 0;
    if (axvr_parse_manifest("axiomos-manifest\nsplash ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad 3\n", 96) == 0)
        return 0;
    if (axvr_check("splash", (const u8 *)"abc", 3) != AXVR_RESULT_OK)
        return 0;
    if (axvr_check("splash", (const u8 *)"abd", 3) != AXVR_RESULT_MISMATCH)
        return 0;
    axvr_chain_reset();
    axvr_check("unknown", (const u8 *)"abc", 3);
    if (axvr_strict())
        return 0;
    axvr_state.failed = 0;
    axvr_state.skipped = 0;
    if (!axvr_strict())
        return 0;
    axvr_chain_extend(digest);
    if (axvr_state.chain_updates != 1)
        return 0;
    return 1;
}

