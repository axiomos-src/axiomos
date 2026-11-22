/* AxiomOS - containers: bitmaps, ring buffers and intrusive lists.
 *
 * The physical page allocator tracks free frames in a bitmap, the serial and
 * network drivers keep their receive queues in rings, and the scheduler keeps
 * its run queues in intrusive lists.  None of these allocate; the caller owns
 * the storage.
 */

#include <axiom/lib.h>

/* ------------------------------------------------------------------ */
/* Bitmap                                                              */
/* ------------------------------------------------------------------ */

void ax_bitmap_init(ax_bitmap *bm, u64 *storage, usize bits)
{
    bm->words = storage;
    bm->bits = bits;
    usize words = AX_BITMAP_WORDS(bits);
    for (usize i = 0; i < words; i++) {
        storage[i] = 0;
    }
}

void ax_bitmap_set_all(ax_bitmap *bm)
{
    usize words = AX_BITMAP_WORDS(bm->bits);
    for (usize i = 0; i < words; i++) {
        bm->words[i] = ~0ULL;
    }
    /* Clear the bits past the end so scans never report a phantom hit. */
    usize tail = bm->bits % 64;
    if (tail && words) {
        bm->words[words - 1] = (1ULL << tail) - 1ULL;
    }
}

void ax_bitmap_clear_all(ax_bitmap *bm)
{
    usize words = AX_BITMAP_WORDS(bm->bits);
    for (usize i = 0; i < words; i++) {
        bm->words[i] = 0;
    }
}

bool_t ax_bitmap_test(const ax_bitmap *bm, usize bit)
{
    if (bit >= bm->bits) {
        return false;
    }
    return (bm->words[bit / 64] >> (bit % 64)) & 1ULL;
}

void ax_bitmap_set(ax_bitmap *bm, usize bit)
{
    if (bit >= bm->bits) {
        return;
    }
    bm->words[bit / 64] |= 1ULL << (bit % 64);
}

void ax_bitmap_clear(ax_bitmap *bm, usize bit)
{
    if (bit >= bm->bits) {
        return;
    }
    bm->words[bit / 64] &= ~(1ULL << (bit % 64));
}

void ax_bitmap_flip(ax_bitmap *bm, usize bit)
{
    if (bit >= bm->bits) {
        return;
    }
    bm->words[bit / 64] ^= 1ULL << (bit % 64);
}

/* Scan word by word; the popcount-free test on a whole word is a single
 * compare, so a full bitmap costs one pass over the words. */
i64 ax_bitmap_find_first_clear(const ax_bitmap *bm)
{
    usize words = AX_BITMAP_WORDS(bm->bits);

    for (usize i = 0; i < words; i++) {
        u64 w = bm->words[i];
        if (w != ~0ULL) {
            usize bit = i * 64 + (usize)ax_ctz32((u32)(~w));
            /* ctz32 sees the low 32 bits first; if those are all set, look at
             * the high half. */
            if ((u32)~w == 0) {
                bit = i * 64 + 32 + (usize)ax_ctz32((u32)(~w >> 32));
            }
            if (bit < bm->bits) {
                return (i64)bit;
            }
        }
    }
    return -1;
}

i64 ax_bitmap_find_first_set(const ax_bitmap *bm)
{
    usize words = AX_BITMAP_WORDS(bm->bits);

    for (usize i = 0; i < words; i++) {
        u64 w = bm->words[i];
        if (w) {
            usize bit = i * 64 + (usize)ax_ctz32((u32)w);
            if ((u32)w == 0) {
                bit = i * 64 + 32 + (usize)ax_ctz32((u32)(w >> 32));
            }
            if (bit < bm->bits) {
                return (i64)bit;
            }
        }
    }
    return -1;
}

/* Find the first run of `count` consecutive clear bits.  Returns the starting
 * bit or -1.  The scan is linear but skips whole words when they are full. */
i64 ax_bitmap_find_clear_range(const ax_bitmap *bm, usize count)
{
    if (count == 0 || count > bm->bits) {
        return -1;
    }
    usize run = 0;
    usize start = 0;

    for (usize bit = 0; bit < bm->bits; bit++) {
        if (ax_bitmap_test(bm, bit)) {
            run = 0;
            start = bit + 1;
            continue;
        }
        run++;
        if (run == count) {
            return (i64)start;
        }
    }
    return -1;
}

usize ax_bitmap_count_set(const ax_bitmap *bm)
{
    usize words = AX_BITMAP_WORDS(bm->bits);
    usize total = 0;

    for (usize i = 0; i < words; i++) {
        u64 w = bm->words[i];
        total += (usize)ax_popcount32((u32)w);
        total += (usize)ax_popcount32((u32)(w >> 32));
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Ring buffer                                                         */
/* ------------------------------------------------------------------ */

void ax_ring_init(ax_ring *ring, u8 *storage, usize capacity)
{
    ring->data = storage;
    ring->capacity = capacity;
    ring->read_pos = 0;
    ring->write_pos = 0;
    ring->used = 0;
}

usize ax_ring_write(ax_ring *ring, const void *src, usize len)
{
    const u8 *s = (const u8 *)src;
    usize written = 0;

    if (!ring->capacity) {
        return 0;
    }
    while (written < len && ring->used < ring->capacity) {
        ring->data[ring->write_pos] = s[written++];
        ring->write_pos = (ring->write_pos + 1) % ring->capacity;
        ring->used++;
    }
    return written;
}

usize ax_ring_peek(const ax_ring *ring, void *dst, usize len)
{
    u8 *d = (u8 *)dst;
    usize pos = ring->read_pos;
    usize n = len < ring->used ? len : ring->used;

    for (usize i = 0; i < n; i++) {
        d[i] = ring->data[pos];
        pos = (pos + 1) % ring->capacity;
    }
    return n;
}

usize ax_ring_read(ax_ring *ring, void *dst, usize len)
{
    usize n = ax_ring_peek(ring, dst, len);
    ring->read_pos = (ring->read_pos + n) % ring->capacity;
    ring->used -= n;
    return n;
}

usize ax_ring_used(const ax_ring *ring)
{
    return ring->used;
}

usize ax_ring_free(const ax_ring *ring)
{
    return ring->capacity - ring->used;
}

void ax_ring_reset(ax_ring *ring)
{
    ring->read_pos = 0;
    ring->write_pos = 0;
    ring->used = 0;
}

/* ------------------------------------------------------------------ */
/* Intrusive list                                                      */
/* ------------------------------------------------------------------ */

void ax_list_init(ax_node *head)
{
    head->next = head;
    head->prev = head;
}

bool_t ax_list_empty(const ax_node *head)
{
    return head->next == head;
}

static void list_link(ax_node *prev, ax_node *next, ax_node *item)
{
    item->prev = prev;
    item->next = next;
    prev->next = item;
    next->prev = item;
}

void ax_list_push_front(ax_node *head, ax_node *item)
{
    list_link(head, head->next, item);
}

void ax_list_push_back(ax_node *head, ax_node *item)
{
    list_link(head->prev, head, item);
}

void ax_list_insert_after(ax_node *pos, ax_node *item)
{
    list_link(pos, pos->next, item);
}

void ax_list_insert_before(ax_node *pos, ax_node *item)
{
    list_link(pos->prev, pos, item);
}

void ax_list_remove(ax_node *item)
{
    item->prev->next = item->next;
    item->next->prev = item->prev;
    item->next = item;
    item->prev = item;
}

ax_node *ax_list_pop_front(ax_node *head)
{
    if (ax_list_empty(head)) {
        return NULL_PTR;
    }
    ax_node *item = head->next;
    ax_list_remove(item);
    return item;
}

ax_node *ax_list_pop_back(ax_node *head)
{
    if (ax_list_empty(head)) {
        return NULL_PTR;
    }
    ax_node *item = head->prev;
    ax_list_remove(item);
    return item;
}

usize ax_list_length(const ax_node *head)
{
    usize n = 0;
    for (const ax_node *p = head->next; p != head; p = p->next) {
        n++;
    }
    return n;
}

ax_node *ax_list_nth(const ax_node *head, usize index)
{
    usize i = 0;
    for (ax_node *p = head->next; p != head; p = p->next) {
        if (i++ == index) {
            return p;
        }
    }
    return NULL_PTR;
}
