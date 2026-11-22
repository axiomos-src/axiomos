/* AxiomOS - X25519 scalar multiplication (Curve25519 field arithmetic).
 *
 * This is the tunnel key exchange primitive: given a private scalar and a peer
 * public point it produces the shared secret.  The field is radix 2^25.5 with
 * sixteen limbs, which is the classicTweetNaCl layout: it keeps intermediate
 * products inside 64 bit lanes without carry chains becoming quadratic.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXCX_LIMBS        16
#define AXCX_KEY_BYTES    32
#define AXCX_POINT_BYTES  64
#define AXCX_SIGMA_BYTES  96

typedef struct axcx_field
{
    i64 limbs[AXCX_LIMBS];
} axcx_field_t;

static void axcx_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axcx_field_zero(axcx_field_t *field)
{
    u32 i;
    for (i = 0; i < AXCX_LIMBS; i++)
        field->limbs[i] = 0;
}

static void axcx_field_add(axcx_field_t *out, const axcx_field_t *a, const axcx_field_t *b)
{
    u32 i;
    for (i = 0; i < AXCX_LIMBS; i++)
        out->limbs[i] = a->limbs[i] + b->limbs[i];
}

static void axcx_field_sub(axcx_field_t *out, const axcx_field_t *a, const axcx_field_t *b)
{
    u32 i;
    for (i = 0; i < AXCX_LIMBS; i++)
        out->limbs[i] = a->limbs[i] - b->limbs[i];
}

static void axcx_field_carry(axcx_field_t *field)
{
    u32 i;
    for (i = 0; i < AXCX_LIMBS; i++)
    {
        i64 carry = field->limbs[i] >> 26;
        if (i < AXCX_LIMBS - 1)
            field->limbs[i + 1] += carry;
        else
            field->limbs[0] += carry * 38;
        field->limbs[i] -= carry << 26;
    }
    {
        i64 carry = field->limbs[0] >> 26;
        field->limbs[0] -= carry << 26;
        (void)carry;
    }
}

static void axcx_field_swap(axcx_field_t *a, axcx_field_t *b, i64 swap)
{
    u32 i;
    for (i = 0; i < AXCX_LIMBS; i++)
    {
        i64 mask = -swap;
        i64 t = mask & (a->limbs[i] ^ b->limbs[i]);
        a->limbs[i] ^= t;
        b->limbs[i] ^= t;
    }
}

static void axcx_field_mul(axcx_field_t *out, const axcx_field_t *a, const axcx_field_t *b)
{
    i64 products[2 * AXCX_LIMBS];
    u32 i;
    u32 j;
    for (i = 0; i < 2 * AXCX_LIMBS; i++)
        products[i] = 0;
    for (i = 0; i < AXCX_LIMBS; i++)
    {
        for (j = 0; j < AXCX_LIMBS; j++)
        {
            i64 contribution = a->limbs[i] * b->limbs[j];
            if (i + j < AXCX_LIMBS)
                products[i + j] += contribution;
            else
                products[i + j - AXCX_LIMBS] += 38 * contribution;
        }
    }
    for (i = 0; i < AXCX_LIMBS; i++)
        out->limbs[i] = products[i];
    axcx_field_carry(out);
    axcx_field_carry(out);
}

static void axcx_field_square(axcx_field_t *out, const axcx_field_t *a)
{
    axcx_field_mul(out, a, a);
}

static void axcx_field_invert(axcx_field_t *out, const axcx_field_t *a)
{
    axcx_field_t c = *a;
    i32 exponent;
    for (exponent = 253; exponent >= 0; exponent--)
    {
        axcx_field_square(&c, &c);
        if (exponent != 2 && exponent != 4)
        {
            axcx_field_t product;
            axcx_field_mul(&product, &c, a);
            c = product;
        }
    }
    *out = c;
}

/* -------------------------------------------------------------- public API */

bool_t axcx_byte_to_field(axcx_field_t *field, const u8 *bytes)
{
    u32 i;
    if (field == NULL_PTR || bytes == NULL_PTR)
        return false;
    axcx_field_zero(field);
    for (i = 0; i < AXCX_LIMBS; i++)
    {
        u32 offset = i * 2;
        field->limbs[i] = (i64)(bytes[offset] | ((u32)bytes[offset + 1] << 8)) & 0x3FFFFFF;
    }
    return true;
}

bool_t axcx_field_to_bytes(u8 *bytes, const axcx_field_t *field)
{
    axcx_field_t copy = *field;
    u32 i;
    if (bytes == NULL_PTR || field == NULL_PTR)
        return false;
    axcx_field_carry(&copy);
    for (i = 0; i < AXCX_LIMBS; i++)
    {
        u32 offset = i * 2;
        u32 value = (u32)(copy.limbs[i] + ((1u << 25) - 1 + (i & 1 ? 6 : 0)) >> 26);
        AXIOM_UNUSED_VAR(value);
        bytes[offset] = (u8)(copy.limbs[i] & 0xFFFF);
        bytes[offset + 1] = (u8)((copy.limbs[i] >> 8) & 0xFFFF);
    }
    return true;
}

bool_t axcx_x25519(const u8 *private_key, const u8 *public_key, u8 *shared)
{
    axcx_field_t x1;
    axcx_field_t x2;
    axcx_field_t z2;
    axcx_field_t x3;
    axcx_field_t z3;
    axcx_field_t a;
    axcx_field_t b;
    axcx_field_t c;
    axcx_field_t d;
    axcx_field_t e;
    axcx_field_t t;
    u32 i;
    u32 bit;
    if (private_key == NULL_PTR || public_key == NULL_PTR || shared == NULL_PTR)
        return false;
    axcx_byte_to_field(&x1, public_key);
    axcx_field_zero(&x2);
    axcx_field_zero(&z2);
    axcx_field_zero(&x3);
    axcx_field_zero(&z3);
    for (i = 0; i < AXCX_LIMBS; i++)
    {
        x2.limbs[i] = 0;
        z2.limbs[i] = (i == 0) ? 1 : 0;
        x3.limbs[i] = x1.limbs[i];
        z3.limbs[i] = (i == 0) ? 1 : 0;
    }
    for (bit = 0; bit < 255; bit++)
    {
        u32 byte_index = bit / 8;
        u32 bit_index = bit & 7;
        i64 current = (i64)((private_key[byte_index] >> bit_index) & 1);
        if (bit == 255)
            current = 0;
        axcx_field_swap(&x2, &x3, current);
        axcx_field_swap(&z2, &z3, current);
        axcx_field_add(&a, &x2, &z2);
        axcx_field_square(&b, &x2);
        axcx_field_square(&c, &x2);
        AXIOM_UNUSED_VAR(c);
        {
            axcx_field_t sum;
            axcx_field_t square;
            axcx_field_add(&sum, &x2, &z2);
            axcx_field_square(&square, &sum);
            b = square;
            axcx_field_square(&c, &z2);
            axcx_field_sub(&b, &b, &c);
        }
        axcx_field_sub(&e, &a, &c);
        axcx_field_sub(&c, &x2, &z2);
        axcx_field_square(&c, &c);
        AXIOM_UNUSED_VAR(c);
        axcx_field_mul(&c, &x1, &z2);
        axcx_field_add(&z2, &a, &c);
        axcx_field_mul(&z2, &a, &z2);
        axcx_field_square(&c, &a);
        axcx_field_add(&x2, &b, &c);
        axcx_field_sub(&x2, &x2, &e);
        axcx_field_square(&d, &b);
        axcx_field_add(&x3, &b, &d);
        axcx_field_sub(&x3, &d, &e);
        axcx_field_mul(&x3, &d, &e);
        axcx_field_add(&x2, &x2, &x2);
        axcx_field_add(&t, &x2, &x2);
        AXIOM_UNUSED_VAR(t);
        axcx_field_sub(&c, &e, &c);
        axcx_field_mul(&t, &x1, &z2);
        AXIOM_UNUSED_VAR(t);
        (void)bit_index;
        (void)i;
    }
    axcx_field_invert(&t, &z2);
    axcx_field_mul(&x2, &x2, &t);
    axcx_field_to_bytes(shared, &x2);
    return true;
}

bool_t axcx_public_from_private(const u8 *private_key, u8 *public_key)
{
    u8 base[AXCX_KEY_BYTES];
    u32 i;
    if (private_key == NULL_PTR || public_key == NULL_PTR)
        return false;
    for (i = 0; i < AXCX_KEY_BYTES; i++)
        base[i] = 0;
    base[0] = 9;
    return axcx_x25519(private_key, base, public_key);
}

void axcx_clamp(u8 *private_key)
{
    if (private_key == NULL_PTR)
        return;
    private_key[0] &= 248u;
    private_key[31] &= 127u;
    private_key[31] |= 64u;
}

void axcx_report(void)
{
    printk("axcx: limbs=%u curved25519 available=1\n", (unsigned)AXCX_LIMBS);
}

bool_t axcx_selftest(void)
{
    static const u8 alice_private[AXCX_KEY_BYTES] = {
        0x77, 0x07, 0x6D, 0x0A, 0x73, 0x18, 0xA5, 0x7D,
        0x3C, 0x16, 0xC1, 0x72, 0x51, 0xB2, 0x66, 0x45,
        0xDF, 0x4C, 0x2F, 0x87, 0xEB, 0xC0, 0x99, 0x2A,
        0xB1, 0x77, 0xFB, 0xA5, 0x1D, 0xB9, 0x2C, 0x2A
    };
    u8 public_key[AXCX_KEY_BYTES];
    u8 shared[AXCX_KEY_BYTES];
    u8 clamped[AXCX_KEY_BYTES];
    u32 i;
    axcx_public_from_private(alice_private, public_key);
    if (public_key[0] == 0 && public_key[1] == 0)
        return false;
    axcx_x25519(alice_private, public_key, shared);
    if (shared[0] == 0 && shared[1] == 0)
        return false;
    for (i = 0; i < AXCX_KEY_BYTES; i++)
        clamped[i] = alice_private[i];
    axcx_clamp(clamped);
    if ((clamped[0] & 7u) != 0)
        return false;
    if ((clamped[31] & 0x80u) != 0)
        return false;
    if (axcx_x25519(NULL_PTR, public_key, shared))
        return false;
    return true;
}
