/* AxiomOS - cryptography interface.
 *
 * Everything here is implemented in-kernel without allocation: the caller owns
 * the context and the buffers.  The tree ships SHA-256 already; this header
 * adds AES, the older digests that firmware tables and image formats still
 * reference (SHA-1, MD5), the 512-bit digests, and the checksum/HMAC helpers
 * that the boot measurements and the HVCI policy parser use.
 */
#ifndef AXIOM_CRYPTO_H
#define AXIOM_CRYPTO_H

#include <axiom/types.h>

/* ------------------------------------------------------------------ */
/* AES                                                                 */
/* ------------------------------------------------------------------ */

#define AES_MAX_ROUND_KEYS 60      /* (14 + 1) * 4 words for AES-256 */

struct aes_ctx {
    u32  rk[AES_MAX_ROUND_KEYS];
    u32  rounds;
    u32  key_words;
};

int  aes_init(struct aes_ctx *ctx, const u8 *key, u32 key_bits);
void aes_encrypt_block(const struct aes_ctx *ctx, const u8 in[16], u8 out[16]);
void aes_decrypt_block(const struct aes_ctx *ctx, const u8 in[16], u8 out[16]);
void aes_ecb_encrypt(const struct aes_ctx *ctx, const u8 *in, u8 *out, usize blocks);
void aes_ecb_decrypt(const struct aes_ctx *ctx, const u8 *in, u8 *out, usize blocks);
void aes_cbc_encrypt(const struct aes_ctx *ctx, u8 iv[16], const u8 *in,
                     u8 *out, usize blocks);
void aes_cbc_decrypt(const struct aes_ctx *ctx, u8 iv[16], const u8 *in,
                     u8 *out, usize blocks);
void aes_ctr_crypt(const struct aes_ctx *ctx, u8 counter[16], const u8 *in,
                   u8 *out, usize bytes);
usize aes_pkcs7_pad(u8 *buffer, usize len, usize block);
int   aes_pkcs7_unpad(const u8 *buffer, usize len, usize block, usize *out_len);

/* ------------------------------------------------------------------ */
/* SHA-256 (implemented in crypto/sha256.c)                            */
/* ------------------------------------------------------------------ */

u64 sha256_hash(const void *data, u64 size, u8 out[32]);
u64 sha256_hash_bytes(const u8 *data, u64 size, u8 out[32]);

/* ------------------------------------------------------------------ */
/* SHA-1, SHA-512, MD5                                                 */
/* ------------------------------------------------------------------ */

struct sha1_ctx {
    u32 h[5];
    u64 total;
    u8  buffer[64];
    u32 used;
};

void sha1_init(struct sha1_ctx *ctx);
void sha1_update(struct sha1_ctx *ctx, const void *data, usize len);
void sha1_final(struct sha1_ctx *ctx, u8 out[20]);
void sha1_hash(const void *data, usize len, u8 out[20]);
void sha1_hmac(const u8 *key, usize key_len, const u8 *msg, usize msg_len, u8 out[20]);

struct sha512_ctx {
    u64 h[8];
    u64 total_lo;
    u64 total_hi;
    u8  buffer[128];
    u32 used;
};

void sha512_init(struct sha512_ctx *ctx);
void sha512_update(struct sha512_ctx *ctx, const void *data, usize len);
void sha512_final(struct sha512_ctx *ctx, u8 out[64]);
void sha512_hash(const void *data, usize len, u8 out[64]);
void sha512_hmac(const u8 *key, usize key_len, const u8 *msg, usize msg_len, u8 out[64]);

struct md5_ctx {
    u32 h[4];
    u64 total;
    u8  buffer[64];
    u32 used;
};

void md5_init(struct md5_ctx *ctx);
void md5_update(struct md5_ctx *ctx, const void *data, usize len);
void md5_final(struct md5_ctx *ctx, u8 out[16]);
void md5_hash(const void *data, usize len, u8 out[16]);

/* Constant-ish time comparison for digest and MAC verification. */
bool_t crypto_equal(const u8 *a, const u8 *b, usize len);

/* Hex helpers used by the policy files and the crash dump header. */
void crypto_hex_encode(const u8 *data, usize len, char *out);
int  crypto_hex_decode(const char *text, u8 *out, usize cap);

#endif /* AXIOM_CRYPTO_H */
