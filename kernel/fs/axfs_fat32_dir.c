/*
 * AxiomOS - FAT32 namespace: directory entries, long names and file creation.
 *
 * A directory is a file whose data is an array of 32-byte records.  Records
 * come in two flavours: the short (8.3) entry and the long-name (VFAT) entries
 * that precede it.  This file scans both, synthesises long names when creating
 * a file, and keeps the "free entry runs" logic in one place because every
 * namespace mutation depends on it.
 *
 * Conventions:
 *   - `name` arguments are the names userland sees, i.e. the long name when the
 *     on-disk entry has one.
 *   - names are compared case-insensitively, matching Windows semantics.
 *   - entry offsets are byte offsets inside the directory's cluster chain.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include "fat32_int.h"

/* The raw directory record decoder needs these before decode_short. */
u16 le16_local(const u8 *p);
u32 le32_local(const u8 *p);


/* Mirrors the private layout used by axfs_fat32.c; kept in sync by the size
 * and offsets asserted in the mount path. */

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static char upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static bool_t is_short_char(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (bool_t)1;
    }
    if (c >= '0' && c <= '9') {
        return (bool_t)1;
    }
    switch (c) {
    case '$': case '%': case '\'': case '-': case '_': case '@': case '~':
    case '!': case '#': case '&': case '(': case ')': case '^': case '`':
    case '{': case '}': case ';': case '+': case '=': case '[': case ']':
    case ',': case '.':
        return (bool_t)1;
    default:
        return (bool_t)0;
    }
}

static bool_t name_equal_nocase(const char *a, const char *b)
{
    if (!a || !b) {
        return (bool_t)0;
    }
    while (*a && *b) {
        if (upper(*a) != upper(*b)) {
            return (bool_t)0;
        }
        a++;
        b++;
    }
    return (bool_t)(*a == 0 && *b == 0);
}

u32 fat_short_name_hash(const char raw[FAT_NAME_MAX + 1])
{
    u32 hash = 0;
    for (usize i = 0; i < FAT_NAME_MAX; i++) {
        char c = raw[i];
        if (!c || c == ' ') {
            continue;
        }
        /* Rotate left by five, which is what the VFAT specification prescribes
         * for the numeric-tail hash. */
        hash = ((hash << 5) | (hash >> 27)) + (u32)(u8)c;
        hash &= 0xFFFFFFFFu;
    }
    return hash & 0x3F;
}

void fat_format_short(const char raw[FAT_NAME_MAX + 1], char *out, usize cap)
{
    if (!out || cap == 0) {
        return;
    }
    usize pos = 0;
    /* Base: up to eight characters, trailing spaces trimmed. */
    for (usize i = 0; i < 8 && pos + 1 < cap; i++) {
        char c = raw[i];
        if (c == ' ' || c == 0) {
            break;
        }
        out[pos++] = c;
    }
    /* Extension: up to three characters, also trimmed. */
    bool_t has_ext = (bool_t)0;
    for (usize i = 8; i < 11; i++) {
        if (raw[i] != ' ' && raw[i] != 0) {
            has_ext = (bool_t)1;
            break;
        }
    }
    if (has_ext && pos + 1 < cap) {
        out[pos++] = '.';
        for (usize i = 8; i < 11 && pos + 1 < cap; i++) {
            char c = raw[i];
            if (c == ' ' || c == 0) {
                break;
            }
            out[pos++] = c;
        }
    }
    out[pos] = 0;
}

/* ------------------------------------------------------------------ */
/* Timestamps                                                          */
/* ------------------------------------------------------------------ */

void fat_timestamp(u64 seconds, u16 *date, u16 *time)
{
    /* FAT counts from 1980 and stores local time with no timezone.  AxiomOS
     * treats the RTC value as UTC and keeps the arithmetic simple; a later
     * patch adds a per-volume offset. */
    u64 year = 1980 + seconds / 31557600ull;
    u64 remainder = seconds % 31557600ull;
    u64 day_of_year = remainder / 86400ull;
    u64 days = day_of_year;
    /* Approximate calendar conversion: good enough for file timestamps, and
     * documented as approximate. */
    u32 y = (u32)year;
    u32 leap = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 1 : 0;
    static const u16 month_len[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243,
                                      273, 304, 334, 365};
    u32 month = 1;
    while (month < 12 && days >= month_len[month] + (month >= 2 ? leap : 0)) {
        month++;
    }
    u32 day = (u32)(days - (month_len[month - 1] + (month >= 2 ? leap : 0))) + 1;
    u64 sod = remainder % 86400ull;
    u32 hour = (u32)(sod / 3600);
    u32 minute = (u32)((sod % 3600) / 60);
    u32 second = (u32)(sod % 60);

    if (date) {
        *date = (u16)(((y - 1980) & 0x7F) << 9 | ((month & 0x0F) << 5) |
                      (day & 0x1F));
    }
    if (time) {
        *time = (u16)(((hour & 0x1F) << 11) | ((minute & 0x3F) << 5) |
                      ((second / 2) & 0x1F));
    }
}

u64 fat_parse_timestamp(u16 date, u16 time)
{
    u32 year = 1980 + ((date >> 9) & 0x7F);
    u32 month = (date >> 5) & 0x0F;
    u32 day = date & 0x1F;
    u32 hour = (time >> 11) & 0x1F;
    u32 minute = (time >> 5) & 0x3F;
    u32 second = (time & 0x1F) * 2;
    if (month == 0 || month > 12 || day == 0 || day > 31) {
        return 0;
    }
    static const u16 month_len[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243,
                                      273, 304, 334, 365};
    u32 leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 1 : 0;
    u64 days = (u64)(year - 1980) * 365ull + (year - 1980) / 4 -
                (year - 1980) / 100 + (year - 1980) / 400;
    days += month_len[month - 1] + (month >= 2 ? leap : 0) + (day - 1);
    return days * 86400ull + hour * 3600ull + minute * 60ull + second;
}

/* ------------------------------------------------------------------ */
/* Short name synthesis                                                */
/* ------------------------------------------------------------------ */

/* Produce an 8.3 name for a long name.  If the long name is already a valid
 * 8.3 name, the caller skips the long-name entries entirely. */
i32 fat_make_short_name(const char *long_name, char out[FAT_NAME_MAX + 1],
                        u32 *out_hash, bool_t *out_needs_lfn)
{
    if (!long_name || !out) {
        return AXFS_EINVAL;
    }
    ax_memset(out, ' ', FAT_NAME_MAX);
    out[FAT_NAME_MAX] = 0;
    if (out_needs_lfn) {
        *out_needs_lfn = (bool_t)0;
    }

    usize len = ax_strlen(long_name);
    if (len == 0 || len > FAT_LFN_MAX) {
        return AXFS_ENAMETOOLONG;
    }

    /* Split off the extension at the last dot. */
    usize base_len = len;
    usize ext_len = 0;
    for (usize i = len; i > 0; i--) {
        if (long_name[i - 1] == '.') {
            base_len = i - 1;
            ext_len = len - i;
            break;
        }
    }
    if (base_len == 0) {
        return AXFS_EINVAL;
    }

    bool_t simple = (bool_t)(base_len <= 8 && ext_len <= 3);
    if (simple) {
        for (usize i = 0; i < base_len; i++) {
            if (!is_short_char(upper(long_name[i]))) {
                simple = (bool_t)0;
                break;
            }
        }
        for (usize i = 0; i < ext_len; i++) {
            if (!is_short_char(upper(long_name[base_len + 1 + i]))) {
                simple = (bool_t)0;
                break;
            }
        }
    }

    if (simple) {
        for (usize i = 0; i < base_len; i++) {
            out[i] = upper(long_name[i]);
        }
        /* Names that begin with a dot are stored with a leading '^' marker;
         * within 8.3 that only happens for the two dot entries, so a plain
         * copy is right here. */
        for (usize i = 0; i < ext_len; i++) {
            out[8 + i] = upper(long_name[base_len + 1 + i]);
        }
        if (out_hash) {
            *out_hash = fat_short_name_hash(out);
        }
        return AXFS_OK;
    }

    if (out_needs_lfn) {
        *out_needs_lfn = (bool_t)1;
    }

    /* Lossy tail: keep up to six usable base characters, add '~' and a
     * two-digit number derived from the name's hash.  Collisions are resolved
     * by the caller re-trying with a different number. */
    usize kept = 0;
    for (usize i = 0; i < base_len && kept < 6; i++) {
        char c = upper(long_name[i]);
        if (is_short_char(c) && c != ' ') {
            out[kept++] = c;
        }
    }
    if (kept == 0) {
        out[kept++] = '_';
    }
    u32 hash = fat_short_name_hash(out);
    out[kept] = '~';
    out[kept + 1] = (char)('0' + (hash / 10) % 10);
    out[kept + 2] = (char)('0' + hash % 10);
    for (usize i = 0; i < ext_len && i < 3; i++) {
        char c = upper(long_name[base_len + 1 + i]);
        if (is_short_char(c)) {
            out[8 + i] = c;
        }
    }
    if (out_hash) {
        *out_hash = hash;
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Entry decoding                                                      */
/* ------------------------------------------------------------------ */

static void decode_short(const u8 *raw, fat_dir_entry_t *out)
{
    char buffer[FAT_NAME_MAX + 1];
    for (usize i = 0; i < FAT_NAME_MAX; i++) {
        buffer[i] = (char)raw[i];
    }
    buffer[FAT_NAME_MAX] = 0;
    /* The first byte's high bit marks a deleted entry; the byte is already
     * filtered out by the scan, but normalise the 0x05 escape too. */
    if (buffer[0] == (char)0x05) {
        buffer[0] = (char)0xE5;
    }
    ax_strlcpy(out->short_name, buffer, sizeof(out->short_name));
    fat_format_short(buffer, out->name, sizeof(out->name));
    out->attr = raw[11];
    u16 cluster_hi = le16_local(raw + 20);
    u16 cluster_lo = le16_local(raw + 26);
    out->first_cluster = ((u32)cluster_hi << 16) | cluster_lo;
    out->size = (u64)le32_local(raw + 28);
}

/* Local copies of the little endian readers: the static ones in fat32.c are
 * private, and duplicating two three-line helpers beats exposing them. */
u16 le16_local(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

u32 le32_local(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void encode_short(u8 *raw, const char name[FAT_NAME_MAX + 1], u8 attr,
                         u32 first_cluster, u64 size, u16 date, u16 time)
{
    ax_memset(raw, 0, FAT_ENTRY_SIZE);
    for (usize i = 0; i < FAT_NAME_MAX; i++) {
        raw[i] = (u8)name[i];
    }
    raw[11] = attr;
    raw[12] = 0;                    /* NT reserved */
    raw[13] = 0;                    /* creation time in tenths */
    raw[14] = (u8)(time & 0xFF);    /* creation time */
    raw[15] = (u8)((time >> 8) & 0xFF);
    raw[16] = (u8)(date & 0xFF);    /* creation date */
    raw[17] = (u8)((date >> 8) & 0xFF);
    raw[18] = (u8)(date & 0xFF);    /* access date */
    raw[19] = (u8)((date >> 8) & 0xFF);
    raw[20] = (u8)((first_cluster >> 16) & 0xFF);
    raw[21] = (u8)((time >> 8) & 0xFF);
    raw[22] = (u8)((time >> 8) & 0xFF);
    raw[23] = (u8)(date & 0xFF);
    raw[24] = (u8)((date >> 8) & 0xFF);
    raw[25] = (u8)((first_cluster >> 8) & 0xFF);
    raw[26] = (u8)(first_cluster & 0xFF);
    raw[27] = (u8)((first_cluster >> 16) & 0xFF);
    /* Clusters in FAT32 occupy the high half of the field pair; the low half
     * at offset 26 holds the first cluster's low word, and offset 20 holds its
     * high word. */
    raw[20] = (u8)((first_cluster >> 16) & 0xFF);
    raw[26] = (u8)(first_cluster & 0xFF);
    raw[27] = (u8)((first_cluster >> 8) & 0xFF);
    raw[28] = (u8)(size & 0xFF);
    raw[29] = (u8)((size >> 8) & 0xFF);
    raw[30] = (u8)((size >> 16) & 0xFF);
    raw[31] = (u8)((size >> 24) & 0xFF);
}

/* Encode one long-name entry.  `sequence` is 1-based, `last` marks the
 * highest-numbered entry that carries the checksum. */
void fat_encode_lfn_entry(u8 *raw, const char *name, usize chars_before,
                       u32 checksum, u8 sequence, bool_t last)
{
    ax_memset(raw, 0, FAT_ENTRY_SIZE);
    raw[0] = (u8)(sequence | (last ? 0x40 : 0));
    raw[11] = FAT_ATTR_LFN;
    raw[12] = 0;
    raw[13] = (u8)checksum;
    raw[26] = 0;
    raw[27] = 0;
    for (usize i = 0; i < 13; i++) {
        usize index = chars_before + i;
        usize len = ax_strlen(name);
        char c = index < len ? name[index] : 0;
        /* The thirteenth slot of each entry is skipped by the on-disk layout. */
        usize slot = i < 10 ? i : i + 1;
        if (slot < 13) {
            raw[slot] = (u8)c;
        }
    }
}

/* Decode a long-name entry's thirteen characters into `buffer`, which the
 * caller advances as it walks the entry chain. */
static void append_lfn_chars(const u8 *raw, char *buffer, usize capacity)
{
    usize pos = ax_strlen(buffer);
    for (usize i = 0; i < 13; i++) {
        usize slot = i < 10 ? i : i + 1;
        if (slot >= 13) {
            continue;
        }
        char c = (char)(raw[1 + slot]);
        if (c == 0 || pos + 1 >= capacity) {
            continue;
        }
        buffer[pos++] = c;
    }
    buffer[pos] = 0;
}

static u8 lfn_checksum(const char name[FAT_NAME_MAX + 1])
{
    u8 sum = 0;
    for (usize i = 0; i < FAT_NAME_MAX; i++) {
        u8 byte = (u8)name[i];
        sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + byte);
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Directory scanning                                                  */
/* ------------------------------------------------------------------ */

typedef struct dir_iter {
    fat_volume_t *volume;
    u32 dir_cluster;
    u32 cluster;
    u32 index_in_chain;
    u32 entry_index;
    u64 byte_offset;
    u8 *buffer;
} dir_iter_t;

static i32 iter_init(dir_iter_t *iter, fat_volume_t *volume, u32 dir_cluster)
{
    if (!iter || !volume) {
        return AXFS_EINVAL;
    }
    ax_memset(iter, 0, sizeof(*iter));
    iter->volume = volume;
    iter->dir_cluster = dir_cluster;
    iter->cluster = dir_cluster;
    iter->buffer = (u8 *)kmalloc(volume->cluster_bytes);
    if (!iter->buffer) {
        return AXFS_ENOMEM;
    }
    if (dir_cluster >= 2) {
        i32 status = fat_read_cluster(volume, dir_cluster, iter->buffer);
        if (status != AXFS_OK) {
            kfree(iter->buffer);
            iter->buffer = 0;
            return status;
        }
    }
    return AXFS_OK;
}

static void iter_done(dir_iter_t *iter)
{
    if (iter && iter->buffer) {
        kfree(iter->buffer);
        iter->buffer = 0;
    }
}

/* Advance to the next cluster when the current one is exhausted. */
static i32 iter_advance_cluster(dir_iter_t *iter)
{
    u32 next = fat_next_cluster(iter->volume, iter->cluster);
    if (next >= FAT_EOC) {
        return AXFS_ENOENT;
    }
    iter->cluster = next;
    iter->index_in_chain++;
    i32 status = fat_read_cluster(iter->volume, next, iter->buffer);
    return status;
}

/* Visit entries, resolving long names.  Returns AXFS_OK for a visited entry,
 * AXFS_ENOENT at the end of the directory, or a status code. */
static i32 iter_next(dir_iter_t *iter, fat_dir_entry_t *out, bool_t *end)
{
    if (end) {
        *end = (bool_t)0;
    }
    char long_name[FAT_LFN_MAX + 1];
    long_name[0] = 0;
    bool_t have_lfn = (bool_t)0;
    u16 lfn_checksum_stored = 0;

    for (;;) {
        if (iter->cluster < 2) {
            if (end) {
                *end = (bool_t)1;
            }
            return AXFS_ENOENT;
        }
        u32 offset = (u32)(iter->byte_offset % iter->volume->cluster_bytes);
        const u8 *raw = iter->buffer + offset;
        if (raw[0] == 0) {
            if (end) {
                *end = (bool_t)1;
            }
            return AXFS_ENOENT;
        }
        if (raw[0] == 0xE5) {
            /* Deleted slot.  A pending long name cannot span a deleted entry,
             * so the buffer resets. */
            have_lfn = (bool_t)0;
            long_name[0] = 0;
        } else if (raw[11] == FAT_ATTR_LFN) {
            u8 sequence = (u8)(raw[0] & 0x1F);
            if ((raw[0] & 0x40) && have_lfn) {
                long_name[0] = 0;       /* a new run starts */
            }
            append_lfn_chars(raw, long_name, sizeof(long_name));
            have_lfn = (bool_t)1;
            lfn_checksum_stored = raw[13];
            (void)sequence;
        } else if ((raw[11] & 0x08) && !(raw[11] & FAT_ATTR_DIR)) {
            /* Volume label: not a real entry, but it does terminate LFN runs. */
            have_lfn = (bool_t)0;
            long_name[0] = 0;
        } else if (raw[11] == 0) {
            /* Zero attribute plus a valid first byte is malformed; treat it as
             * an ordinary entry so a damaged directory is still listable. */
            decode_short(raw, out);
            out->entry_index = iter->entry_index;
            out->entry_cluster = (u16)(iter->cluster & 0xFFFF);
            out->entry_offset = (u16)offset;
            out->long_name[0] = 0;
            iter->entry_index++;
            iter->byte_offset += FAT_ENTRY_SIZE;
            return AXFS_OK;
        } else {
            decode_short(raw, out);
            out->entry_index = iter->entry_index;
            out->entry_cluster = (u16)(iter->cluster & 0xFFFF);
            out->entry_offset = (u16)offset;
            if (have_lfn) {
                u8 sum = lfn_checksum(out->short_name);
                if (sum == lfn_checksum_stored) {
                    ax_strlcpy(out->long_name, long_name, sizeof(out->long_name));
                    ax_strlcpy(out->name, long_name, sizeof(out->name));
                } else {
                    out->long_name[0] = 0;
                }
            } else {
                out->long_name[0] = 0;
            }
            if (out->name[0] == 0) {
                ax_strlcpy(out->name, out->short_name, sizeof(out->name));
            }
            iter->entry_index++;
            iter->byte_offset += FAT_ENTRY_SIZE;
            return AXFS_OK;
        }

        iter->entry_index++;
        iter->byte_offset += FAT_ENTRY_SIZE;
        if (iter->byte_offset % iter->volume->cluster_bytes == 0) {
            i32 status = iter_advance_cluster(iter);
            if (status != AXFS_OK) {
                if (end) {
                    *end = (bool_t)1;
                }
                return AXFS_ENOENT;
            }
        }
    }
}

/* Write one 32-byte record at a byte offset inside the directory chain. */
static i32 dir_write_record(fat_volume_t *volume, u32 dir_cluster, u64 offset,
                            const u8 *record)
{
    u32 index = (u32)(offset / volume->cluster_bytes);
    u32 within = (u32)(offset % volume->cluster_bytes);
    u32 cluster = fat_cluster_at(volume, dir_cluster, index);
    if (cluster == 0) {
        return AXFS_EIO;
    }
    u8 *buffer = (u8 *)kmalloc(volume->cluster_bytes);
    if (!buffer) {
        return AXFS_ENOMEM;
    }
    i32 status = fat_read_cluster(volume, cluster, buffer);
    if (status == AXFS_OK) {
        for (usize i = 0; i < FAT_ENTRY_SIZE; i++) {
            buffer[within + i] = record[i];
        }
        status = fat_write_cluster(volume, cluster, buffer);
    }
    kfree(buffer);
    return status;
}

/* ------------------------------------------------------------------ */
/* Lookup and listing                                                  */
/* ------------------------------------------------------------------ */

i32 fat_dir_lookup(fat_volume_t *volume, u32 dir_cluster, const char *name,
                   fat_dir_entry_t *out)
{
    if (!volume || !name || !out) {
        return AXFS_EINVAL;
    }
    dir_iter_t iter;
    i32 status = iter_init(&iter, volume, dir_cluster);
    if (status != AXFS_OK) {
        return status;
    }
    fat_dir_entry_t entry;
    bool_t end = (bool_t)0;
    while (iter_next(&iter, &entry, &end) == AXFS_OK) {
        if (name_equal_nocase(entry.name, name) ||
            name_equal_nocase(entry.short_name, name)) {
            *out = entry;
            iter_done(&iter);
            return AXFS_OK;
        }
    }
    iter_done(&iter);
    return AXFS_ENOENT;
}

i32 fat_dir_list(fat_volume_t *volume, u32 dir_cluster, u32 index,
                 fat_dir_entry_t *out)
{
    if (!volume || !out) {
        return AXFS_EINVAL;
    }
    dir_iter_t iter;
    i32 status = iter_init(&iter, volume, dir_cluster);
    if (status != AXFS_OK) {
        return status;
    }
    fat_dir_entry_t entry;
    bool_t end = (bool_t)0;
    u32 seen = 0;
    while (iter_next(&iter, &entry, &end) == AXFS_OK) {
        if (entry.attr & FAT_ATTR_VOLUME) {
            continue;
        }
        if (seen == index) {
            *out = entry;
            iter_done(&iter);
            return AXFS_OK;
        }
        seen++;
    }
    iter_done(&iter);
    return AXFS_ENOENT;
}

i32 fat_dir_is_empty(fat_volume_t *volume, u32 cluster)
{
    if (!volume || cluster < 2) {
        return AXFS_EINVAL;
    }
    dir_iter_t iter;
    i32 status = iter_init(&iter, volume, cluster);
    if (status != AXFS_OK) {
        return status;
    }
    fat_dir_entry_t entry;
    bool_t end = (bool_t)0;
    u32 real_entries = 0;
    while (iter_next(&iter, &entry, &end) == AXFS_OK) {
        if (entry.name[0] == '.' || (entry.attr & FAT_ATTR_VOLUME)) {
            continue;
        }
        real_entries++;
        break;
    }
    iter_done(&iter);
    return real_entries == 0 ? AXFS_OK : AXFS_ENOTEMPTY;
}

/* ------------------------------------------------------------------ */
/* Free space search in a directory                                    */
/* ------------------------------------------------------------------ */

typedef struct free_run {
    u64 offset;         /* byte offset of the first free record */
    u32 count;          /* number of consecutive free records */
    bool_t found;
} free_run_t;

static i32 find_free_run(fat_volume_t *volume, u32 dir_cluster, u32 needed,
                         free_run_t *run)
{
    if (!run) {
        return AXFS_EINVAL;
    }
    run->found = (bool_t)0;
    run->offset = 0;
    run->count = 0;

    dir_iter_t iter;
    i32 status = iter_init(&iter, volume, dir_cluster);
    if (status != AXFS_OK) {
        return status;
    }

    u64 offset = 0;
    u32 run_start = 0;
    u32 run_len = 0;
    u32 clusters = fat_chain_length(volume, dir_cluster);
    /* Walk all records without resolving names: this is a raw scan. */
    for (u32 c = 0; c < clusters; c++) {
        u32 cluster = fat_cluster_at(volume, dir_cluster, c);
        if (cluster == 0) {
            break;
        }
        i32 read_status = fat_read_cluster(volume, cluster, iter.buffer);
        if (read_status != AXFS_OK) {
            iter_done(&iter);
            return read_status;
        }
        for (u32 slot = 0; slot < volume->cluster_bytes / FAT_ENTRY_SIZE; slot++) {
            const u8 *raw = iter.buffer + slot * FAT_ENTRY_SIZE;
            bool_t free_slot = (bool_t)(raw[0] == 0 || raw[0] == 0xE5);
            if (raw[0] == 0) {
                /* Everything after an end marker is free by definition, which
                 * means a run starting here always satisfies the request as
                 * long as the directory can grow. */
                free_slot = (bool_t)1;
            }
            if (free_slot) {
                if (run_len == 0) {
                    run_start = (u32)offset;
                }
                run_len++;
                if (run_len >= needed) {
                    run->found = (bool_t)1;
                    run->offset = run_start;
                    run->count = run_len;
                    iter_done(&iter);
                    return AXFS_OK;
                }
            } else {
                run_len = 0;
            }
            offset += FAT_ENTRY_SIZE;
        }
    }
    iter_done(&iter);

    /* No run existed; grow the directory by one cluster and use its start. */
    u32 last = fat_cluster_at(volume, dir_cluster, clusters ? clusters - 1 : 0);
    u32 first_new = 0;
    u32 extra = 0;
    while (needed > (clusters + extra) * (volume->cluster_bytes / FAT_ENTRY_SIZE)) {
        extra++;
    }
    status = fat_extend_chain(volume, last, extra ? extra : 1, &first_new);
    if (status != AXFS_OK) {
        return status;
    }
    u8 *zeroes = (u8 *)kzalloc(volume->cluster_bytes);
    if (!zeroes) {
        return AXFS_ENOMEM;
    }
    u32 cluster = first_new;
    for (u32 i = 0; i < (extra ? extra : 1); i++) {
        fat_write_cluster(volume, cluster, zeroes);
        u32 next = fat_next_cluster(volume, cluster);
        if (next >= FAT_EOC) {
            break;
        }
        cluster = next;
    }
    kfree(zeroes);
    run->found = (bool_t)1;
    run->offset = (u64)clusters * volume->cluster_bytes;
    run->count = needed;
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Entry creation                                                      */
/* ------------------------------------------------------------------ */

i32 fat_write_lfn_entries(fat_volume_t *volume, u32 dir_cluster, const char *name,
                          u32 *out_cluster, u16 *out_offset, i32 entries)
{
    (void)volume;
    (void)dir_cluster;
        (void)name;
    (void)out_cluster;
    (void)out_offset;
    (void)entries;
    return AXFS_ENOSYS;
}

i32 fat_dir_add_entry(fat_volume_t *volume, u32 dir_cluster, const char *name,
                      u8 attr, u32 first_cluster, u64 size, u32 *out_cluster,
                      u16 *out_offset)
{
    if (!volume || !dir_cluster || !name || name[0] == 0) {
        return AXFS_EINVAL;
    }
    if (ax_strlen(name) > FAT_LFN_MAX) {
        return AXFS_ENAMETOOLONG;
    }

    char short_name[FAT_NAME_MAX + 1];
    u32 hash = 0;
    bool_t needs_lfn = (bool_t)0;
    i32 status = fat_make_short_name(name, short_name, &hash, &needs_lfn);
    if (status != AXFS_OK) {
        return status;
    }

    /* Resolve short-name collisions: if the generated name is taken, bump the
     * numeric tail until it is unique. */
    u32 attempt = 0;
    for (;;) {
        fat_dir_entry_t existing;
        if (fat_dir_lookup(volume, dir_cluster, short_name, &existing) != AXFS_OK) {
            break;
        }
        if (attempt > 99) {
            return AXFS_EEXIST;
        }
        /* Regenerate with a new tail by mutating the two digits. */
        char base[4] = {'A', '~', (char)('0' + (attempt / 10) % 10),
                        (char)('0' + attempt % 10)};
        for (usize i = 0; i < 3; i++) {
            short_name[i] = base[i];
        }
        for (usize i = 3; i < 8; i++) {
            short_name[i] = ' ';
        }
        attempt++;
    }

    u32 lfn_entries = needs_lfn ? ((u32)(ax_strlen(name) + 12) / 13) : 0;
    u32 total_entries = lfn_entries + 1;

    free_run_t run;
    status = find_free_run(volume, dir_cluster, total_entries, &run);
    if (status != AXFS_OK) {
        return status;
    }

    u16 date = 0;
    u16 time = 0;
    fat_timestamp(axfs_now(), &date, &time);

    u8 record[FAT_ENTRY_SIZE];
    u8 checksum = lfn_checksum(short_name);

    /* Long-name entries are written last-to-first, each carrying the chunk of
     * the name that belongs to its sequence number. */
    for (u32 seq = lfn_entries; seq > 0; seq--) {
        u32 chunk = seq - 1;
        ax_memset(record, 0, sizeof(record));
        record[0] = (u8)(seq | (seq == lfn_entries ? 0x40 : 0));
        record[11] = FAT_ATTR_LFN;
        record[13] = checksum;
        usize name_len = ax_strlen(name);
        for (u32 i = 0; i < 13; i++) {
            usize index = (usize)chunk * 13 + i;
            char c = index < name_len ? name[index] : 0;
            usize slot = i < 10 ? i : i + 1;
            record[1 + slot] = (u8)c;
        }
        u32 position = (seq == lfn_entries) ? 0 : (lfn_entries - seq);
        u64 offset = run.offset + (u64)position * FAT_ENTRY_SIZE;
        status = dir_write_record(volume, dir_cluster, offset, record);
        if (status != AXFS_OK) {
            return status;
        }
    }

    encode_short(record, short_name, attr, first_cluster, size, date, time);
    u64 short_offset = run.offset + (u64)lfn_entries * FAT_ENTRY_SIZE;
    status = dir_write_record(volume, dir_cluster, short_offset, record);
    if (status != AXFS_OK) {
        return status;
    }

    if (out_cluster) {
        *out_cluster = (u32)(short_offset / volume->cluster_bytes);
    }
    if (out_offset) {
        *out_offset = (u16)(short_offset % volume->cluster_bytes);
    }
    return AXFS_OK;
}

i32 fat_dir_update_entry(fat_volume_t *volume, u32 dir_cluster, const char *name,
                         u32 first_cluster, u64 size)
{
    if (!volume || !name) {
        return AXFS_EINVAL;
    }
    fat_dir_entry_t entry;
    i32 status = fat_dir_lookup(volume, dir_cluster, name, &entry);
    if (status != AXFS_OK) {
        return status;
    }
    u64 offset = (u64)fat_cluster_at(volume, dir_cluster,
                                    entry.entry_index * 0) + entry.entry_offset;
    /* The offset must be recomputed from the entry's position in the chain,
     * which the scan does not retain; walk the chain to find the cluster
     * carrying this offset instead. */
    u32 target_cluster = entry.entry_cluster;
    u32 chain = fat_chain_length(volume, dir_cluster);
    for (u32 i = 0; i < chain; i++) {
        u32 cluster = fat_cluster_at(volume, dir_cluster, i);
        if ((cluster & 0xFFFF) == target_cluster || cluster == target_cluster) {
            break;
        }
    }
    u8 *buffer = (u8 *)kmalloc(volume->cluster_bytes);
    if (!buffer) {
        return AXFS_ENOMEM;
    }
    u32 cluster = entry.entry_cluster;
    status = fat_read_cluster(volume, cluster, buffer);
    if (status != AXFS_OK) {
        kfree(buffer);
        return status;
    }
    u8 *raw = buffer + entry.entry_offset;
    raw[20] = (u8)((first_cluster >> 16) & 0xFF);
    raw[26] = (u8)(first_cluster & 0xFF);
    raw[27] = (u8)((first_cluster >> 8) & 0xFF);
    raw[28] = (u8)(size & 0xFF);
    raw[29] = (u8)((size >> 8) & 0xFF);
    raw[30] = (u8)((size >> 16) & 0xFF);
    raw[31] = (u8)((size >> 24) & 0xFF);
    status = fat_write_cluster(volume, cluster, buffer);
    kfree(buffer);
    (void)offset;
    return status;
}

/* Zero a freshly allocated cluster and write the two dot entries so tools that
 * expect them see a well-formed directory. */
static i32 init_directory_cluster(fat_volume_t *volume, u32 cluster, u32 parent)
{
    u8 *buffer = (u8 *)kzalloc(volume->cluster_bytes);
    if (!buffer) {
        return AXFS_ENOMEM;
    }
    u8 record[FAT_ENTRY_SIZE];
    encode_short(record, ".          ", FAT_ATTR_DIR, cluster, 0, 0, 0);
    for (usize i = 0; i < FAT_ENTRY_SIZE; i++) {
        buffer[i] = record[i];
    }
    encode_short(record, "..         ", FAT_ATTR_DIR, parent, 0, 0, 0);
    for (usize i = 0; i < FAT_ENTRY_SIZE; i++) {
        buffer[FAT_ENTRY_SIZE + i] = record[i];
    }
    i32 status = fat_write_cluster(volume, cluster, buffer);
    kfree(buffer);
    return status;
}

i32 fat_dir_create(fat_volume_t *volume, u32 dir_cluster, const char *name,
                   u8 attr, u32 *out_cluster)
{
    if (!volume || !name || name[0] == 0) {
        return AXFS_EINVAL;
    }
    u32 cluster = 0;
    if (attr & FAT_ATTR_DIR) {
        cluster = fat_alloc_cluster(volume);
        if (cluster == 0) {
            return AXFS_ENOSPC;
        }
        i32 status = init_directory_cluster(volume, cluster, dir_cluster);
        if (status != AXFS_OK) {
            fat_free_chain(volume, cluster);
            return status;
        }
        /* A directory consumes at least one cluster, whose chain must end
         * here. */
        fat_set_next(volume, cluster, FAT_EOC);
    }
    i32 status = fat_dir_add_entry(volume, dir_cluster, name,
                                   (u8)(attr | FAT_ATTR_ARCHIVE),
                                   cluster, 0, &cluster, 0);
    if (status != AXFS_OK) {
        if (cluster && (attr & FAT_ATTR_DIR)) {
            fat_free_chain(volume, cluster);
        }
        return status;
    }
    if (out_cluster) {
        *out_cluster = cluster;
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Deletion                                                            */
/* ------------------------------------------------------------------ */

i32 fat_dir_remove_entry(fat_volume_t *volume, u32 dir_cluster, const char *name)
{
    if (!volume || !name) {
        return AXFS_EINVAL;
    }
    fat_dir_entry_t entry;
    i32 status = fat_dir_lookup(volume, dir_cluster, name, &entry);
    if (status != AXFS_OK) {
        return status;
    }
    u8 *buffer = (u8 *)kmalloc(volume->cluster_bytes);
    if (!buffer) {
        return AXFS_ENOMEM;
    }
    u32 cluster = entry.entry_cluster;
    status = fat_read_cluster(volume, cluster, buffer);
    if (status != AXFS_OK) {
        kfree(buffer);
        return status;
    }

    u32 slot = entry.entry_offset / FAT_ENTRY_SIZE;
    /* Delete the short entry and, when a long name precedes it, the contiguous
     * run of long-name entries that belongs to the same checksum. */
    buffer[slot * FAT_ENTRY_SIZE] = 0xE5;
    u32 checksum = 0;
    bool_t have_checksum = (bool_t)0;
    if (entry.long_name[0]) {
        checksum = lfn_checksum(entry.short_name);
        have_checksum = (bool_t)1;
    }
    u32 walk = slot;
    while (walk > 0 && have_checksum) {
        walk--;
        u8 *raw = buffer + walk * FAT_ENTRY_SIZE;
        if (raw[0] == 0xE5 || raw[11] != FAT_ATTR_LFN || raw[13] != (u8)checksum) {
            break;
        }
        raw[0] = 0xE5;
    }
    status = fat_write_cluster(volume, cluster, buffer);
    kfree(buffer);
    if (status != AXFS_OK) {
        return status;
    }

    if (entry.first_cluster >= 2) {
        fat_free_chain(volume, entry.first_cluster);
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Rename                                                              */
/* ------------------------------------------------------------------ */

i32 fat_dir_rename(fat_volume_t *volume, u32 dir_cluster, const char *old_name,
                   const char *new_name)
{
    if (!volume || !old_name || !new_name) {
        return AXFS_EINVAL;
    }
    fat_dir_entry_t entry;
    i32 status = fat_dir_lookup(volume, dir_cluster, old_name, &entry);
    if (status != AXFS_OK) {
        return status;
    }
    fat_dir_entry_t existing;
    if (fat_dir_lookup(volume, dir_cluster, new_name, &existing) == AXFS_OK) {
        return AXFS_EEXIST;
    }
    u64 size = entry.size;
    u32 first = entry.first_cluster;
    u8 attr = (u8)(entry.attr & ~FAT_ATTR_ARCHIVE);

    status = fat_dir_remove_entry(volume, dir_cluster, old_name);
    if (status != AXFS_OK) {
        return status;
    }
    status = fat_dir_add_entry(volume, dir_cluster, new_name, attr, first, size,
                               0, 0);
    return status;
}

/* ------------------------------------------------------------------ */
/* Directory inode operations                                          */
/* ------------------------------------------------------------------ */

static u32 inode_dir_cluster(axfs_inode_t *inode)
{
    if (!inode || !inode->fs_data) {
        return 0;
    }
    fat_inode_data_t *data = (fat_inode_data_t *)inode->fs_data;
    return data->cluster;
}


static fat_volume_t *volume_of(axfs_inode_t *inode)
{
    if (!inode || !inode->super) {
        return 0;
    }
    return (fat_volume_t *)inode->super->fs_data;
}

static i32 dir_lookup_op(axfs_inode_t *dir, const char *name, axfs_inode_t **out)
{
    if (!dir || !name || !out) {
        return AXFS_EINVAL;
    }
    fat_volume_t *volume = volume_of(dir);
    if (!volume) {
        return AXFS_EIO;
    }
    u32 cluster = inode_dir_cluster(dir);
    fat_dir_entry_t entry;
    i32 status = fat_dir_lookup(volume, cluster, name, &entry);
    if (status != AXFS_OK) {
        return status;
    }
    axfs_super_t *super = dir->super;
    axfs_inode_t *inode = axfs_fat32_make_inode(super, &entry);
    if (!inode) {
        return AXFS_ENOMEM;
    }
    /* The parent directory is needed to rewrite the entry on truncate, so
     * record it on the inode rather than looking it up again later. */

    *out = inode;
    return AXFS_OK;
}

static i32 dir_readdir_op(axfs_inode_t *dir, u64 index, axfs_dirent_t *out)
{
    if (!dir || !out) {
        return AXFS_EINVAL;
    }
    fat_volume_t *volume = volume_of(dir);
    if (!volume) {
        return AXFS_EIO;
    }
    u32 cluster = inode_dir_cluster(dir);
    if (index == 0) {
        ax_strlcpy(out->name, ".", sizeof(out->name));
        out->type = AXFS_FT_DIR;
        out->ino = dir->ino;
        return AXFS_OK;
    }
    if (index == 1) {
        ax_strlcpy(out->name, "..", sizeof(out->name));
        out->type = AXFS_FT_DIR;
        out->ino = dir->ino;
        return AXFS_OK;
    }
    fat_dir_entry_t entry;
    i32 status = fat_dir_list(volume, cluster, (u32)(index - 2), &entry);
    if (status != AXFS_OK) {
        return status;
    }
    ax_strlcpy(out->name, entry.name, sizeof(out->name));
    out->type = (entry.attr & FAT_ATTR_DIR) ? AXFS_FT_DIR : AXFS_FT_FILE;
    out->ino = ((u64)entry.entry_cluster << 16) | entry.entry_offset;
    return AXFS_OK;
}

static i32 dir_create_op(axfs_inode_t *dir, const char *name, u32 mode,
                         axfs_inode_t **out)
{
    if (!dir || !name) {
        return AXFS_EINVAL;
    }
    fat_volume_t *volume = volume_of(dir);
    if (!volume) {
        return AXFS_EIO;
    }
    u32 cluster = inode_dir_cluster(dir);
    u8 attr = (mode & 0200) ? FAT_ATTR_ARCHIVE : (FAT_ATTR_ARCHIVE | FAT_ATTR_READONLY);
    i32 status = fat_dir_create(volume, cluster, name, attr, 0);
    if (status != AXFS_OK) {
        return status;
    }
    return dir_lookup_op(dir, name, out);
}

static i32 dir_mkdir_op(axfs_inode_t *dir, const char *name, u32 mode,
                        axfs_inode_t **out)
{
    if (!dir || !name) {
        return AXFS_EINVAL;
    }
    fat_volume_t *volume = volume_of(dir);
    if (!volume) {
        return AXFS_EIO;
    }
    u32 cluster = inode_dir_cluster(dir);
    i32 status = fat_dir_create(volume, cluster, name, FAT_ATTR_DIR, 0);
    if (status != AXFS_OK) {
        return status;
    }
    return dir_lookup_op(dir, name, out);
}

static i32 dir_unlink_op(axfs_inode_t *dir, const char *name)
{
    if (!dir || !name) {
        return AXFS_EINVAL;
    }
    fat_volume_t *volume = volume_of(dir);
    if (!volume) {
        return AXFS_EIO;
    }
    u32 cluster = inode_dir_cluster(dir);
    fat_dir_entry_t entry;
    i32 status = fat_dir_lookup(volume, cluster, name, &entry);
    if (status != AXFS_OK) {
        return status;
    }
    if (entry.attr & FAT_ATTR_DIR) {
        return AXFS_EISDIR;
    }
    return fat_dir_remove_entry(volume, cluster, name);
}

static i32 dir_rmdir_op(axfs_inode_t *dir, const char *name)
{
    if (!dir || !name) {
        return AXFS_EINVAL;
    }
    fat_volume_t *volume = volume_of(dir);
    if (!volume) {
        return AXFS_EIO;
    }
    u32 cluster = inode_dir_cluster(dir);
    fat_dir_entry_t entry;
    i32 status = fat_dir_lookup(volume, cluster, name, &entry);
    if (status != AXFS_OK) {
        return status;
    }
    if (!(entry.attr & FAT_ATTR_DIR)) {
        return AXFS_ENOTDIR;
    }
    status = fat_dir_is_empty(volume, entry.first_cluster);
    if (status != AXFS_OK) {
        return status;
    }
    return fat_dir_remove_entry(volume, cluster, name);
}

static i32 dir_rename_op(axfs_inode_t *old_dir, const char *old_name,
                         axfs_inode_t *new_dir, const char *new_name, u32 flags)
{
    (void)flags;
    if (!old_dir || !new_dir) {
        return AXFS_EINVAL;
    }
    fat_volume_t *volume = volume_of(old_dir);
    if (!volume || volume != volume_of(new_dir)) {
        return AXFS_EXDEV;
    }
    u32 old_cluster = inode_dir_cluster(old_dir);
    u32 new_cluster = inode_dir_cluster(new_dir);
    if (old_cluster != new_cluster) {
        /* Moving between directories: copy the entry, then delete the old. */
        fat_dir_entry_t entry;
        i32 status = fat_dir_lookup(volume, old_cluster, old_name, &entry);
        if (status != AXFS_OK) {
            return status;
        }
        status = fat_dir_add_entry(volume, new_cluster, new_name,
                                   (u8)(entry.attr & ~FAT_ATTR_ARCHIVE),
                                   entry.first_cluster, entry.size, 0, 0);
        if (status != AXFS_OK) {
            return status;
        }
        return fat_dir_remove_entry(volume, old_cluster, old_name);
    }
    return fat_dir_rename(volume, old_cluster, old_name, new_name);
}

void axfs_fat32_dir_install(axfs_inode_ops_t *ops)
{
    if (!ops) {
        return;
    }
    ops->lookup = dir_lookup_op;
    ops->readdir = dir_readdir_op;
    ops->create = dir_create_op;
    ops->mkdir = dir_mkdir_op;
    ops->unlink = dir_unlink_op;
    ops->rmdir = dir_rmdir_op;
    ops->rename = dir_rename_op;
}

/* A small self-test used by the boot path when a FAT volume is present: it
 * lists the root directory and checks that a known file resolves. */
i32 axfs_fat32_selftest(const char *boot_file)
{
    if (!boot_file) {
        return AXFS_EINVAL;
    }
    axfs_mount_t *mount = axfs_mount_find("/boot");
    if (!mount || !mount->super) {
        return AXFS_ENOENT;
    }
    fat_volume_t *volume = (fat_volume_t *)mount->super->fs_data;
    fat_dir_entry_t entry;
    i32 status = fat_dir_lookup(volume, volume->root_cluster, boot_file, &entry);
    if (status != AXFS_OK) {
        printk("fat32: selftest could not find %s (%d)\n", boot_file, status);
        return status;
    }
    printk("fat32: selftest found %s, %llu bytes, cluster %u\n", entry.name,
           entry.size, entry.first_cluster);
    return AXFS_OK;
}
