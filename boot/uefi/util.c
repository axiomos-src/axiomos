/* AxiomOS UEFI loader - freestanding utility functions.
 *
 * The loader is compiled with -ffreestanding, so no CRT is available.  These
 * routines are deliberately simple: performance does not matter here, the
 * kernel has its own optimised versions in arch/x86_64/string.asm.
 */

#include "loader.h"

void *u_memset(void *dst, int c, efi_uintn n)
{
    efi_uint8 *d = (efi_uint8 *)dst;
    while (n--)
        *d++ = (efi_uint8)c;
    return dst;
}

void *u_memcpy(void *dst, const void *src, efi_uintn n)
{
    efi_uint8 *d = (efi_uint8 *)dst;
    const efi_uint8 *s = (const efi_uint8 *)src;
    /* Copy in 8-byte chunks when both buffers are aligned; firmware passes
     * unaligned buffers surprisingly often (device paths in particular). */
    if (((efi_uintn)d & 7) == 0 && ((efi_uintn)s & 7) == 0) {
        efi_uint64 *d64 = (efi_uint64 *)d;
        const efi_uint64 *s64 = (const efi_uint64 *)s;
        while (n >= 8) {
            *d64++ = *s64++;
            n -= 8;
        }
        d = (efi_uint8 *)d64;
        s = (const efi_uint8 *)s64;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

int u_memcmp(const void *a, const void *b, efi_uintn n)
{
    const efi_uint8 *pa = (const efi_uint8 *)a;
    const efi_uint8 *pb = (const efi_uint8 *)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

efi_uintn u_strlen(const char *s)
{
    efi_uintn n = 0;
    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
}

int u_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

void u_strcpy(char *dst, const char *src, efi_uintn cap)
{
    efi_uintn i = 0;
    if (!cap)
        return;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

void cpu_relax(void)
{
    __asm__ __volatile__("pause");
}

void halt_forever(void)
{
    for (;;)
        __asm__ __volatile__("cli; hlt");
}

/* ------------------------------------------------------------------ */
/* ASCII <-> UTF-16 conversion used by the file system layer.          */
/* Paths handed to firmware must be UCS-2.  The loader only needs the  */
/* ASCII subset because its own paths are compiled in.                 */
/* ------------------------------------------------------------------ */

efi_uintn ascii_to_ucs2(const char *src, efi_char16 *dst, efi_uintn cap)
{
    efi_uintn i = 0;
    while (src[i] && i + 1 < cap) {
        dst[i] = (efi_char16)src[i];
        i++;
    }
    dst[i] = 0;
    return i;
}

/* ------------------------------------------------------------------ */
/* Simple number formatting helpers                                    */
/* ------------------------------------------------------------------ */

void u_to_dec(u64 value, char *out)
{
    char tmp[24];
    int i = 0, j = 0;
    if (value == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    while (value) {
        tmp[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i)
        out[j++] = tmp[--i];
    out[j] = 0;
}

void u_to_hex(u64 value, char *out, int min_digits)
{
    static const char *digits = "0123456789abcdef";
    int shift = 60;
    int written = 0;
    int started = 0;
    while (shift >= 0) {
        int nibble = (int)((value >> shift) & 0xF);
        int remaining_digits = (shift / 4) + 1;
        if (nibble || started || remaining_digits <= min_digits) {
            *out++ = digits[nibble];
            written++;
            started = 1;
        }
        shift -= 4;
    }
    if (!written)
        *out++ = '0';
    *out = 0;
}
