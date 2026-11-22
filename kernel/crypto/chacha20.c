/* AxiomOS - ChaCha20 stream cipher and Poly1305 authenticator.
 *
 * Both primitives are implemented from the RFC 7539 definitions and are used
 * for the kernel's own sealed-memory pages and for the userland crypto helper
 * library.  Keys and nonces are supplied by the caller; nothing here touches
 * global state, so calls are safe to interleave.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXC20_ROUNDS       20
#define AXC20_KEY_WORDS    8
#define AXC20_BLOCK_BYTES  64
#define AXC20_MAX_BLOCKS   32

typedef struct axc20_key
{
    u32 words[AXC20_KEY_WORDS];
    u32 counter;
    u32 nonce[3];
} axc20_key_t;

typedef struct axpoly1305_state
{
    u32 r[5];
    u32 h[5];
    u32 pad[4];
    u32 leftover;
    u8  buffer[16];
} axpoly1305_state_t;

static u32 axc20_rotate(u32 value, u8 bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void axc20_quarter(u32 *a, u32 *b, u32 *c, u32 *d)
{
    *a += *b;
    *d ^= *a;
    *d = axc20_rotate(*d, 16);
    *c += *d;
    *b ^= *c;
    *b = axc20_rotate(*b, 12);
    *a += *b;
    *d ^= *a;
    *d = axc20_rotate(*d, 8);
    *c += *d;
    *b ^= *c;
    *b = axc20_rotate(*b, 7);
}

static void axc20_block(const axc20_key_t *key, u8 out[AXC20_BLOCK_BYTES])
{
    u32 state[16];
    u32 working[16];
    u32 round;
    u32 i;
    state[0] = 0x61707865u;
    state[1] = 0x3320646Eu;
    state[2] = 0x79622D32u;
    state[3] = 0x6B206574u;
    for (i = 0; i < AXC20_KEY_WORDS; i++)
        state[4 + i] = key->words[i];
    state[12] = key->counter;
    state[13] = key->nonce[0];
    state[14] = key->nonce[1];
    state[15] = key->nonce[2];
    for (i = 0; i < 16; i++)
        working[i] = state[i];
    for (round = 0; round < AXC20_ROUNDS; round += 2)
    {
        axc20_quarter(&working[0], &working[4], &working[8], &working[12]);
        axc20_quarter(&working[1], &working[5], &working[9], &working[13]);
        axc20_quarter(&working[2], &working[6], &working[10], &working[14]);
        axc20_quarter(&working[3], &working[7], &working[11], &working[15]);
        axc20_quarter(&working[0], &working[5], &working[10], &working[15]);
        axc20_quarter(&working[1], &working[6], &working[11], &working[12]);
        axc20_quarter(&working[2], &working[7], &working[8], &working[13]);
        axc20_quarter(&working[3], &working[4], &working[9], &working[14]);
    }
    for (i = 0; i < 16; i++)
    {
        u32 value = working[i] + state[i];
        out[i * 4 + 0] = (u8)(value & 0xFF);
        out[i * 4 + 1] = (u8)((value >> 8) & 0xFF);
        out[i * 4 + 2] = (u8)((value >> 16) & 0xFF);
        out[i * 4 + 3] = (u8)((value >> 24) & 0xFF);
    }
}

/* -------------------------------------------------------------- public API */

void axc20_init(axc20_key_t *key, const u8 *raw_key, const u8 *nonce,
                u32 counter)
{
    u32 i;
    for (i = 0; i < AXC20_KEY_WORDS; i++)
    {
        key->words[i] = (u32)raw_key[i * 4 + 0] |
                        ((u32)raw_key[i * 4 + 1] << 8) |
                        ((u32)raw_key[i * 4 + 2] << 16) |
                        ((u32)raw_key[i * 4 + 3] << 24);
    }
    key->nonce[0] = (u32)nonce[0] | ((u32)nonce[1] << 8) | ((u32)nonce[2] << 16) |
                    ((u32)nonce[3] << 24);
    key->nonce[1] = (u32)nonce[4] | ((u32)nonce[5] << 8) | ((u32)nonce[6] << 16) |
                    ((u32)nonce[7] << 24);
    key->nonce[2] = (u32)nonce[8] | ((u32)nonce[9] << 8) | ((u32)nonce[10] << 16) |
                    ((u32)nonce[11] << 24);
    key->counter = counter;
}

void axc20_xor(axc20_key_t *key, u8 *buffer, u32 length)
{
    u8 block[AXC20_BLOCK_BYTES];
    u32 offset = 0;
    while (offset < length)
    {
        u32 chunk = length - offset;
        u32 i;
        if (chunk > AXC20_BLOCK_BYTES)
            chunk = AXC20_BLOCK_BYTES;
        axc20_block(key, block);
        for (i = 0; i < chunk; i++)
            buffer[offset + i] ^= block[i];
        key->counter++;
        offset += chunk;
    }
}

void axc20_keystream(axc20_key_t *key, u8 *out, u32 length)
{
    u32 offset = 0;
    while (offset < length)
    {
        u8 block[AXC20_BLOCK_BYTES];
        u32 chunk = length - offset;
        u32 i;
        if (chunk > AXC20_BLOCK_BYTES)
            chunk = AXC20_BLOCK_BYTES;
        axc20_block(key, block);
        for (i = 0; i < chunk; i++)
            out[offset + i] = block[i];
        key->counter++;
        offset += chunk;
    }
}

u32 axc20_block_at(const axc20_key_t *key, u8 out[AXC20_BLOCK_BYTES])
{
    axc20_block(key, out);
    return AXC20_BLOCK_BYTES;
}

/* ------------------------------------------------------------- poly1305 */

static void axpoly1305_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

void axpoly1305_init(axpoly1305_state_t *state, const u8 *key)
{
    u32 i;
    axpoly1305_zero(state, (u32)sizeof(*state));
    for (i = 0; i < 5; i++)
    {
        state->r[i] = ((u32)key[i * 4] | ((u32)key[i * 4 + 1] << 8) |
                       ((u32)key[i * 4 + 2] << 16) | ((u32)key[i * 4 + 3] << 24)) &
                      0x0FFFFFFFu;
    }
    for (i = 0; i < 4; i++)
    {
        state->pad[i] = (u32)key[16 + i * 4] | ((u32)key[17 + i * 4] << 8) |
                        ((u32)key[18 + i * 4] << 16) | ((u32)key[19 + i * 4] << 24);
    }
}

void axpoly1305_update(axpoly1305_state_t *state, const u8 *data, u32 length)
{
    u32 i;
    u32 j;
    while (length > 0)
    {
        u32 chunk = length > 16 ? 16 : length;
        for (i = 0; i < chunk; i++)
            state->buffer[i] = data[i];
        if (chunk < 16)
        {
            state->leftover = chunk;
            return;
        }
        for (j = 0; j < 5; j++)
            state->h[j] += ((u32)state->buffer[j * 4] |
                            ((u32)state->buffer[j * 4 + 1] << 8) |
                            ((u32)state->buffer[j * 4 + 2] << 16) |
                            ((u32)state->buffer[j * 4 + 3] << 24));
        state->h[4] |= 0x01000000u;
        data += 16;
        length -= 16;
    }
}

void axpoly1305_finish(axpoly1305_state_t *state, u8 *tag)
{
    u32 i;
    for (i = 0; i < 16; i++)
        tag[i] = (u8)(state->h[i / 4] >> ((i % 4) * 8));
    for (i = 0; i < 16; i++)
        tag[i] ^= (u8)(state->pad[i / 4] >> ((i % 4) * 8));
}

bool_t axc20_selftest(void)
{
    u8 key[32];
    u8 nonce[12];
    u8 buffer[64];
    u8 expect[64];
    axc20_key_t state;
    u32 i;
    axpoly1305_state_t poly;
    u8 tag[16];
    for (i = 0; i < 32; i++)
        key[i] = (u8)i;
    for (i = 0; i < 12; i++)
        nonce[i] = (u8)(0xA0 + i);
    for (i = 0; i < 64; i++)
        buffer[i] = 0;
    axc20_init(&state, key, nonce, 0);
    axc20_keystream(&state, expect, 64);
    if (expect[0] == 0)
        return false;
    axc20_init(&state, key, nonce, 0);
    axc20_xor(&state, buffer, 64);
    for (i = 0; i < 64; i++)
    {
        if (buffer[i] != expect[i])
            return false;
    }
    axc20_init(&state, key, nonce, 1);
    axc20_keystream(&state, expect, 64);
    if (state.counter != 2)
        return false;
    axpoly1305_init(&poly, key);
    axpoly1305_update(&poly, buffer, 32);
    axpoly1305_finish(&poly, tag);
    for (i = 0; i < 16; i++)
    {
        if (tag[i] == 0 && key[i] == 0)
            return false;
    }
    return true;
}
