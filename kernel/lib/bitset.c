/* AxiomOS - fixed width bitset and bitmap helpers. */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXBS_WORDS_MAX 64

typedef struct axbs_bitset
{
    u64 words[AXBS_WORDS_MAX];
    u32 word_count;
    u32 bit_count;
} axbs_bitset_t;

static void axbs_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

bool_t axbs_init(axbs_bitset_t *set, u32 bits)
{
    u32 words;
    if (bits == 0)
        return false;
    words = (bits + 63) / 64;
    if (words > AXBS_WORDS_MAX)
        return false;
    axbs_zero(set, (u32)sizeof(*set));
    set->word_count = words;
    set->bit_count = bits;
    return true;
}

bool_t axbs_set(axbs_bitset_t *set, u32 bit)
{
    if (bit >= set->bit_count)
        return false;
    set->words[bit / 64] |= (1ull << (bit % 64));
    return true;
}

bool_t axbs_clear(axbs_bitset_t *set, u32 bit)
{
    if (bit >= set->bit_count)
        return false;
    set->words[bit / 64] &= ~(1ull << (bit % 64));
    return true;
}

bool_t axbs_test(const axbs_bitset_t *set, u32 bit)
{
    if (bit >= set->bit_count)
        return false;
    return (set->words[bit / 64] & (1ull << (bit % 64))) != 0;
}

bool_t axbs_toggle(axbs_bitset_t *set, u32 bit)
{
    if (bit >= set->bit_count)
        return false;
    set->words[bit / 64] ^= (1ull << (bit % 64));
    return axbs_test(set, bit);
}

u32 axbs_count(const axbs_bitset_t *set)
{
    u32 total = 0;
    u32 i;
    for (i = 0; i < set->word_count; i++)
    {
        u64 word = set->words[i];
        while (word != 0)
        {
            word &= word - 1;
            total++;
        }
    }
    return total;
}

u32 axbs_first_zero(const axbs_bitset_t *set)
{
    u32 i;
    for (i = 0; i < set->word_count; i++)
    {
        u64 word = set->words[i];
        u32 bit = 0;
        if (word == 0xFFFFFFFFFFFFFFFFull)
            continue;
        while (bit < 64 && (word & (1ull << bit)) != 0)
            bit++;
        {
            u32 index = i * 64 + bit;
            return index < set->bit_count ? index : 0xFFFFFFFFu;
        }
    }
    return 0xFFFFFFFFu;
}

u32 axbs_first_set(const axbs_bitset_t *set)
{
    u32 i;
    for (i = 0; i < set->word_count; i++)
    {
        u64 word = set->words[i];
        u32 bit = 0;
        if (word == 0)
            continue;
        while ((word & (1ull << bit)) == 0)
            bit++;
        {
            u32 index = i * 64 + bit;
            return index < set->bit_count ? index : 0xFFFFFFFFu;
        }
    }
    return 0xFFFFFFFFu;
}

void axbs_and(axbs_bitset_t *dst, const axbs_bitset_t *a, const axbs_bitset_t *b)
{
    u32 i;
    u32 words = dst->word_count;
    if (a->word_count < words)
        words = a->word_count;
    if (b->word_count < words)
        words = b->word_count;
    for (i = 0; i < words; i++)
        dst->words[i] = a->words[i] & b->words[i];
    for (; i < dst->word_count; i++)
        dst->words[i] = 0;
}

void axbs_or(axbs_bitset_t *dst, const axbs_bitset_t *a, const axbs_bitset_t *b)
{
    u32 i;
    u32 words = dst->word_count;
    if (a->word_count < words)
        words = a->word_count;
    if (b->word_count < words)
        words = b->word_count;
    for (i = 0; i < words; i++)
        dst->words[i] = a->words[i] | b->words[i];
    for (; i < dst->word_count; i++)
        dst->words[i] = 0;
}

void axbs_xor(axbs_bitset_t *dst, const axbs_bitset_t *a, const axbs_bitset_t *b)
{
    u32 i;
    u32 words = dst->word_count;
    if (a->word_count < words)
        words = a->word_count;
    if (b->word_count < words)
        words = b->word_count;
    for (i = 0; i < words; i++)
        dst->words[i] = a->words[i] ^ b->words[i];
    for (; i < dst->word_count; i++)
        dst->words[i] = 0;
}

void axbs_not(axbs_bitset_t *dst, const axbs_bitset_t *src)
{
    u32 i;
    u32 words = dst->word_count < src->word_count ? dst->word_count : src->word_count;
    for (i = 0; i < words; i++)
        dst->words[i] = ~src->words[i];
    for (; i < dst->word_count; i++)
        dst->words[i] = 0xFFFFFFFFFFFFFFFFull;
    {
        u32 remainder = dst->bit_count % 64;
        if (remainder != 0 && dst->word_count > 0)
            dst->words[dst->word_count - 1] &= (1ull << remainder) - 1;
    }
}

bool_t axbs_empty(const axbs_bitset_t *set)
{
    u32 i;
    for (i = 0; i < set->word_count; i++)
    {
        if (set->words[i] != 0)
            return false;
    }
    return true;
}

void axbs_clear_all(axbs_bitset_t *set)
{
    u32 i;
    for (i = 0; i < set->word_count; i++)
        set->words[i] = 0;
}

void axbs_fill(axbs_bitset_t *set)
{
    u32 i;
    for (i = 0; i < set->word_count; i++)
        set->words[i] = 0xFFFFFFFFFFFFFFFFull;
    {
        u32 remainder = set->bit_count % 64;
        if (remainder != 0 && set->word_count > 0)
            set->words[set->word_count - 1] &= (1ull << remainder) - 1;
    }
}

u32 axbs_next_zero(const axbs_bitset_t *set, u32 start)
{
    u32 bit;
    for (bit = start; bit < set->bit_count; bit++)
    {
        if (!axbs_test(set, bit))
            return bit;
    }
    return 0xFFFFFFFFu;
}

u32 axbs_run_length(const axbs_bitset_t *set, u32 start, bool_t value)
{
    u32 length = 0;
    u32 bit;
    for (bit = start; bit < set->bit_count; bit++)
    {
        if (axbs_test(set, bit) != value)
            break;
        length++;
    }
    return length;
}

u32 axbs_load_word(const axbs_bitset_t *set, u32 index)
{
    if (index >= set->word_count)
        return 0;
    return (u32)(set->words[index] & 0xFFFFFFFFull);
}

void axbs_report(const axbs_bitset_t *set)
{
    printk("axbs: bits=%u words=%u set=%u empty=%u\n", (unsigned)set->bit_count,
           (unsigned)set->word_count, (unsigned)axbs_count(set),
           (unsigned)axbs_empty(set));
}

bool_t axbs_selftest(void)
{
    axbs_bitset_t a;
    axbs_bitset_t b;
    axbs_bitset_t c;
    if (!axbs_init(&a, 256))
        return false;
    if (!axbs_init(&b, 256))
        return false;
    if (!axbs_init(&c, 256))
        return false;
    if (axbs_count(&a) != 0)
        return false;
    if (!axbs_set(&a, 5) || !axbs_set(&a, 130))
        return false;
    if (!axbs_test(&a, 5) || axbs_test(&a, 6))
        return false;
    if (axbs_count(&a) != 2)
        return false;
    if (axbs_first_set(&a) != 5)
        return false;
    if (axbs_first_zero(&a) != 0)
        return false;
    if (axbs_set(&a, 300))
        return false;
    axbs_clear(&a, 5);
    if (axbs_test(&a, 5))
        return false;
    axbs_set(&b, 7);
    axbs_or(&c, &a, &b);
    if (axbs_count(&c) != 2)
        return false;
    axbs_and(&c, &a, &b);
    if (axbs_count(&c) != 0)
        return false;
    axbs_not(&c, &a);
    if (axbs_count(&c) != 254)
        return false;
    axbs_fill(&a);
    if (axbs_count(&a) != 256)
        return false;
    axbs_clear_all(&a);
    if (!axbs_empty(&a))
        return false;
    if (axbs_next_zero(&b, 0) != 0)
        return false;
    if (axbs_run_length(&b, 7, true) != 1)
        return false;
    if (axbs_toggle(&b, 7) != false)
        return false;
    return true;
}
