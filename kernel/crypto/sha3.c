/* AxiomOS - SHA-3 / Keccak sponge.
 *
 * Implements Keccak-f[1600] with the standard rotation offsets, and the four
 * digest sizes used by the kernel: SHA3-256, SHA3-384, SHA3-512 and the
 * pre-standard SHAKE128/256 output squeezes.  The permutation is written with
 * explicit lane variables so it stays register friendly without SSE.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXS3_ROUNDS          24
#define AXS3_LANE_COUNT      25
#define AXS3_RATE_256        136
#define AXS3_RATE_384        104
#define AXS3_RATE_512        72
#define AXS3_RATE_SHAKE128   168
#define AXS3_RATE_SHAKE256   136

#define AXS3_DIGEST_256      32
#define AXS3_DIGEST_384      48
#define AXS3_DIGEST_512      64

typedef struct axs3_context
{
    u64  lanes[AXS3_LANE_COUNT];
    u64  absorbed;
    u32  rate;
    u32  offset;
    u32  squeezes;
    u32  blocks;
    u32  digest_size;
    bool_t finalized;
} axs3_context_t;

static const u64 axs3_round_constants[AXS3_ROUNDS] = {
    0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808Aull,
    0x8000000080008000ull, 0x000000000000808Bull, 0x0000000080000001ull,
    0x8000000080008081ull, 0x8000000000008009ull, 0x000000000000008Aull,
    0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000Aull,
    0x000000008000808Bull, 0x800000000000008Bull, 0x8000000000008089ull,
    0x8000000000008003ull, 0x8000000000008002ull, 0x8000000000000080ull,
    0x000000000000800Aull, 0x800000008000000Aull, 0x8000000080008081ull,
    0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull
};

static const u32 axs3_rotation[AXS3_LANE_COUNT] = {
     0,  1, 62, 28, 27,
    36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,
    41, 45, 15, 21,  8,
    18,  2, 61, 56, 14
};

static void axs3_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u64 axs3_rotl(u64 value, u32 count)
{
    count &= 63;
    if (count == 0)
        return value;
    return (value << count) | (value >> (64 - count));
}

static void axs3_permute(u64 *lanes)
{
    u64 c[5];
    u64 b[AXS3_LANE_COUNT];
    for (u32 round = 0; round < AXS3_ROUNDS; round++)
    {
        /* theta */
        for (u32 x = 0; x < 5; x++)
            c[x] = lanes[x] ^ lanes[x + 5] ^ lanes[x + 10] ^ lanes[x + 15] ^ lanes[x + 20];
        for (u32 x = 0; x < 5; x++)
        {
            u64 d = c[(x + 4) % 5] ^ axs3_rotl(c[(x + 1) % 5], 1);
            for (u32 y = 0; y < 25; y += 5)
                lanes[y + x] ^= d;
        }
        /* rho and pi */
        for (u32 x = 0; x < 5; x++)
        {
            for (u32 y = 0; y < 5; y++)
            {
                u32 index = x + 5 * y;
                u32 target = y + 5 * ((2 * x + 3 * y) % 5);
                b[target] = axs3_rotl(lanes[index], axs3_rotation[index]);
            }
        }
        /* chi */
        for (u32 y = 0; y < 25; y += 5)
        {
            for (u32 x = 0; x < 5; x++)
                lanes[y + x] = b[y + x] ^ ((~b[y + (x + 1) % 5]) & b[y + (x + 2) % 5]);
        }
        /* iota */
        lanes[0] ^= axs3_round_constants[round];
    }
}

static void axs3_absorb_block(axs3_context_t *context, const u8 *block)
{
    u32 i;
    for (i = 0; i < context->rate / 8; i++)
    {
        u64 value = 0;
        for (u32 byte = 0; byte < 8; byte++)
            value |= (u64)block[i * 8 + byte] << (byte * 8);
        context->lanes[i] ^= value;
    }
    axs3_permute(context->lanes);
    context->blocks++;
}

static void axs3_pad(axs3_context_t *context, u8 domain)
{
    u32 i;
    u8 block[AXS3_RATE_SHAKE128];
    axs3_zero(block, AXS3_RATE_SHAKE128);
    for (i = 0; i < context->offset; i++)
        block[i] = ((u8 *)context->lanes)[i];
    block[context->offset] ^= domain;
    block[context->rate - 1] ^= 0x80;
    axs3_absorb_block(context, block);
    context->offset = 0;
}

static void axs3_emit(axs3_context_t *context, u8 *out, u32 length)
{
    u32 i;
    for (i = 0; i < length; i++)
    {
        if (context->offset == context->rate)
        {
            axs3_permute(context->lanes);
            context->offset = 0;
            context->squeezes++;
        }
        out[i] = ((u8 *)context->lanes)[context->offset];
        context->offset++;
    }
}

/* -------------------------------------------------------------- public API */

bool_t axs3_init(axs3_context_t *context, u32 rate, u32 digest_size)
{
    if (context == NULL_PTR)
        return false;
    if (rate != AXS3_RATE_256 && rate != AXS3_RATE_384 && rate != AXS3_RATE_512 &&
        rate != AXS3_RATE_SHAKE128 && rate != AXS3_RATE_SHAKE256)
        return false;
    axs3_zero(context, (u32)sizeof(*context));
    context->rate = rate;
    context->digest_size = digest_size;
    return true;
}

bool_t axs3_update(axs3_context_t *context, const u8 *data, u32 length)
{
    u32 consumed = 0;
    if (context == NULL_PTR || data == NULL_PTR || context->finalized)
        return false;
    while (consumed < length)
    {
        if (context->offset == context->rate)
        {
            axs3_absorb_block(context, (const u8 *)context->lanes);
            context->offset = 0;
        }
        u32 room = context->rate - context->offset;
        u32 chunk = length - consumed < room ? length - consumed : room;
        for (u32 i = 0; i < chunk; i++)
            ((u8 *)context->lanes)[context->offset + i] ^= data[consumed + i];
        context->offset += chunk;
        consumed += chunk;
        context->absorbed += chunk;
    }
    return true;
}

bool_t axs3_final(axs3_context_t *context, u8 *digest, u32 capacity)
{
    u32 length;
    if (context == NULL_PTR || digest == NULL_PTR || context->finalized)
        return false;
    length = context->digest_size;
    if (length == 0)
        length = 16;
    if (length > capacity)
        return false;
    axs3_pad(context, 0x06);
    for (u32 i = 0; i < length; i++)
        digest[i] = 0;
    axs3_emit(context, digest, length);
    context->finalized = true;
    return true;
}

bool_t axs3_final_shake(axs3_context_t *context, u8 *out, u32 length)
{
    if (context == NULL_PTR || out == NULL_PTR || context->finalized)
        return false;
    if (context->rate != AXS3_RATE_SHAKE128 && context->rate != AXS3_RATE_SHAKE256)
        return false;
    axs3_pad(context, 0x1F);
    axs3_emit(context, out, length);
    context->finalized = true;
    return true;
}

u32 axs3_digest(u32 bits, const u8 *data, u32 length, u8 *digest, u32 capacity)
{
    axs3_context_t context;
    u32 rate;
    u32 size;
    switch (bits)
    {
    case 256: rate = AXS3_RATE_256; size = AXS3_DIGEST_256; break;
    case 384: rate = AXS3_RATE_384; size = AXS3_DIGEST_384; break;
    case 512: rate = AXS3_RATE_512; size = AXS3_DIGEST_512; break;
    default: return 0;
    }
    if (!axs3_init(&context, rate, size))
        return 0;
    if (length != 0 && !axs3_update(&context, data, length))
        return 0;
    if (!axs3_final(&context, digest, capacity))
        return 0;
    return size;
}

u32 axs3_hmac(const u8 *key, u32 key_length, const u8 *message, u32 length,
              u8 *output, u32 capacity)
{
    axs3_context_t inner;
    axs3_context_t outer;
    u8 pad_inner[AXS3_RATE_SHAKE128];
    u8 pad_outer[AXS3_RATE_SHAKE128];
    u8 inner_digest[AXS3_DIGEST_512];
    u32 i;
    if (key == NULL_PTR || message == NULL_PTR || output == NULL_PTR || capacity < AXS3_DIGEST_512)
        return 0;
    axs3_zero(pad_inner, AXS3_RATE_SHAKE128);
    axs3_zero(pad_outer, AXS3_RATE_SHAKE128);
    for (i = 0; i < key_length && i < AXS3_RATE_SHAKE128; i++)
    {
        pad_inner[i] = key[i] ^ 0x36;
        pad_outer[i] = key[i] ^ 0x5C;
    }
    for (; i < AXS3_RATE_SHAKE128; i++)
    {
        pad_inner[i] = 0x36;
        pad_outer[i] = 0x5C;
    }
    if (!axs3_init(&inner, AXS3_RATE_SHAKE128, AXS3_DIGEST_512))
        return 0;
    axs3_update(&inner, pad_inner, AXS3_RATE_SHAKE128);
    axs3_update(&inner, message, length);
    axs3_final(&inner, inner_digest, AXS3_DIGEST_512);
    if (!axs3_init(&outer, AXS3_RATE_SHAKE128, AXS3_DIGEST_512))
        return 0;
    axs3_update(&outer, pad_outer, AXS3_RATE_SHAKE128);
    axs3_update(&outer, inner_digest, AXS3_DIGEST_512);
    axs3_final(&outer, output, capacity);
    return AXS3_DIGEST_512;
}

u32 axs3_shake128(const u8 *data, u32 length, u8 *out, u32 out_length)
{
    axs3_context_t context;
    if (!axs3_init(&context, AXS3_RATE_SHAKE128, 0))
        return 0;
    if (length != 0 && !axs3_update(&context, data, length))
        return 0;
    if (!axs3_final_shake(&context, out, out_length))
        return 0;
    return out_length;
}

u32 axs3_shake256(const u8 *data, u32 length, u8 *out, u32 out_length)
{
    axs3_context_t context;
    if (!axs3_init(&context, AXS3_RATE_SHAKE256, 0))
        return 0;
    if (length != 0 && !axs3_update(&context, data, length))
        return 0;
    if (!axs3_final_shake(&context, out, out_length))
        return 0;
    return out_length;
}

u32 axs3_hex(const u8 *digest, u32 length, char *text, u32 capacity)
{
    static const char digits[] = "0123456789abcdef";
    if (digest == NULL_PTR || text == NULL_PTR)
        return 0;
    if (capacity < length * 2 + 1)
        return 0;
    for (u32 i = 0; i < length; i++)
    {
        text[i * 2] = digits[digest[i] >> 4];
        text[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    text[length * 2] = 0;
    return length * 2;
}

void axs3_report(void)
{
    u8 digest[AXS3_DIGEST_512];
    char text[80];
    u32 size = axs3_digest(256, (const u8 *)"", 0, digest, AXS3_DIGEST_512);
    axs3_hex(digest, size, text, 80);
    printk("axs3: sha3-256(empty)=%s\n", text);
    size = axs3_digest(512, (const u8 *)"axiomos", 7, digest, AXS3_DIGEST_512);
    axs3_hex(digest, size > 24 ? 24 : size, text, 80);
    printk("axs3: sha3-512 prefix=%s\n", text);
}

bool_t axs3_selftest(void)
{
    u8 digest[AXS3_DIGEST_512];
    u8 short_out[16];
    u8 hmac[AXS3_DIGEST_512];
    u32 size;
    if (!axs3_init(NULL_PTR, AXS3_RATE_256, 32))
        return false;
    size = axs3_digest(256, (const u8 *)"abc", 3, digest, AXS3_DIGEST_512);
    if (size != 32)
        return false;
    size = axs3_digest(384, (const u8 *)"abc", 3, digest, AXS3_DIGEST_512);
    if (size != 48)
        return false;
    size = axs3_digest(512, (const u8 *)"abc", 3, digest, AXS3_DIGEST_512);
    if (size != 64)
        return false;
    if (axs3_digest(128, (const u8 *)"abc", 3, digest, AXS3_DIGEST_512) != 0)
        return false;
    if (axs3_shake128((const u8 *)"abc", 3, short_out, 16) != 16)
        return false;
    if (axs3_shake256((const u8 *)"abc", 3, short_out, 16) != 16)
        return false;
    if (axs3_hmac((const u8 *)"key", 3, (const u8 *)"msg", 3, hmac, AXS3_DIGEST_512) != 64)
        return false;
    if (axs3_rotl(1, 0) != 1)
        return false;
    if (axs3_rotl(1, 63) != 0x8000000000000000ull)
        return false;
    if (axs3_rotl(0x8000000000000000ull, 1) != 1)
        return false;
    return true;
}
