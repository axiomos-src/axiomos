/* AxiomOS - SHA-1, SHA-512 and MD5 digests plus HMAC wrappers.
 *
 * SHA-1 and MD5 are not used for security decisions in AxiomOS: SHA-1 only
 * appears in the legacy firmware hand-off blob format and in the boot log
 * fingerprint, MD5 only for ISO 9660 / disk-image identification compatibility.
 * SHA-512 backs the 512-bit boot measurement chain and the sealed dump MAC.
 * Everything is incremental so that a multi-megabyte kernel image can be
 * hashed in streamed blocks without a temporary buffer.
 */

#include <axiom/crypto.h>
#include <axiom/lib.h>

/* ================================================================== */
/* SHA-1                                                              */
/* ================================================================== */

#define ROL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_compress(struct sha1_ctx *ctx, const u8 *block)
{
    u32 w[80];

    for (u32 i = 0; i < 16; i++) {
        w[i] = ((u32)block[4 * i] << 24) | ((u32)block[4 * i + 1] << 16) |
               ((u32)block[4 * i + 2] << 8) | ((u32)block[4 * i + 3]);
    }
    for (u32 i = 16; i < 80; i++) {
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    u32 a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];

    for (u32 i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        u32 temp = ROL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL32(b, 30);
        b = a;
        a = temp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

void sha1_init(struct sha1_ctx *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->total = 0;
    ctx->used = 0;
}

void sha1_update(struct sha1_ctx *ctx, const void *data, usize len)
{
    const u8 *p = (const u8 *)data;

    ctx->total += len;
    while (len) {
        if (ctx->used == 0 && len >= 64) {
            sha1_compress(ctx, p);
            p += 64;
            len -= 64;
            continue;
        }
        u32 chunk = 64 - ctx->used;
        if (chunk > len) {
            chunk = (u32)len;
        }
        ax_memcpy(ctx->buffer + ctx->used, p, chunk);
        ctx->used += chunk;
        p += chunk;
        len -= chunk;
        if (ctx->used == 64) {
            sha1_compress(ctx, ctx->buffer);
            ctx->used = 0;
        }
    }
}

void sha1_final(struct sha1_ctx *ctx, u8 out[20])
{
    u8 pad[64];
    u64 bits = ctx->total * 8;

    ax_memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    u32 pad_len = (ctx->used < 56) ? (56 - ctx->used) : (120 - ctx->used);

    sha1_update(ctx, pad, pad_len);

    u8 length_be[8];
    for (u32 i = 0; i < 8; i++) {
        length_be[7 - i] = (u8)(bits >> (8 * i));
    }
    sha1_update(ctx, length_be, 8);

    for (u32 i = 0; i < 5; i++) {
        out[4 * i + 0] = (u8)(ctx->h[i] >> 24);
        out[4 * i + 1] = (u8)(ctx->h[i] >> 16);
        out[4 * i + 2] = (u8)(ctx->h[i] >> 8);
        out[4 * i + 3] = (u8)(ctx->h[i]);
    }
}

void sha1_hash(const void *data, usize len, u8 out[20])
{
    struct sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

/* HMAC-SHA1 per RFC 2104. */
void sha1_hmac(const u8 *key, usize key_len, const u8 *msg, usize msg_len, u8 out[20])
{
    u8 k_ipad[64];
    u8 k_opad[64];
    u8 inner[20];
    struct sha1_ctx ctx;

    ax_memset(k_ipad, 0, 64);
    if (key_len > 64) {
        sha1_hash(key, key_len, k_ipad);
        key_len = 20;
    } else {
        ax_memcpy(k_ipad, key, key_len);
    }
    ax_memcpy(k_opad, k_ipad, 64);
    for (u32 i = 0; i < 64; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5C;
    }

    sha1_init(&ctx);
    sha1_update(&ctx, k_ipad, 64);
    sha1_update(&ctx, msg, msg_len);
    sha1_final(&ctx, inner);

    sha1_init(&ctx);
    sha1_update(&ctx, k_opad, 64);
    sha1_update(&ctx, inner, 20);
    sha1_final(&ctx, out);
}

/* ================================================================== */
/* SHA-512                                                            */
/* ================================================================== */

static const u64 sha512_k[80] = {
    0x428A2F98D728AE22ULL, 0x7137449123EF65CDULL, 0xB5C0FBCFEC4D3B2FULL,
    0xE9B5DBA58189DBBCULL, 0x3956C25BF348B538ULL, 0x59F111F1B605D019ULL,
    0x923F82A4AF194F9BULL, 0xAB1C5ED5DA6D8118ULL, 0xD807AA98A3030242ULL,
    0x12835B0145706FBEULL, 0x243185BE4EE4B28CULL, 0x550C7DC3D5FFB4E2ULL,
    0x72BE5D74F27B896FULL, 0x80DEB1FE3B1696B1ULL, 0x9BDC06A725C71235ULL,
    0xC19BF174CF692694ULL, 0xE49B69C19EF14AD2ULL, 0xEFBE4786384F25E3ULL,
    0x0FC19DC68B8CD5B5ULL, 0x240CA1CC77AC9C65ULL, 0x2DE92C6F592B0275ULL,
    0x4A7484AA6EA6E483ULL, 0x5CB0A9DCBD41FBD4ULL, 0x76F988DA831153B5ULL,
    0x983E5152EE66DFABULL, 0xA831C66D2DB43210ULL, 0xB00327C898FB213FULL,
    0xBF597FC7BEEF0EE4ULL, 0xC6E00BF33DA88FC2ULL, 0xD5A79147930AA725ULL,
    0x06CA6351E003826FULL, 0x142929670A0E6E70ULL, 0x27B70A8546D22FFCULL,
    0x2E1B21385C26C926ULL, 0x4D2C6DFC5AC42AEDULL, 0x53380D139D95B3DFULL,
    0x650A73548BAF63DEULL, 0x766A0ABB3C77B2A8ULL, 0x81C2C92E47EDAEE6ULL,
    0x92722C851482353BULL, 0xA2BFE8A14CF10364ULL, 0xA81A664BBC423001ULL,
    0xC24B8B70D0F89791ULL, 0xC76C51A30654BE30ULL, 0xD192E819D6EF5218ULL,
    0xD69906245565A910ULL, 0xF40E35855771202AULL, 0x106AA07032BBD1B8ULL,
    0x19A4C116B8D2D0C8ULL, 0x1E376C085141AB53ULL, 0x2748774CDF8EEB99ULL,
    0x34B0BCB5E19B48A8ULL, 0x391C0CB3C5C95A63ULL, 0x4ED8AA4AE3418ACBULL,
    0x5B9CCA4F7763E373ULL, 0x682E6FF3D6B2B8A3ULL, 0x748F82EE5DEFB2FCULL,
    0x78A5636F43172F60ULL, 0x84C87814A1F0AB72ULL, 0x8CC702081A6439ECULL,
    0x90BEFFFA23631E28ULL, 0xA4506CEBDE82BDE9ULL, 0xBEF9A3F7B2C67915ULL,
    0xC67178F2E372532BULL, 0xCA273ECEEA26619CULL, 0xD186B8C721C0C207ULL,
    0xEADA7DD6CDE0EB1EULL, 0xF57D4F7FEE6ED178ULL, 0x06F067AA72176FBAULL,
    0x0A637DC5A2C898A6ULL, 0x113F9804BEF90DAEULL, 0x1B710B35131C471BULL,
    0x28DB77F523047D84ULL, 0x32CAAB7B40C72493ULL, 0x3C9EBE0A15C9BEBCULL,
    0x431D67C49C100D4CULL, 0x4CC5D4BECB3E42B6ULL, 0x597F299CFC657E2AULL,
    0x5FCB6FAB3AD6FAECULL, 0x6C44198C4A475817ULL,
};

#define ROR64(v, n) (((v) >> (n)) | ((v) << (64 - (n))))

static void sha512_compress(struct sha512_ctx *ctx, const u8 *block)
{
    u64 w[80];

    for (u32 i = 0; i < 16; i++) {
        w[i] = 0;
        for (u32 b = 0; b < 8; b++) {
            w[i] = (w[i] << 8) | (u64)block[8 * i + b];
        }
    }
    for (u32 i = 16; i < 80; i++) {
        u64 s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u64 a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    u64 e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (u32 i = 0; i < 80; i++) {
        u64 s1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
        u64 ch = (e & f) ^ ((~e) & g);
        u64 t1 = h + s1 + ch + sha512_k[i] + w[i];
        u64 s0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
        u64 maj = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

void sha512_init(struct sha512_ctx *ctx)
{
    ctx->h[0] = 0x6A09E667F3BCC908ULL;
    ctx->h[1] = 0xBB67AE8584CAA73BULL;
    ctx->h[2] = 0x3C6EF372FE94F82BULL;
    ctx->h[3] = 0xA54FF53A5F1D36F1ULL;
    ctx->h[4] = 0x510E527FADE682D1ULL;
    ctx->h[5] = 0x9B05688C2B3E6C1FULL;
    ctx->h[6] = 0x1F83D9ABFB41BD6BULL;
    ctx->h[7] = 0x5BE0CD19137E2179ULL;
    ctx->total_lo = 0;
    ctx->total_hi = 0;
    ctx->used = 0;
}

void sha512_update(struct sha512_ctx *ctx, const void *data, usize len)
{
    const u8 *p = (const u8 *)data;

    ctx->total_lo += (u64)len;
    if (ctx->total_lo < (u64)len) {
        ctx->total_hi++;
    }

    while (len) {
        if (ctx->used == 0 && len >= 128) {
            sha512_compress(ctx, p);
            p += 128;
            len -= 128;
            continue;
        }
        u32 chunk = 128 - ctx->used;
        if (chunk > len) {
            chunk = (u32)len;
        }
        ax_memcpy(ctx->buffer + ctx->used, p, chunk);
        ctx->used += chunk;
        p += chunk;
        len -= chunk;
        if (ctx->used == 128) {
            sha512_compress(ctx, ctx->buffer);
            ctx->used = 0;
        }
    }
}

void sha512_final(struct sha512_ctx *ctx, u8 out[64])
{
    u8 pad[128];
    u64 bits_lo = ctx->total_lo * 8;
    u64 bits_hi = (ctx->total_hi << 3) | (ctx->total_lo >> 61);

    ax_memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    u32 pad_len = (ctx->used < 112) ? (112 - ctx->used) : (240 - ctx->used);

    sha512_update(ctx, pad, pad_len);

    u8 length_be[16];
    for (u32 i = 0; i < 8; i++) {
        length_be[7 - i] = (u8)(bits_hi >> (8 * i));
        length_be[15 - i] = (u8)(bits_lo >> (8 * i));
    }
    sha512_update(ctx, length_be, 16);

    for (u32 i = 0; i < 8; i++) {
        for (u32 b = 0; b < 8; b++) {
            out[8 * i + b] = (u8)(ctx->h[i] >> (56 - 8 * b));
        }
    }
}

void sha512_hash(const void *data, usize len, u8 out[64])
{
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}

void sha512_hmac(const u8 *key, usize key_len, const u8 *msg, usize msg_len, u8 out[64])
{
    u8 k_ipad[128];
    u8 k_opad[128];
    u8 inner[64];
    struct sha512_ctx ctx;

    ax_memset(k_ipad, 0, 128);
    if (key_len > 128) {
        sha512_hash(key, key_len, k_ipad);
        key_len = 64;
    } else {
        ax_memcpy(k_ipad, key, key_len);
    }
    ax_memcpy(k_opad, k_ipad, 128);
    for (u32 i = 0; i < 128; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5C;
    }

    sha512_init(&ctx);
    sha512_update(&ctx, k_ipad, 128);
    sha512_update(&ctx, msg, msg_len);
    sha512_final(&ctx, inner);

    sha512_init(&ctx);
    sha512_update(&ctx, k_opad, 128);
    sha512_update(&ctx, inner, 64);
    sha512_final(&ctx, out);
}

/* ================================================================== */
/* MD5                                                                */
/* ================================================================== */

static const u32 md5_k[64] = {
    0xD76AA478u, 0xE8C7B756u, 0x242070DBu, 0xC1BDCEEEu,
    0xF57C0FAFu, 0x4787C62Au, 0xA8304613u, 0xFD469501u,
    0x698098D8u, 0x8B44F7AFu, 0xFFFF5BB1u, 0x895CD7BEu,
    0x6B901122u, 0xFD987193u, 0xA679438Eu, 0x49B40821u,
    0xF61E2562u, 0xC040B340u, 0x265E5A51u, 0xE9B6C7AAu,
    0xD62F105Du, 0x02441453u, 0xD8A1E681u, 0xE7D3FBC8u,
    0x21E1CDE6u, 0xC33707D6u, 0xF4D50D87u, 0x455A14EDu,
    0xA9E3E905u, 0xFCEFA3F8u, 0x676F02D9u, 0x8D2A4C8Au,
    0xFFFA3942u, 0x8771F681u, 0x6D9D6122u, 0xFDE5380Cu,
    0xA4BEEA44u, 0x4BDECFA9u, 0xF6BB4B60u, 0xBEBFBC70u,
    0x289B7EC6u, 0xEAA127FAu, 0xD4EF3085u, 0x04881D05u,
    0xD9D4D039u, 0xE6DB99E5u, 0x1FA27CF8u, 0xC4AC5665u,
    0xF4292244u, 0x432AFF97u, 0xAB9423A7u, 0xFC93A039u,
    0x655B59C3u, 0x8F0CCC92u, 0xFFEFF47Du, 0x85845DD1u,
    0x6FA87E4Fu, 0xFE2CE6E0u, 0xA3014314u, 0x4E0811A1u,
    0xF7537E82u, 0xBD3AF235u, 0x2AD7D2BBu, 0xEB86D391u,
};

static const u8 md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_compress(struct md5_ctx *ctx, const u8 *block)
{
    u32 m[16];

    for (u32 i = 0; i < 16; i++) {
        m[i] = (u32)block[4 * i] | ((u32)block[4 * i + 1] << 8) |
               ((u32)block[4 * i + 2] << 16) | ((u32)block[4 * i + 3] << 24);
    }

    u32 a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];

    for (u32 i = 0; i < 64; i++) {
        u32 f, g;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | (~d));
            g = (7 * i) % 16;
        }
        u32 temp = d;
        d = c;
        c = b;
        b = b + ROL32((a + f + md5_k[i] + m[g]), md5_s[i]);
        a = temp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
}

void md5_init(struct md5_ctx *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->total = 0;
    ctx->used = 0;
}

void md5_update(struct md5_ctx *ctx, const void *data, usize len)
{
    const u8 *p = (const u8 *)data;

    ctx->total += len;
    while (len) {
        if (ctx->used == 0 && len >= 64) {
            md5_compress(ctx, p);
            p += 64;
            len -= 64;
            continue;
        }
        u32 chunk = 64 - ctx->used;
        if (chunk > len) {
            chunk = (u32)len;
        }
        ax_memcpy(ctx->buffer + ctx->used, p, chunk);
        ctx->used += chunk;
        p += chunk;
        len -= chunk;
        if (ctx->used == 64) {
            md5_compress(ctx, ctx->buffer);
            ctx->used = 0;
        }
    }
}

void md5_final(struct md5_ctx *ctx, u8 out[16])
{
    u8 pad[64];
    u64 bits = ctx->total * 8;

    ax_memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    u32 pad_len = (ctx->used < 56) ? (56 - ctx->used) : (120 - ctx->used);
    md5_update(ctx, pad, pad_len);

    u8 length_le[8];
    for (u32 i = 0; i < 8; i++) {
        length_le[i] = (u8)(bits >> (8 * i));
    }
    md5_update(ctx, length_le, 8);

    for (u32 i = 0; i < 4; i++) {
        out[4 * i + 0] = (u8)(ctx->h[i]);
        out[4 * i + 1] = (u8)(ctx->h[i] >> 8);
        out[4 * i + 2] = (u8)(ctx->h[i] >> 16);
        out[4 * i + 3] = (u8)(ctx->h[i] >> 24);
    }
}

void md5_hash(const void *data, usize len, u8 out[16])
{
    struct md5_ctx ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx, out);
}

/* ================================================================== */
/* Helpers                                                            */
/* ================================================================== */

/* Compare digests without an early exit so that verification does not leak the
 * position of the first mismatching byte. */
bool_t crypto_equal(const u8 *a, const u8 *b, usize len)
{
    u8 diff = 0;
    for (usize i = 0; i < len; i++) {
        diff |= (u8)(a[i] ^ b[i]);
    }
    return diff == 0;
}

void crypto_hex_encode(const u8 *data, usize len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (usize i = 0; i < len; i++) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0xF];
    }
    out[2 * len] = 0;
}

int crypto_hex_decode(const char *text, u8 *out, usize cap)
{
    usize n = 0;
    for (; text[0] && text[1]; text += 2) {
        if (n >= cap) {
            return -1;
        }
        int hi = ax_digit_value(text[0]);
        int lo = ax_digit_value(text[1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[n++] = (u8)((hi << 4) | lo);
    }
    return (int)n;
}
