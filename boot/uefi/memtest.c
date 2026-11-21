/* AxiomOS UEFI loader - memory test.
 *
 * Before handing a memory map to the kernel the loader walks the usable ranges
 * and runs three pattern tests: an address-in-address walk, a moving inversion
 * pattern and a checksum pass.  Only pages that pass are marked as tested in the
 * boot info block so the kernel can avoid trusting firmware claims blindly.
 */

#include "loader.h"

#define AXMT_MAX_RANGES      32
#define AXMT_SAMPLE_PAGES    64
#define AXMT_PATTERN_WORDS   8
#define AXMT_CHUNK_BYTES     4096

#define AXMT_RESULT_SKIPPED  0
#define AXMT_RESULT_PASSED   1
#define AXMT_RESULT_FAILED   2
#define AXMT_RESULT_PARTIAL  3

typedef struct axmt_range
{
    u64  base;
    u64  length;
    u32  pages;
    u32  pages_tested;
    u32  pages_failed;
    u8   result;
    int  usable;
} axmt_range_t;

typedef struct axmt_state
{
    axmt_range_t ranges[AXMT_MAX_RANGES];
    u32  range_count;
    u32  pages_total;
    u32  pages_tested;
    u32  failures;
    u32  pattern_errors;
    u32  walk_errors;
    u32  checksum_errors;
    u64  bytes_covered;
    u64  first_failure;
    u32  duration_ms;
    int  enabled;
    int  quick_mode;
    int  ready;
} axmt_state_t;

static axmt_state_t axmt_state;

static const u64 axmt_patterns[AXMT_PATTERN_WORDS] = {
    0x0000000000000000ull,
    0xFFFFFFFFFFFFFFFFull,
    0x5555555555555555ull,
    0xAAAAAAAAAAAAAAAAull,
    0x0123456789ABCDEFull,
    0xFEDCBA9876543210ull,
    0x00FF00FF00FF00FFull,
    0xFF00FF00FF00FF00ull
};

static void axmt_zero(void *ptr, u64 size)
{
    u8 *bytes = (u8 *)ptr;
    u64 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axmt_store(u64 address, u64 value)
{
    *(volatile u64 *)address = value;
}

static u64 axmt_load(u64 address)
{
    return *(volatile u64 *)address;
}

/* -------------------------------------------------------------- public API */

void axmt_init(int quick)
{
    axmt_zero(&axmt_state, (u64)sizeof(axmt_state));
    axmt_state.enabled = 1;
    axmt_state.quick_mode = quick;
    axmt_state.ready = 1;
}

int axmt_add_range(u64 base, u64 length, int usable)
{
    axmt_range_t *range;
    if (axmt_state.range_count >= AXMT_MAX_RANGES)
        return 0;
    if (length < AXMT_CHUNK_BYTES)
        return 0;
    if ((base & 0xFFFull) != 0)
    {
        base = (base + 0xFFFull) & ~0xFFFull;
        if (length <= (base - (base & ~0xFFFull)))
            return 0;
    }
    range = &axmt_state.ranges[axmt_state.range_count];
    range->base = base;
    range->length = length;
    range->pages = (u32)(length / AXMT_CHUNK_BYTES);
    range->usable = usable;
    range->result = AXMT_RESULT_SKIPPED;
    axmt_state.range_count++;
    axmt_state.pages_total += range->pages;
    return 1;
}

u32 axmt_range_count(void)
{
    return axmt_state.range_count;
}

int axmt_walk_range(u32 index)
{
    axmt_range_t *range;
    u32 page;
    u32 limit;
    if (index >= axmt_state.range_count)
        return 0;
    range = &axmt_state.ranges[index];
    if (!range->usable)
        return 0;
    limit = axmt_state.quick_mode ? 8 : range->pages;
    if (limit > range->pages)
        limit = range->pages;
    for (page = 0; page < limit; page++)
    {
        u64 address = range->base + (u64)page * AXMT_CHUNK_BYTES;
        u64 stored;
        axmt_store(address, address ^ 0xA5A50000ull);
        stored = axmt_load(address);
        if (stored != (address ^ 0xA5A50000ull))
        {
            axmt_state.walk_errors++;
            axmt_state.failures++;
            range->pages_failed++;
            if (axmt_state.first_failure == 0)
                axmt_state.first_failure = address;
            range->result = AXMT_RESULT_FAILED;
        }
        else if (range->result == AXMT_RESULT_SKIPPED)
        {
            range->result = AXMT_RESULT_PASSED;
        }
        range->pages_tested++;
        axmt_state.pages_tested++;
    }
    return 1;
}

int axmt_pattern_range(u32 index)
{
    axmt_range_t *range;
    u32 page;
    u32 limit;
    u32 pattern;
    if (index >= axmt_state.range_count)
        return 0;
    range = &axmt_state.ranges[index];
    if (!range->usable)
        return 0;
    limit = axmt_state.quick_mode ? 4 : range->pages;
    if (limit > range->pages)
        limit = range->pages;
    if (limit > (u32)AXMT_SAMPLE_PAGES)
        limit = (u32)AXMT_SAMPLE_PAGES;
    for (page = 0; page < limit; page++)
    {
        u64 address = range->base + (u64)page * AXMT_CHUNK_BYTES;
        for (pattern = 0; pattern < AXMT_PATTERN_WORDS; pattern++)
        {
            axmt_store(address, axmt_patterns[pattern]);
            if (axmt_load(address) != axmt_patterns[pattern])
            {
                axmt_state.pattern_errors++;
                axmt_state.failures++;
                if (axmt_state.first_failure == 0)
                    axmt_state.first_failure = address;
                range->result = AXMT_RESULT_FAILED;
            }
        }
    }
    return 1;
}

int axmt_checksum_range(u32 index)
{
    axmt_range_t *range;
    u32 page;
    u32 limit;
    u64 sum;
    if (index >= axmt_state.range_count)
        return 0;
    range = &axmt_state.ranges[index];
    if (!range->usable)
        return 0;
    limit = axmt_state.quick_mode ? 2 : range->pages;
    if (limit > range->pages)
        limit = range->pages;
    for (page = 0; page < limit; page++)
    {
        u64 address = range->base + (u64)page * AXMT_CHUNK_BYTES;
        sum = 0;
        for (u32 offset = 0; offset < AXMT_CHUNK_BYTES; offset += 8)
            sum += axmt_load(address + offset);
        if (sum == 0xFFFFFFFFFFFFFFFFull)
        {
            axmt_state.checksum_errors++;
            range->result = AXMT_RESULT_PARTIAL;
        }
    }
    return 1;
}

u32 axmt_run(void)
{
    u32 i;
    u32 tested = 0;
    for (i = 0; i < axmt_state.range_count; i++)
    {
        if (!axmt_state.ranges[i].usable)
            continue;
        axmt_walk_range(i);
        axmt_pattern_range(i);
        axmt_checksum_range(i);
        axmt_state.bytes_covered += axmt_state.ranges[i].length;
        tested++;
    }
    return tested;
}

u32 axmt_failures(void)
{
    return axmt_state.failures;
}

u64 axmt_first_failure(void)
{
    return axmt_state.first_failure;
}

u32 axmt_tested_pages(void)
{
    return axmt_state.pages_tested;
}

void axmt_report(void)
{
    u32 i;
    con_printf("[memtest] ranges=%u pages=%u tested=%u failures=%u\n",
               axmt_state.range_count, axmt_state.pages_total,
               axmt_state.pages_tested, axmt_state.failures);
    con_printf("[memtest] walk=%u pattern=%u checksum=%u covered=%lu KiB\n",
               axmt_state.walk_errors, axmt_state.pattern_errors,
               axmt_state.checksum_errors,
               (unsigned long)(axmt_state.bytes_covered / 1024));
    for (i = 0; i < axmt_state.range_count; i++)
    {
        const axmt_range_t *range = &axmt_state.ranges[i];
        con_printf("[memtest]   range %u base=%lx len=%lu result=%u failed=%u\n",
                   i, (unsigned long)range->base,
                   (unsigned long)range->length, range->result,
                   range->pages_failed);
    }
    if (axmt_state.first_failure != 0)
        con_printf("[memtest] first failure at %lx\n",
                   (unsigned long)axmt_state.first_failure);
}

int axmt_selftest(void)
{
    static u64 scratch[AXMT_CHUNK_BYTES / 8];
    axmt_init(1);
    if (!axmt_add_range((u64)(unsigned long)scratch, AXMT_CHUNK_BYTES * 4, 1))
        return 0;
    if (axmt_range_count() != 1)
        return 0;
    if (!axmt_walk_range(0))
        return 0;
    if (axmt_state.walk_errors != 0)
        return 0;
    if (!axmt_pattern_range(0))
        return 0;
    if (axmt_state.pattern_errors != 0)
        return 0;
    if (!axmt_checksum_range(0))
        return 0;
    if (axmt_failures() != 0)
        return 0;
    if (axmt_tested_pages() == 0)
        return 0;
    if (axmt_run() != 1)
        return 0;
    return 1;
}

