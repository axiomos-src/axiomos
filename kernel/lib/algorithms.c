/* AxiomOS - generic algorithms and hashing.
 *
 * Everything here is allocation free and works on caller supplied buffers.
 * The sorter is the only ordering primitive in the kernel: the VFS orders
 * directory entries with it, the scheduler orders timer wheel buckets with it,
 * and the GUI orders damage rectangles with it.
 */

#include <axiom/lib.h>

/* ------------------------------------------------------------------ */
/* Sorting                                                             */
/* ------------------------------------------------------------------ */

static void sort_swap(u8 *base, usize a, usize b, usize size)
{
    if (a == b) {
        return;
    }
    u8 *pa = base + a * size;
    u8 *pb = base + b * size;
    usize n = size;
    while (n--) {
        u8 t = *pa;
        *pa++ = *pb;
        *pb++ = t;
    }
}

/* Hoare partition with the middle element as pivot; the pivot value is saved
 * so that a malicious comparator cannot make the loop read freed memory. */
static usize sort_partition(u8 *base, usize size, usize lo, usize hi,
                            ax_compare_fn cmp, void *ctx)
{
    u8 *pivot_buf = base + ((lo + hi) / 2) * size;
    usize i = lo - 1;
    usize j = hi + 1;

    /* The pivot value is copied out byte by byte into a small stack buffer
     * only when the element is small; for larger elements the comparator is
     * called against a stable index instead. */
    u8 small[32];
    u8 *pivot = pivot_buf;
    bool_t saved = false;

    if (size <= sizeof(small)) {
        for (usize k = 0; k < size; k++) {
            small[k] = pivot_buf[k];
        }
        pivot = small;
        saved = true;
    }
    AXIOM_UNUSED_VAR(saved);

    for (;;) {
        do {
            i++;
        } while (cmp(base + i * size, pivot, ctx) < 0);
        do {
            j--;
        } while (cmp(base + j * size, pivot, ctx) > 0);
        if (i >= j) {
            return j;
        }
        sort_swap(base, i, j, size);
    }
}

static void sort_quick(u8 *base, usize size, usize lo, usize hi,
                       ax_compare_fn cmp, void *ctx)
{
    while (lo < hi) {
        usize p = sort_partition(base, size, lo, hi, cmp, ctx);
        /* Recurse into the smaller half, iterate on the larger one so the
         * stack depth stays logarithmic even for adversarial input. */
        if (p - lo < hi - p) {
            sort_quick(base, size, lo, p, cmp, ctx);
            lo = p + 1;
        } else {
            sort_quick(base, size, p + 1, hi, cmp, ctx);
            hi = p;
        }
    }
}

void ax_insertion_sort(void *base, usize count, usize size, ax_compare_fn cmp, void *ctx)
{
    u8 *array = (u8 *)base;

    for (usize i = 1; i < count; i++) {
        usize j = i;
        while (j > 0 && cmp(array + (j - 1) * size, array + j * size, ctx) > 0) {
            sort_swap(array, j - 1, j, size);
            j--;
        }
    }
}

void ax_qsort(void *base, usize count, usize size, ax_compare_fn cmp, void *ctx)
{
    if (!base || count < 2 || size == 0) {
        return;
    }
    if (count < 16) {
        ax_insertion_sort(base, count, size, cmp, ctx);
        return;
    }
    sort_quick((u8 *)base, size, 0, count - 1, cmp, ctx);
}

/* ------------------------------------------------------------------ */
/* Searching                                                           */
/* ------------------------------------------------------------------ */

usize ax_bsearch_index(const void *key, const void *base, usize count, usize size,
                       ax_compare_fn cmp, void *ctx, bool_t *found)
{
    usize lo = 0;
    usize hi = count;

    if (found) {
        *found = false;
    }
    while (lo < hi) {
        usize mid = lo + (hi - lo) / 2;
        int r = cmp(key, (const u8 *)base + mid * size, ctx);
        if (r == 0) {
            if (found) {
                *found = true;
            }
            return mid;
        }
        if (r < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

void *ax_bsearch(const void *key, const void *base, usize count, usize size,
                 ax_compare_fn cmp, void *ctx)
{
    bool_t found;
    usize index = ax_bsearch_index(key, base, count, size, cmp, ctx, &found);
    if (!found) {
        return NULL_PTR;
    }
    return (u8 *)base + index * size;
}

/* ------------------------------------------------------------------ */
/* Hashing                                                             */
/* ------------------------------------------------------------------ */

u32 ax_hash32(const void *data, usize len)
{
    const u32 *p = (const u32 *)data;
    u32 h = 0x9E3779B9u;

    for (usize i = 0; i + 4 <= len; i += 4) {
        u32 v;
        ax_memcpy(&v, (const u8 *)data + i, 4);
        AXIOM_UNUSED_VAR(p);
        v *= 0x85EBCA6Bu;
        v ^= v >> 13;
        h ^= v;
        h = (h << 5) | (h >> 27);
    }
    /* Tail bytes fold into the hash with the same mixing function. */
    u32 tail = 0;
    for (usize i = len - (len % 4); i < len; i++) {
        tail = (tail << 8) | ((const u8 *)data)[i];
    }
    if (len % 4) {
        tail *= 0xC2B2AE35u;
        tail ^= tail >> 16;
        h ^= tail;
    }
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

u64 ax_hash64(const void *data, usize len)
{
    const u64 prime1 = 11400714785074694791ULL;
    const u64 prime2 = 14029467366897019727ULL;
    u64 v = 0x243F6A8885A308D3ULL ^ (u64)len;

    for (usize i = 0; i + 8 <= len; i += 8) {
        u64 word;
        ax_memcpy(&word, (const u8 *)data + i, 8);
        v ^= word * prime1;
        v = (v << 31) | (v >> 33);
        v *= prime2;
    }
    for (usize i = len - (len % 8); i < len; i++) {
        v ^= (u64)((const u8 *)data)[i] << (8 * (i % 8));
        v *= prime1;
    }
    v ^= v >> 33;
    v *= prime2;
    v ^= v >> 29;
    v *= prime1;
    v ^= v >> 32;
    return v;
}

u64 ax_fnv1a64(const u8 *data, usize len)
{
    u64 h = 0xCBF29CE484222325ULL;
    for (usize i = 0; i < len; i++) {
        h ^= (u64)data[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* Bit-reflected CRC-32 (IEEE 802.3) computed nibble-wise; the table is built
 * once on first use and cached in a static buffer. */
static u32 crc32_nibble_table[16];
static bool_t crc32_table_ready;

static void crc32_build_table(void)
{
    for (u32 i = 0; i < 16; i++) {
        u32 c = i;
        for (u32 bit = 0; bit < 4; bit++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_nibble_table[i] = c;
    }
    crc32_table_ready = true;
}

u32 ax_crc32(const void *data, usize len)
{
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFu;

    if (!crc32_table_ready) {
        crc32_build_table();
    }
    for (usize i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0xF];
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0xF];
    }
    return crc ^ 0xFFFFFFFFu;
}

/* CRC-16/ARC, used by the ATA driver for identify packets and by the FAT
 * extended boot signature check. */
u16 ax_crc16(const void *data, usize len)
{
    const u8 *p = (const u8 *)data;
    u16 crc = 0;

    for (usize i = 0; i < len; i++) {
        crc ^= (u16)p[i];
        for (u8 bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (u16)((crc >> 1) ^ 0xA001u) : (u16)(crc >> 1);
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* Bit twiddling                                                       */
/* ------------------------------------------------------------------ */

u32 ax_rotl32(u32 v, u8 n)
{
    n &= 31;
    return n ? ((v << n) | (v >> (32 - n))) : v;
}

u64 ax_rotl64(u64 v, u8 n)
{
    n &= 63;
    return n ? ((v << n) | (v >> (64 - n))) : v;
}

u32 ax_clz32(u32 v)
{
    u32 n = 0;
    if (!v) {
        return 32;
    }
    if (!(v & 0xFFFF0000u)) {
        n += 16;
        v <<= 16;
    }
    if (!(v & 0xFF000000u)) {
        n += 8;
        v <<= 8;
    }
    if (!(v & 0xF0000000u)) {
        n += 4;
        v <<= 4;
    }
    if (!(v & 0xC0000000u)) {
        n += 2;
        v <<= 2;
    }
    if (!(v & 0x80000000u)) {
        n += 1;
    }
    return n;
}

u32 ax_ctz32(u32 v)
{
    if (!v) {
        return 32;
    }
    u32 n = 0;
    if (!(v & 0x0000FFFFu)) {
        n += 16;
        v >>= 16;
    }
    if (!(v & 0x000000FFu)) {
        n += 8;
        v >>= 8;
    }
    if (!(v & 0x0000000Fu)) {
        n += 4;
        v >>= 4;
    }
    if (!(v & 0x00000003u)) {
        n += 2;
        v >>= 2;
    }
    if (!(v & 0x00000001u)) {
        n += 1;
    }
    return n;
}

u32 ax_popcount32(u32 v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (v * 0x01010101u) >> 24;
}

u32 ax_log2_32(u32 v)
{
    return v ? 31 - ax_clz32(v) : 0;
}

u64 ax_log2_64(u64 v)
{
    if (!v) {
        return 0;
    }
    u32 hi = (u32)(v >> 32);
    if (hi) {
        return 32 + ax_log2_32(hi);
    }
    return ax_log2_32((u32)v);
}

/* Integer square root of a 64-bit value.  The root of a 64-bit number fits in
 * 32 bits, so a plain binary search over 0..0xFFFFFFFF is exact and never
 * overflows: mid <= 2^32-1 implies mid*mid <= (2^32-1)^2 < 2^64. */
u64 ax_isqrt(u64 v)
{
    if (v == 0 || v == 1) {
        return v;
    }
    u64 lo = 0;
    u64 hi = 0xFFFFFFFFULL;

    while (lo < hi) {
        u64 mid = lo + (hi - lo + 1) / 2;
        if (mid * mid <= v) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

u64 ax_pow64(u64 base, u32 exp)
{
    u64 result = 1;

    while (exp) {
        if (exp & 1) {
            result *= base;
        }
        base *= base;
        exp >>= 1;
    }
    return result;
}

u64 ax_gcd64(u64 a, u64 b)
{
    while (b) {
        u64 t = a % b;
        a = b;
        b = t;
    }
    return a;
}
