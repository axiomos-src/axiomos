/* AxiomOS - kernel base library interface.
 *
 * The kernel is freestanding, so it brings its own implementations of the
 * byte and string primitives, the number formatters and the few generic
 * algorithms (sorting, binary search, checksums) that the rest of the tree
 * needs.  Everything here is re-entrant and does not allocate; nothing in this
 * header depends on the heap.
 *
 * Naming: the public symbols carry an "ax_" prefix so that the kernel can be
 * linked next to Rust crates that pull in their own libc-like shims without
 * fighting over the classic names (memcpy, strlen, ...).
 */
#ifndef AXIOM_LIB_H
#define AXIOM_LIB_H

#include <axiom/types.h>

/* ------------------------------------------------------------------ */
/* Memory primitives                                                   */
/* ------------------------------------------------------------------ */

void     *ax_memcpy(void *dst, const void *src, usize n);
void     *ax_memset(void *dst, u8 value, usize n);
void     *ax_memmove(void *dst, const void *src, usize n);
int       ax_memcmp(const void *a, const void *b, usize n);
void     *ax_memchr(const void *p, u8 value, usize n);
void     *ax_memrchr(const void *p, u8 value, usize n);
usize     ax_memcount(const void *p, u8 value, usize n);
void      ax_memswap(void *a, void *b, usize n);
void      ax_memzero(void *dst, usize n);
bool_t    ax_memeq(const void *a, const void *b, usize n);
void     *ax_memcpy_volatile(void *dst, const void *src, usize n);
void      ax_memset_volatile(void *dst, u8 value, usize n);

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

usize     ax_strlen(const char *s);
usize     ax_strnlen(const char *s, usize max);
int       ax_strcmp(const char *a, const char *b);
int       ax_strcasecmp(const char *a, const char *b);
int       ax_strncmp(const char *a, const char *b, usize n);
char     *ax_strcpy(char *dst, const char *src);
char     *ax_strncpy(char *dst, const char *src, usize n);
usize     ax_strlcpy(char *dst, const char *src, usize cap);
char     *ax_strcat(char *dst, const char *src);
char     *ax_strncat(char *dst, const char *src, usize n);
usize     ax_strlcat(char *dst, const char *src, usize cap);
char     *ax_strchr(const char *s, char c);
char     *ax_strrchr(const char *s, char c);
char     *ax_strstr(const char *haystack, const char *needle);
char     *ax_strnstr(const char *haystack, const char *needle, usize n);
bool_t    ax_startswith(const char *s, const char *prefix);
bool_t    ax_endswith(const char *s, const char *suffix);
char     *ax_trim(char *s);
char     *ax_trim_end(char *s);
usize     ax_tokenize(char *s, char sep, char **out, usize max_tokens);
void      ax_str_reverse(char *s);
char     *ax_str_lower(char *s);
char     *ax_str_upper(char *s);
void      ax_str_join(char *dst, usize cap, const char *const *parts, usize count,
                      const char *sep);

/* ------------------------------------------------------------------ */
/* Character classification                                            */
/* ------------------------------------------------------------------ */

AXIOM_INLINE bool_t ax_isdigit(char c) { return c >= '0' && c <= '9'; }
AXIOM_INLINE bool_t ax_isupper(char c) { return c >= 'A' && c <= 'Z'; }
AXIOM_INLINE bool_t ax_islower(char c) { return c >= 'a' && c <= 'z'; }
AXIOM_INLINE bool_t ax_isalpha(char c) { return ax_isupper(c) || ax_islower(c); }
AXIOM_INLINE bool_t ax_isalnum(char c) { return ax_isalpha(c) || ax_isdigit(c); }
AXIOM_INLINE bool_t ax_isxdigit(char c)
{
    return ax_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
AXIOM_INLINE bool_t ax_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
AXIOM_INLINE bool_t ax_iscntrl(char c) { return (u8)c < 0x20 || (u8)c == 0x7F; }
AXIOM_INLINE bool_t ax_isprint(char c) { return (u8)c >= 0x20 && (u8)c < 0x7F; }
AXIOM_INLINE bool_t ax_isprint_high(char c) { return (u8)c >= 0x20; }
AXIOM_INLINE char ax_tolower(char c)
{
    return ax_isupper(c) ? (char)(c - 'A' + 'a') : c;
}
AXIOM_INLINE char ax_toupper(char c)
{
    return ax_islower(c) ? (char)(c - 'a' + 'A') : c;
}
AXIOM_INLINE int ax_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Number conversion                                                   */
/* ------------------------------------------------------------------ */

i64       ax_atoi(const char *s);
u64       ax_atou(const char *s);
i64       ax_strtol(const char *s, char **end, int base);
u64       ax_strtoull(const char *s, char **end, int base);
i64       ax_strtoll(const char *s, char **end, int base);
int       ax_parse_size(const char *s, u64 *out);      /* "64M", "1G", "512" */
int       ax_parse_bool(const char *s, bool_t *out);   /* "on/off/true/false" */
char     *ax_utoa(u64 value, char *buf);
char     *ax_itoa(i64 value, char *buf);
char     *ax_utox(u64 value, char *buf, bool_t prefix);
char     *ax_utoa_padded(u64 value, char *buf, u32 width, u8 pad);
u32       ax_str_to_hash(const char *s);

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

/* Subset of C99 printf: %d %i %u %o %x %X %b %c %s %p %% with the flags
 * '-' '0' '+' ' ' '#' , an optional field width, an optional precision and the
 * length modifiers hh h l ll z.  Returns the number of characters that would
 * have been written, like the standard. */
int       ax_vsnprintf(char *dst, usize cap, const char *fmt, __builtin_va_list args);
int       ax_snprintf(char *dst, usize cap, const char *fmt, ...) AXIOM_PRINTF(3, 4);
int       ax_vscnprintf(char *dst, usize cap, const char *fmt, __builtin_va_list args);

/* ------------------------------------------------------------------ */
/* Algorithms                                                          */
/* ------------------------------------------------------------------ */

typedef int (*ax_compare_fn)(const void *a, const void *b, void *ctx);
typedef void (*ax_visit_fn)(void *item, void *ctx);

void      ax_qsort(void *base, usize count, usize size, ax_compare_fn cmp, void *ctx);
void      ax_insertion_sort(void *base, usize count, usize size, ax_compare_fn cmp, void *ctx);
void     *ax_bsearch(const void *key, const void *base, usize count, usize size,
                     ax_compare_fn cmp, void *ctx);
usize     ax_bsearch_index(const void *key, const void *base, usize count, usize size,
                           ax_compare_fn cmp, void *ctx, bool_t *found);
u32       ax_hash32(const void *data, usize len);
u64       ax_hash64(const void *data, usize len);
u32       ax_crc32(const void *data, usize len);
u16       ax_crc16(const void *data, usize len);
u64       ax_fnv1a64(const u8 *data, usize len);
u32       ax_rotl32(u32 v, u8 n);
u64       ax_rotl64(u64 v, u8 n);
u32       ax_log2_32(u32 v);
u64       ax_log2_64(u64 v);
u64       ax_isqrt(u64 v);
u32       ax_popcount32(u32 v);
u32       ax_clz32(u32 v);
u32       ax_ctz32(u32 v);
u64       ax_pow64(u64 base, u32 exp);
u64       ax_gcd64(u64 a, u64 b);

/* ------------------------------------------------------------------ */
/* Bitmaps and ring buffers                                            */
/* ------------------------------------------------------------------ */

#define AX_BITMAP_WORDS(bits) (((bits) + 63u) / 64u)

typedef struct {
    u64  *words;
    usize bits;
} ax_bitmap;

void      ax_bitmap_init(ax_bitmap *bm, u64 *storage, usize bits);
void      ax_bitmap_set_all(ax_bitmap *bm);
void      ax_bitmap_clear_all(ax_bitmap *bm);
bool_t    ax_bitmap_test(const ax_bitmap *bm, usize bit);
void      ax_bitmap_set(ax_bitmap *bm, usize bit);
void      ax_bitmap_clear(ax_bitmap *bm, usize bit);
void      ax_bitmap_flip(ax_bitmap *bm, usize bit);
i64       ax_bitmap_find_first_clear(const ax_bitmap *bm);
i64       ax_bitmap_find_first_set(const ax_bitmap *bm);
i64       ax_bitmap_find_clear_range(const ax_bitmap *bm, usize count);
usize     ax_bitmap_count_set(const ax_bitmap *bm);

typedef struct {
    u8    *data;
    usize  capacity;
    usize  read_pos;
    usize  write_pos;
    usize  used;
} ax_ring;

void      ax_ring_init(ax_ring *ring, u8 *storage, usize capacity);
usize     ax_ring_write(ax_ring *ring, const void *src, usize len);
usize     ax_ring_read(ax_ring *ring, void *dst, usize len);
usize     ax_ring_peek(const ax_ring *ring, void *dst, usize len);
usize     ax_ring_used(const ax_ring *ring);
usize     ax_ring_free(const ax_ring *ring);
void      ax_ring_reset(ax_ring *ring);

/* ------------------------------------------------------------------ */
/* Intrusive linked list                                               */
/* ------------------------------------------------------------------ */

typedef struct ax_node {
    struct ax_node *next;
    struct ax_node *prev;
} ax_node;

void      ax_list_init(ax_node *head);
bool_t    ax_list_empty(const ax_node *head);
void      ax_list_push_front(ax_node *head, ax_node *item);
void      ax_list_push_back(ax_node *head, ax_node *item);
void      ax_list_insert_after(ax_node *pos, ax_node *item);
void      ax_list_insert_before(ax_node *pos, ax_node *item);
void      ax_list_remove(ax_node *item);
ax_node  *ax_list_pop_front(ax_node *head);
ax_node  *ax_list_pop_back(ax_node *head);
usize     ax_list_length(const ax_node *head);
ax_node  *ax_list_nth(const ax_node *head, usize index);

#define AX_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - (usize)&(((type *)0)->member)))

#define AX_LIST_FOR_EACH(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define AX_LIST_FOR_EACH_SAFE(pos, tmp, head) \
    for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head); \
         (pos) = (tmp), (tmp) = (pos)->next)

#endif /* AXIOM_LIB_H */
