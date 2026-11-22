/* AxiomOS - AES (FIPS-197) with ECB, CBC and CTR modes.
 *
 * The kernel uses AES for three things: sealing the crash dump so that a panic
 * on a shared machine does not leak heap contents, protecting the TPM-less
 * "disk key" of the encrypted overlay filesystem, and driving the CTR mode
 * counter of the network stack's optional payload encryption.  There is no
 * timing hardening beyond table lookups, which is acceptable because phase 1
 * does not expose AES to a remote attacker.
 */

#include <axiom/crypto.h>
#include <axiom/lib.h>

static const u8 aes_sbox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5,
    0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0,
    0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC,
    0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A,
    0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0,
    0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B,
    0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85,
    0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5,
    0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17,
    0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88,
    0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C,
    0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9,
    0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6,
    0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E,
    0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94,
    0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68,
    0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

/* Inverse S-box, rebuilt once lazily instead of held in rodata. */
static u8 aes_rsbox[256];
static bool_t aes_rsbox_ready;

static void aes_build_rsbox(void)
{
    for (u32 i = 0; i < 256; i++) {
        aes_rsbox[aes_sbox[i]] = (u8)i;
    }
    aes_rsbox_ready = true;
}

static u8 aes_xtime(u8 v)
{
    return (u8)((v << 1) ^ ((v & 0x80) ? 0x1B : 0x00));
}

static u8 aes_gmul(u8 a, u8 b)
{
    u8 p = 0;
    for (u8 i = 0; i < 8; i++) {
        if (b & 1) {
            p ^= a;
        }
        bool_t high = (a & 0x80) ? true : false;
        a = (u8)(a << 1);
        if (high) {
            a ^= 0x1B;
        }
        b >>= 1;
    }
    return p;
}

static u32 aes_sub_word(u32 w)
{
    return ((u32)aes_sbox[(w >> 24) & 0xFF] << 24) |
           ((u32)aes_sbox[(w >> 16) & 0xFF] << 16) |
           ((u32)aes_sbox[(w >> 8) & 0xFF] << 8) |
           ((u32)aes_sbox[w & 0xFF]);
}

static u32 aes_rot_word(u32 w)
{
    return (w << 8) | (w >> 24);
}

/* FIPS-197 key schedule.  Round keys are stored as words so that the round
 * function can feed them straight into AddRoundKey. */
int aes_init(struct aes_ctx *ctx, const u8 *key, u32 key_bits)
{
    u32 nk;
    u32 rounds;
    u32 rcon = 1;
    u32 i;

    if (!ctx || !key) {
        return -1;
    }
    switch (key_bits) {
    case 128:
        nk = 4;
        rounds = 10;
        break;
    case 192:
        nk = 6;
        rounds = 12;
        break;
    case 256:
        nk = 8;
        rounds = 14;
        break;
    default:
        return -1;
    }

    ctx->rounds = rounds;
    ctx->key_words = nk * 4 + 28;

    for (i = 0; i < nk; i++) {
        ctx->rk[i] = ((u32)key[4 * i] << 24) | ((u32)key[4 * i + 1] << 16) |
                     ((u32)key[4 * i + 2] << 8) | ((u32)key[4 * i + 3]);
    }

    for (i = nk; i < 4 * (rounds + 1); i++) {
        u32 temp = ctx->rk[i - 1];
        if (i % nk == 0) {
            temp = aes_sub_word(aes_rot_word(temp)) ^ (rcon << 24);
            rcon = aes_xtime((u8)rcon);
        } else if (nk > 6 && (i % nk == 4)) {
            temp = aes_sub_word(temp);
        }
        ctx->rk[i] = ctx->rk[i - nk] ^ temp;
    }
    return 0;
}

static void aes_add_round_key(u8 state[16], const u32 *rk)
{
    for (u32 c = 0; c < 4; c++) {
        u32 w = rk[c];
        state[4 * c + 0] ^= (u8)(w >> 24);
        state[4 * c + 1] ^= (u8)(w >> 16);
        state[4 * c + 2] ^= (u8)(w >> 8);
        state[4 * c + 3] ^= (u8)(w);
    }
}

static void aes_sub_bytes(u8 s[16])
{
    for (u32 i = 0; i < 16; i++) {
        s[i] = aes_sbox[s[i]];
    }
}

static void aes_inv_sub_bytes(u8 s[16])
{
    if (!aes_rsbox_ready) {
        aes_build_rsbox();
    }
    for (u32 i = 0; i < 16; i++) {
        s[i] = aes_rsbox[s[i]];
    }
}

/* State is column major: byte (r, c) lives at s[r + 4*c]. */
static void aes_shift_rows(u8 s[16])
{
    u8 t;

    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = t;
}

static void aes_inv_shift_rows(u8 s[16])
{
    u8 t;

    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

static void aes_mix_columns(u8 s[16])
{
    for (u32 c = 0; c < 4; c++) {
        u8 *col = s + 4 * c;
        u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        u8 all = (u8)(a0 ^ a1 ^ a2 ^ a3);
        col[0] = (u8)(a0 ^ all ^ aes_xtime((u8)(a0 ^ a1)));
        col[1] = (u8)(a1 ^ all ^ aes_xtime((u8)(a1 ^ a2)));
        col[2] = (u8)(a2 ^ all ^ aes_xtime((u8)(a2 ^ a3)));
        col[3] = (u8)(a3 ^ all ^ aes_xtime((u8)(a3 ^ a0)));
    }
}

static void aes_inv_mix_columns(u8 s[16])
{
    for (u32 c = 0; c < 4; c++) {
        u8 *col = s + 4 * c;
        u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (u8)(aes_gmul(a0, 0x0E) ^ aes_gmul(a1, 0x0B) ^
                      aes_gmul(a2, 0x0D) ^ aes_gmul(a3, 0x09));
        col[1] = (u8)(aes_gmul(a0, 0x09) ^ aes_gmul(a1, 0x0E) ^
                      aes_gmul(a2, 0x0B) ^ aes_gmul(a3, 0x0D));
        col[2] = (u8)(aes_gmul(a0, 0x0D) ^ aes_gmul(a1, 0x09) ^
                      aes_gmul(a2, 0x0E) ^ aes_gmul(a3, 0x0B));
        col[3] = (u8)(aes_gmul(a0, 0x0B) ^ aes_gmul(a1, 0x0D) ^
                      aes_gmul(a2, 0x09) ^ aes_gmul(a3, 0x0E));
    }
}

void aes_encrypt_block(const struct aes_ctx *ctx, const u8 in[16], u8 out[16])
{
    u8 s[16];

    ax_memcpy(s, in, 16);
    aes_add_round_key(s, ctx->rk);
    for (u32 round = 1; round < ctx->rounds; round++) {
        aes_sub_bytes(s);
        aes_shift_rows(s);
        aes_mix_columns(s);
        aes_add_round_key(s, ctx->rk + 4 * round);
    }
    aes_sub_bytes(s);
    aes_shift_rows(s);
    aes_add_round_key(s, ctx->rk + 4 * ctx->rounds);
    ax_memcpy(out, s, 16);
}

void aes_decrypt_block(const struct aes_ctx *ctx, const u8 in[16], u8 out[16])
{
    u8 s[16];

    ax_memcpy(s, in, 16);
    aes_add_round_key(s, ctx->rk + 4 * ctx->rounds);
    for (u32 round = ctx->rounds; round > 1; round--) {
        aes_inv_shift_rows(s);
        aes_inv_sub_bytes(s);
        aes_add_round_key(s, ctx->rk + 4 * (round - 1));
        aes_inv_mix_columns(s);
    }
    aes_inv_shift_rows(s);
    aes_inv_sub_bytes(s);
    aes_add_round_key(s, ctx->rk);
    ax_memcpy(out, s, 16);
}

/* ---------------- modes -------------------------------------------- */

void aes_ecb_encrypt(const struct aes_ctx *ctx, const u8 *in, u8 *out, usize blocks)
{
    for (usize i = 0; i < blocks; i++) {
        aes_encrypt_block(ctx, in + 16 * i, out + 16 * i);
    }
}

void aes_ecb_decrypt(const struct aes_ctx *ctx, const u8 *in, u8 *out, usize blocks)
{
    for (usize i = 0; i < blocks; i++) {
        aes_decrypt_block(ctx, in + 16 * i, out + 16 * i);
    }
}

void aes_cbc_encrypt(const struct aes_ctx *ctx, u8 iv[16], const u8 *in,
                     u8 *out, usize blocks)
{
    u8 chain[16];
    u8 block[16];

    ax_memcpy(chain, iv, 16);
    for (usize i = 0; i < blocks; i++) {
        for (u32 b = 0; b < 16; b++) {
            block[b] = (u8)(in[16 * i + b] ^ chain[b]);
        }
        aes_encrypt_block(ctx, block, chain);
        ax_memcpy(out + 16 * i, chain, 16);
    }
    ax_memcpy(iv, chain, 16);
}

void aes_cbc_decrypt(const struct aes_ctx *ctx, u8 iv[16], const u8 *in,
                     u8 *out, usize blocks)
{
    u8 chain[16];
    u8 plain[16];

    ax_memcpy(chain, iv, 16);
    for (usize i = 0; i < blocks; i++) {
        aes_decrypt_block(ctx, in + 16 * i, plain);
        for (u32 b = 0; b < 16; b++) {
            out[16 * i + b] = (u8)(plain[b] ^ chain[b]);
        }
        ax_memcpy(chain, in + 16 * i, 16);
    }
    ax_memcpy(iv, chain, 16);
}

/* CTR mode turns the block cipher into a stream cipher; the counter block is
 * incremented as a 128-bit big-endian integer so that any block size works. */
void aes_ctr_crypt(const struct aes_ctx *ctx, u8 counter[16], const u8 *in,
                   u8 *out, usize bytes)
{
    u8 keystream[16];
    usize done = 0;

    while (done < bytes) {
        aes_encrypt_block(ctx, counter, keystream);
        usize chunk = bytes - done;
        if (chunk > 16) {
            chunk = 16;
        }
        for (usize i = 0; i < chunk; i++) {
            out[done + i] = (u8)(in[done + i] ^ keystream[i]);
        }
        done += chunk;
        for (i32 b = 15; b >= 0; b--) {
            counter[b]++;
            if (counter[b]) {
                break;
            }
        }
    }
}

/* PKCS#7 padding as used by the sealed crash dump. */
usize aes_pkcs7_pad(u8 *buffer, usize len, usize block)
{
    usize pad = block - (len % block);
    if (pad == 0) {
        pad = block;
    }
    for (usize i = 0; i < pad; i++) {
        buffer[len + i] = (u8)pad;
    }
    return len + pad;
}

int aes_pkcs7_unpad(const u8 *buffer, usize len, usize block, usize *out_len)
{
    (void)block;
    if (!len) {
        return -1;
    }
    u8 pad = buffer[len - 1];
    if (!pad || pad > 16 || pad > len) {
        return -1;
    }
    for (usize i = 0; i < pad; i++) {
        if (buffer[len - 1 - i] != pad) {
            return -1;
        }
    }
    *out_len = len - pad;
    return 0;
}
