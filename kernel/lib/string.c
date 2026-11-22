/* AxiomOS - byte and string primitives.
 *
 * These are the hot paths of the kernel: the page allocator zeroes memory
 * through ax_memset, the frame buffer renderer copies rows through ax_memcpy,
 * and the console formatter walks strings through ax_vsnprintf.  The
 * implementations therefore prefer simple, branch-light loops that the
 * compiler can vectorise with SSE2 only when the build allows it; the kernel is
 * built with -mno-sse, so these stay scalar on purpose.
 */

#include <axiom/lib.h>

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

void *ax_memcpy(void *dst, const void *src, usize n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    /* Forward-overlapping copies are the common case when the loader places
     * ELF segments into a freshly allocated contiguous block; word moves first
     * then byte moves. */
    while (n >= 8) {
        *(u64 *)d = *(const u64 *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *ax_memcpy_volatile(void *dst, const void *src, usize n)
{
    volatile u8 *d = (volatile u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *ax_memset(void *dst, u8 value, usize n)
{
    u8 *d = (u8 *)dst;
    u64 pattern = (u64)value;
    pattern |= pattern << 8;
    pattern |= pattern << 16;
    pattern |= pattern << 32;

    while (n >= 8) {
        *(u64 *)d = pattern;
        d += 8;
        n -= 8;
    }
    while (n--) {
        *d++ = value;
    }
    return dst;
}

void ax_memset_volatile(void *dst, u8 value, usize n)
{
    volatile u8 *d = (volatile u8 *)dst;
    while (n--) {
        *d++ = value;
    }
}

void ax_memzero(void *dst, usize n)
{
    ax_memset(dst, 0, n);
}

void *ax_memmove(void *dst, const void *src, usize n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s || d >= s + n) {
        return ax_memcpy(dst, src, n);
    }

    /* Overlapping with dst above src: copy backwards so the tail is
     * preserved. */
    d += n;
    s += n;
    while (n >= 8) {
        d -= 8;
        s -= 8;
        *(u64 *)d = *(const u64 *)s;
        n -= 8;
    }
    while (n--) {
        *--d = *--s;
    }
    return dst;
}

int ax_memcmp(const void *a, const void *b, usize n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;

    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

bool_t ax_memeq(const void *a, const void *b, usize n)
{
    return ax_memcmp(a, b, n) == 0;
}

void *ax_memchr(const void *p, u8 value, usize n)
{
    const u8 *s = (const u8 *)p;
    while (n--) {
        if (*s == value) {
            return (void *)s;
        }
        s++;
    }
    return NULL_PTR;
}

void *ax_memrchr(const void *p, u8 value, usize n)
{
    const u8 *s = (const u8 *)p;
    s += n;
    while (n--) {
        s--;
        if (*s == value) {
            return (void *)s;
        }
    }
    return NULL_PTR;
}

usize ax_memcount(const void *p, u8 value, usize n)
{
    const u8 *s = (const u8 *)p;
    usize hits = 0;

    while (n--) {
        if (*s == value) {
            hits++;
        }
        s++;
    }
    return hits;
}

/* Swap two equally sized buffers without a temporary allocation.  Used by the
 * sorter and by the window manager when it exchanges damage rectangles. */
void ax_memswap(void *a, void *b, usize n)
{
    u8 *x = (u8 *)a;
    u8 *y = (u8 *)b;

    while (n--) {
        u8 t = *x;
        *x++ = *y;
        *y++ = t;
    }
}

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

usize ax_strlen(const char *s)
{
    const char *p = s;
    while (*p) {
        p++;
    }
    return (usize)(p - s);
}

usize ax_strnlen(const char *s, usize max)
{
    usize n = 0;
    while (n < max && s[n]) {
        n++;
    }
    return n;
}

int ax_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int ax_strcasecmp(const char *a, const char *b)
{
    while (*a && ax_tolower(*a) == ax_tolower(*b)) {
        a++;
        b++;
    }
    return (int)ax_tolower(*a) - (int)ax_tolower(*b);
}

int ax_strncmp(const char *a, const char *b, usize n)
{
    while (n--) {
        if (*a != *b) {
            return (int)(u8)*a - (int)(u8)*b;
        }
        if (!*a) {
            return 0;
        }
        a++;
        b++;
    }
    return 0;
}

char *ax_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

char *ax_strncpy(char *dst, const char *src, usize n)
{
    usize i = 0;
    while (i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    while (i < n) {
        dst[i++] = 0;
    }
    return dst;
}

/* Bounded copy that always leaves room for the terminator; returns the
 * number of characters actually stored, like strlcpy(3). */
usize ax_strlcpy(char *dst, const char *src, usize cap)
{
    usize len = ax_strlen(src);

    if (cap == 0) {
        return len;
    }
    usize copy = len < cap - 1 ? len : cap - 1;
    ax_memcpy(dst, src, copy);
    dst[copy] = 0;
    return copy;
}

char *ax_strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) {
        d++;
    }
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

char *ax_strncat(char *dst, const char *src, usize n)
{
    char *d = dst;
    while (*d) {
        d++;
    }
    while (n-- && *src) {
        *d++ = *src++;
    }
    *d = 0;
    return dst;
}

usize ax_strlcat(char *dst, const char *src, usize cap)
{
    usize dlen = ax_strnlen(dst, cap);
    usize slen = ax_strlen(src);

    if (dlen == cap) {
        return cap + slen;
    }
    usize space = cap - dlen - 1;
    usize copy = slen < space ? slen : space;
    ax_memcpy(dst + dlen, src, copy);
    dst[dlen + copy] = 0;
    return dlen + copy;
}

char *ax_strchr(const char *s, char c)
{
    while (*s) {
        if (*s == c) {
            return (char *)s;
        }
        s++;
    }
    return c == 0 ? (char *)s : NULL_PTR;
}

char *ax_strrchr(const char *s, char c)
{
    const char *last = NULL_PTR;

    while (*s) {
        if (*s == c) {
            last = s;
        }
        s++;
    }
    if (c == 0) {
        return (char *)s;
    }
    return (char *)last;
}

char *ax_strstr(const char *haystack, const char *needle)
{
    usize nlen = ax_strlen(needle);

    if (nlen == 0) {
        return (char *)haystack;
    }
    for (; *haystack; haystack++) {
        if (haystack[0] == needle[0] &&
            ax_strncmp(haystack, needle, nlen) == 0) {
            return (char *)haystack;
        }
    }
    return NULL_PTR;
}

char *ax_strnstr(const char *haystack, const char *needle, usize n)
{
    usize nlen = ax_strlen(needle);

    if (nlen == 0) {
        return (char *)haystack;
    }
    while (n >= nlen) {
        if (ax_strncmp(haystack, needle, nlen) == 0) {
            return (char *)haystack;
        }
        haystack++;
        n--;
    }
    return NULL_PTR;
}

bool_t ax_startswith(const char *s, const char *prefix)
{
    usize n = ax_strlen(prefix);
    return ax_strncmp(s, prefix, n) == 0;
}

bool_t ax_endswith(const char *s, const char *suffix)
{
    usize slen = ax_strlen(s);
    usize tlen = ax_strlen(suffix);

    if (tlen > slen) {
        return false;
    }
    return ax_strcmp(s + slen - tlen, suffix) == 0;
}

/* In-place trim of leading and trailing whitespace; returns the new start. */
char *ax_trim(char *s)
{
    char *start = s;
    char *end;

    while (*start && ax_isspace(*start)) {
        start++;
    }
    end = start + ax_strlen(start);
    while (end > start && ax_isspace(end[-1])) {
        end--;
    }
    *end = 0;
    return start;
}

char *ax_trim_end(char *s)
{
    char *end = s + ax_strlen(s);
    while (end > s && ax_isspace(end[-1])) {
        end--;
    }
    *end = 0;
    return s;
}

/* Split s in place on sep; returns the number of tokens stored.  Empty tokens
 * are dropped so that "a,,b" yields two entries rather than three. */
usize ax_tokenize(char *s, char sep, char **out, usize max_tokens)
{
    usize count = 0;
    char *p = s;

    while (p && *p && count < max_tokens) {
        while (*p == sep) {
            *p++ = 0;
        }
        if (!*p) {
            break;
        }
        out[count++] = p;
        while (*p && *p != sep) {
            p++;
        }
        if (*p == sep) {
            *p++ = 0;
        }
    }
    return count;
}

void ax_str_reverse(char *s)
{
    usize len = ax_strlen(s);

    for (usize i = 0; i < len / 2; i++) {
        char t = s[i];
        s[i] = s[len - 1 - i];
        s[len - 1 - i] = t;
    }
}

char *ax_str_lower(char *s)
{
    for (char *p = s; *p; p++) {
        *p = ax_tolower(*p);
    }
    return s;
}

char *ax_str_upper(char *s)
{
    for (char *p = s; *p; p++) {
        *p = ax_toupper(*p);
    }
    return s;
}

void ax_str_join(char *dst, usize cap, const char *const *parts, usize count,
                 const char *sep)
{
    usize used = 0;

    if (cap == 0) {
        return;
    }
    dst[0] = 0;
    for (usize i = 0; i < count; i++) {
        if (i && sep) {
            usize n = ax_strlcpy(dst + used, sep, cap - used);
            used += n;
            if (used >= cap) {
                return;
            }
        }
        usize n = ax_strlcpy(dst + used, parts[i] ? parts[i] : "", cap - used);
        used += n;
        if (used >= cap) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Number conversion                                                   */
/* ------------------------------------------------------------------ */

i64 ax_strtoll(const char *s, char **end, int base)
{
    const char *p = s;
    u64 value = 0;
    bool_t negative = false;
    bool_t overflow = false;

    while (ax_isspace(*p)) {
        p++;
    }
    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }
    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
            p++;
        } else {
            base = 10;
        }
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    for (;;) {
        int digit = ax_digit_value(*p);
        if (digit < 0 || digit >= base) {
            break;
        }
        if (value > (~0ULL - (u64)digit) / (u64)base) {
            overflow = true;
        }
        value = value * (u64)base + (u64)digit;
        p++;
    }
    if (end) {
        *end = (char *)p;
    }
    if (overflow) {
        return negative ? (-0x7FFFFFFFFFFFFFFFLL - 1) : 0x7FFFFFFFFFFFFFFFLL;
    }
    return negative ? -(i64)value : (i64)value;
}

u64 ax_strtoull(const char *s, char **end, int base)
{
    return (u64)ax_strtoll(s, end, base);
}

i64 ax_strtol(const char *s, char **end, int base)
{
    return ax_strtoll(s, end, base);
}

i64 ax_atoi(const char *s)
{
    return ax_strtoll(s, NULL_PTR, 10);
}

u64 ax_atou(const char *s)
{
    return (u64)ax_strtoll(s, NULL_PTR, 10);
}

/* "512", "64K", "16M", "2G" and "1T" with optional trailing 'B'. */
int ax_parse_size(const char *s, u64 *out)
{
    char *end = NULL_PTR;
    u64 base;

    if (!s || !*s || !out) {
        return -1;
    }
    base = ax_strtoull(s, &end, 0);
    if (end == s) {
        return -1;
    }
    while (ax_isspace(*end)) {
        end++;
    }
    switch (ax_toupper(*end)) {
    case 0:
        break;
    case 'B':
        end++;
        break;
    case 'K':
        base *= 1024ULL;
        end++;
        break;
    case 'M':
        base *= 1024ULL * 1024ULL;
        end++;
        break;
    case 'G':
        base *= 1024ULL * 1024ULL * 1024ULL;
        end++;
        break;
    case 'T':
        base *= 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        end++;
        break;
    default:
        return -1;
    }
    while (ax_isspace(*end)) {
        end++;
    }
    if (*end == 'B' || *end == 'b') {
        end++;
    }
    if (*end) {
        return -1;
    }
    *out = base;
    return 0;
}

int ax_parse_bool(const char *s, bool_t *out)
{
    if (!s || !out) {
        return -1;
    }
    if (!ax_strcasecmp(s, "1") || !ax_strcasecmp(s, "on") ||
        !ax_strcasecmp(s, "true") || !ax_strcasecmp(s, "yes") ||
        !ax_strcasecmp(s, "enable") || !ax_strcasecmp(s, "enabled")) {
        *out = true;
        return 0;
    }
    if (!ax_strcasecmp(s, "0") || !ax_strcasecmp(s, "off") ||
        !ax_strcasecmp(s, "false") || !ax_strcasecmp(s, "no") ||
        !ax_strcasecmp(s, "disable") || !ax_strcasecmp(s, "disabled")) {
        *out = false;
        return 0;
    }
    return -1;
}

char *ax_utoa(u64 value, char *buf)
{
    char tmp[24];
    usize n = 0;
    char *out = buf;

    if (value == 0) {
        *out++ = '0';
        *out = 0;
        return buf;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n) {
        *out++ = tmp[--n];
    }
    *out = 0;
    return buf;
}

char *ax_itoa(i64 value, char *buf)
{
    if (value < 0) {
        buf[0] = '-';
        ax_utoa((u64)(-(value + 1)) + 1ULL, buf + 1);
        return buf;
    }
    return ax_utoa((u64)value, buf);
}

char *ax_utox(u64 value, char *buf, bool_t prefix)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[20];
    usize n = 0;
    char *out = buf;

    if (prefix) {
        *out++ = '0';
        *out++ = 'x';
    }
    if (value == 0) {
        *out++ = '0';
        *out = 0;
        return buf;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = digits[value & 0xF];
        value >>= 4;
    }
    while (n) {
        *out++ = tmp[--n];
    }
    *out = 0;
    return buf;
}

char *ax_utoa_padded(u64 value, char *buf, u32 width, u8 pad)
{
    char tmp[24];
    usize n = 0;

    do {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value);

    while (n < width) {
        tmp[n++] = (char)pad;
    }
    usize i = 0;
    while (n) {
        buf[i++] = tmp[--n];
    }
    buf[i] = 0;
    return buf;
}

/* Small non-cryptographic string hash used by the symbol table and the
 * filesystem name cache. */
u32 ax_str_to_hash(const char *s)
{
    u32 h = 2166136261u;
    while (*s) {
        h ^= (u32)(u8)*s++;
        h *= 16777619u;
    }
    return h;
}
