/* AxiomOS - SHA-256 and simple checksums.
 *
 * SHA-256 is implemented here for the code-integrity manifest.  A second
 * implementation lives in the Rust crate `axi_crypto`; the Rust one is used
 * for the hypervisor-assisted verification path so that a compiler bug in
 * one language cannot silently invalidate both.  Having the C version as
 * reference also means the HVCI subsystem works before Rust code is linked.
 */

#include <axiom/kernel.h>

static const u32 k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

AXIOM_INLINE u32 rotr(u32 x, u32 n)
{
    return (x >> n) | (x << (32 - n));
}

AXIOM_INLINE u32 ch(u32 x, u32 y, u32 z)  { return (x & y) ^ (~x & z); }
AXIOM_INLINE u32 maj(u32 x, u32 y, u32 z) { return (x & y) ^ (x & z) ^ (y & z); }
AXIOM_INLINE u32 bsig0(u32 x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
AXIOM_INLINE u32 bsig1(u32 x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
AXIOM_INLINE u32 ssig0(u32 x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
AXIOM_INLINE u32 ssig1(u32 x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

struct sha256_ctx {
    u32 state[8];
    u64 bit_count;
    u8  buffer[64];
    u32 buffer_len;
};

static void sha256_compress(struct sha256_ctx *ctx, const u8 *block)
{
    u32 w[64];
    u32 a, b, c, d, e, f, g, h;

    for (u32 i = 0; i < 16; i++) {
        w[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16) |
               ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
    }
    for (u32 i = 16; i < 64; i++)
        w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (u32 i = 0; i < 64; i++) {
        u32 t1 = h + bsig1(e) + ch(e, f, g) + k256[i] + w[i];
        u32 t2 = bsig0(a) + maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(struct sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bit_count = 0;
    ctx->buffer_len = 0;
}

static void sha256_update(struct sha256_ctx *ctx, const u8 *data, u64 len)
{
    for (u64 i = 0; i < len; i++) {
        ctx->buffer[ctx->buffer_len++] = data[i];
        if (ctx->buffer_len == 64) {
            sha256_compress(ctx, ctx->buffer);
            ctx->bit_count += 512;
            ctx->buffer_len = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *ctx, u8 out[32])
{
    u32 i = ctx->buffer_len;

    ctx->bit_count += (u64)ctx->buffer_len * 8;

    /* Padding: 0x80, then zeros until 56 mod 64, then the 64-bit length. */
    ctx->buffer[i++] = 0x80;
    if (i > 56) {
        while (i < 64)
            ctx->buffer[i++] = 0;
        sha256_compress(ctx, ctx->buffer);
        i = 0;
    }
    while (i < 56)
        ctx->buffer[i++] = 0;

    ctx->buffer[56] = (u8)(ctx->bit_count >> 56);
    ctx->buffer[57] = (u8)(ctx->bit_count >> 48);
    ctx->buffer[58] = (u8)(ctx->bit_count >> 40);
    ctx->buffer[59] = (u8)(ctx->bit_count >> 32);
    ctx->buffer[60] = (u8)(ctx->bit_count >> 24);
    ctx->buffer[61] = (u8)(ctx->bit_count >> 16);
    ctx->buffer[62] = (u8)(ctx->bit_count >> 8);
    ctx->buffer[63] = (u8)(ctx->bit_count);
    sha256_compress(ctx, ctx->buffer);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (u8)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (u8)(ctx->state[i]);
    }
}

u64 sha256_hash(const void *data, u64 size, u8 out[32])
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const u8 *)data, size);
    sha256_final(&ctx, out);
    return size;
}

u64 sha256_hash_bytes(const u8 *data, u64 size, u8 out[32])
{
    return sha256_hash(data, size, out);
}

/* Incremental interface used when hashing large regions page by page. */
void *sha256_begin(void)
{
    struct sha256_ctx *ctx = (struct sha256_ctx *)kmalloc(sizeof(struct sha256_ctx));
    if (!ctx)
        return NULL_PTR;
    sha256_init(ctx);
    return ctx;
}

void sha256_absorb(void *handle, const void *data, u64 size)
{
    if (!handle)
        return;
    sha256_update((struct sha256_ctx *)handle, (const u8 *)data, size);
}

void sha256_finish(void *handle, u8 out[32])
{
    if (!handle)
        return;
    sha256_final((struct sha256_ctx *)handle, out);
    kfree(handle);
}

/* ------------------------------------------------------------------ */
/* CRC32 and FNV-1a                                                   */
/* ------------------------------------------------------------------ */

u32 crc32(const void *data, u64 size)
{
    static const u32 table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa0ecab78, 0xbdbdf21c
    };
    u32 crc = 0xFFFFFFFF;
    const u8 *p = (const u8 *)data;
    for (u64 i = 0; i < size; i++) {
        u8 byte = p[i];
        crc ^= byte;
        crc = (crc >> 4) ^ table[crc & 0x0F];
        crc = (crc >> 4) ^ table[crc & 0x0F];
    }
    return ~crc;
}

u64 fnv1a64(const void *data, u64 size)
{
    u64 hash = 0xCBF29CE484222325ULL;
    const u8 *p = (const u8 *)data;
    for (u64 i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}
