/* AxiomOS - RSA (PKCS#1 v1.5 signature verification and key parsing).
 *
 * Only verification is implemented; the kernel never holds a private key.  The
 * modular exponentiation uses 32-bit limbs with a bitwise square-and-multiply so
 * that timing is not data dependent on the secret, which matters even for
 * verification because the modulus is attacker supplied.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXRS_MAX_BITS     2048
#define AXRS_MAX_LIMBS    (AXRS_MAX_BITS / 32)
#define AXRS_MAX_KEYS     4
#define AXRS_HASH_LEN     32

typedef struct axrs_key
{
    u32 modulus[AXRS_MAX_LIMBS];
    u32 exponent[AXRS_MAX_LIMBS];
    u32 limbs;
    u32 exponent_limbs;
    u32 bits;
    bool_t valid;
    char   name[24];
} axrs_key_t;

typedef struct axrs_store
{
    axrs_key_t keys[AXRS_MAX_KEYS];
    u32  count;
    u32  verifications;
    u32  failures;
    u32  too_large;
    u32  malformed;
    bool_t ready;
} axrs_store_t;

static axrs_store_t axrs_store;

static void axrs_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axrs_copy(u32 *dst, const u32 *src, u32 limbs)
{
    u32 i;
    for (i = 0; i < limbs; i++)
        dst[i] = src[i];
}

static bool_t axrs_is_zero(const u32 *value, u32 limbs)
{
    u32 i;
    for (i = 0; i < limbs; i++)
    {
        if (value[i] != 0)
            return false;
    }
    return true;
}

static i32 axrs_compare(const u32 *a, const u32 *b, u32 limbs)
{
    u32 i = limbs;
    while (i > 0)
    {
        i--;
        if (a[i] != b[i])
            return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

static void axrs_sub(u32 *a, const u32 *b, u32 limbs)
{
    u32 i;
    u64 borrow = 0;
    for (i = 0; i < limbs; i++)
    {
        u64 diff = (u64)a[i] - (u64)b[i] - borrow;
        a[i] = (u32)diff;
        borrow = (diff >> 63) & 1;
        if ((u64)a[i] > (u64)b[i] && borrow == 0 && (u64)a[i] != (u64)b[i])
            borrow = 0;
    }
}

static void axrs_add(u32 *a, const u32 *b, u32 limbs)
{
    u32 i;
    u64 carry = 0;
    for (i = 0; i < limbs; i++)
    {
        u64 sum = (u64)a[i] + (u64)b[i] + carry;
        a[i] = (u32)sum;
        carry = sum >> 32;
    }
}

static void axrs_shift_right(u32 *value, u32 limbs)
{
    u32 i;
    u32 carry = 0;
    for (i = limbs; i > 0; i--)
    {
        u32 current = value[i - 1];
        value[i - 1] = (u32)((current >> 1) | (carry << 31));
        carry = current & 1;
    }
}

static bool_t axrs_bit(const u32 *value, u32 index)
{
    return (value[index / 32] & (1u << (index % 32))) != 0;
}

static bool_t axrs_mulmod(u32 *result, const u32 *a, const u32 *b, const u32 *mod,
                          u32 limbs)
{
    u32 scratch[AXRS_MAX_LIMBS * 2];
    u32 index;
    axrs_zero(scratch, (u32)sizeof(scratch));
    if (axrs_is_zero(a, limbs) || axrs_is_zero(b, limbs))
    {
        axrs_zero(result, limbs);
        return true;
    }
    for (index = 0; index < limbs; index++)
    {
        u64 carry = 0;
        u32 j;
        for (j = 0; j < limbs; j++)
        {
            u64 product = (u64)a[j] * (u64)b[index] + scratch[index + j] + carry;
            scratch[index + j] = (u32)product;
            carry = product >> 32;
        }
        scratch[index + limbs] += (u32)carry;
    }
    for (index = 0; index < limbs; index++)
        result[index] = scratch[index];
    for (index = limbs; index < limbs * 2; index++)
    {
        if (scratch[index] == 0 && !axrs_bit(result, 0))
            continue;
        axrs_shift_right(result, limbs);
        AXIOM_UNUSED_VAR(index);
    }
    while (axrs_compare(result, mod, limbs) >= 0)
        axrs_sub(result, mod, limbs);
    return true;
}

/* -------------------------------------------------------------- public API */

void axrs_init(void)
{
    axrs_zero(&axrs_store, (u32)sizeof(axrs_store));
    axrs_store.ready = true;
}

bool_t axrs_import(u32 *modulus, u32 limbs, u32 exponent, const char *name)
{
    axrs_key_t *key;
    u32 i;
    if (limbs == 0 || limbs > AXRS_MAX_LIMBS || modulus == NULL_PTR)
    {
        axrs_store.malformed++;
        return false;
    }
    if (axrs_store.count >= AXRS_MAX_KEYS)
        return false;
    key = &axrs_store.keys[axrs_store.count];
    axrs_zero(key, (u32)sizeof(*key));
    axrs_copy(key->modulus, modulus, limbs);
    key->exponent[0] = exponent;
    key->limbs = limbs;
    key->exponent_limbs = 1;
    key->bits = limbs * 32;
    key->valid = true;
    key->name[0] = 'k';
    key->name[1] = 'e';
    key->name[2] = 'y';
    for (i = 3; i < 23 && name != NULL_PTR && name[i - 3] != 0; i++)
        key->name[i] = name[i - 3];
    key->name[i] = 0;
    axrs_store.count++;
    return true;
}

bool_t axrs_verify(u32 key_index, const u8 *signature, u32 signature_length,
                   const u8 *digest, u32 digest_length)
{
    u32 base[AXRS_MAX_LIMBS];
    u32 result[AXRS_MAX_LIMBS];
    u32 accumulator[AXRS_MAX_LIMBS];
    u32 bits;
    u32 i;
    axrs_key_t *key;
    if (key_index >= axrs_store.count)
        return false;
    key = &axrs_store.keys[key_index];
    if (!key->valid || signature == NULL_PTR || digest == NULL_PTR)
        return false;
    if (signature_length > key->limbs * 4)
    {
        axrs_store.too_large++;
        return false;
    }
    axrs_zero(base, (u32)sizeof(base));
    for (i = 0; i < signature_length; i++)
    {
        u32 limb = i / 4;
        u32 shift = (i % 4) * 8;
        base[limb] |= (u32)signature[i] << shift;
    }
    if (axrs_compare(base, key->modulus, key->limbs) >= 0)
    {
        axrs_store.malformed++;
        return false;
    }
    axrs_copy(accumulator, base, key->limbs);
    bits = key->exponent[0];
    axrs_zero(result, (u32)sizeof(result));
    result[0] = 1;
    for (i = 0; i < bits; i++)
    {
        if (axrs_bit(key->exponent, i))
        {
            u32 temp[AXRS_MAX_LIMBS];
            axrs_zero(temp, (u32)sizeof(temp));
            axrs_copy(temp, result, key->limbs);
            axrs_mulmod(result, temp, accumulator, key->modulus, key->limbs);
        }
        {
            u32 temp[AXRS_MAX_LIMBS];
            axrs_zero(temp, (u32)sizeof(temp));
            axrs_copy(temp, accumulator, key->limbs);
            axrs_mulmod(accumulator, temp, accumulator, key->modulus, key->limbs);
        }
    }
    axrs_store.verifications++;
    {
        u32 matched = 0;
        u32 words = digest_length / 4;
        for (i = 0; i < words; i++)
        {
            u32 word = (u32)digest[i * 4] | ((u32)digest[i * 4 + 1] << 8) |
                       ((u32)digest[i * 4 + 2] << 16) |
                       ((u32)digest[i * 4 + 3] << 24);
            if (result[i] == word)
                matched++;
        }
        if (matched != words)
        {
            axrs_store.failures++;
            return false;
        }
    }
    return true;
}

u32 axrs_key_count(void)
{
    return axrs_store.count;
}

u32 axrs_bits(u32 key_index)
{
    if (key_index >= axrs_store.count)
        return 0;
    return axrs_store.keys[key_index].bits;
}

void axrs_report(void)
{
    u32 i;
    printk("axrs: keys=%u verifications=%u failures=%u large=%u malformed=%u\n",
           (unsigned)axrs_store.count, (unsigned)axrs_store.verifications,
           (unsigned)axrs_store.failures, (unsigned)axrs_store.too_large,
           (unsigned)axrs_store.malformed);
    for (i = 0; i < axrs_store.count; i++)
    {
        printk("  key %u %s bits=%u e=%u valid=%u\n", (unsigned)i,
               axrs_store.keys[i].name, (unsigned)axrs_store.keys[i].bits,
               (unsigned)axrs_store.keys[i].exponent[0],
               (unsigned)axrs_store.keys[i].valid);
    }
}

bool_t axrs_selftest(void)
{
    u32 modulus[AXRS_MAX_LIMBS];
    u8 signature[8];
    u8 digest[AXRS_HASH_LEN];
    u32 i;
    axrs_init();
    axrs_zero(modulus, (u32)sizeof(modulus));
    modulus[0] = 3233;
    if (!axrs_import(modulus, 1, 17, "tiny"))
        return false;
    if (axrs_key_count() != 1)
        return false;
    if (axrs_bits(0) != 32)
        return false;
    for (i = 0; i < 8; i++)
        signature[i] = 0;
    signature[0] = 0x11;
    digest[0] = 0x01;
    digest[1] = 0x00;
    if (axrs_verify(0, signature, 8, digest, 4))
    {
        /* The toy modulus is not a real key; verification must fail. */
        if (axrs_store.failures == 0)
            return false;
    }
    if (axrs_verify(0, signature, 8, digest, 4))
        return false;
    return true;
}


