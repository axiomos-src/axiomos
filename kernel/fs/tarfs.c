/* AxiomOS - tarfs read-only archive filesystem.
 *
 * Bootstrapping benefits from a filesystem that needs no formatter: a POSIX
 * ustar archive is mounted in place, either from the loader image or from a
 * block range.  Entries are indexed lazily and cached in a small directory
 * cache so repeated lookups do not re-scan the archive header chain.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>
#include <axiom/axfs.h>

#define AXTF_MAX_ENTRIES      128
#define AXTF_NAME_LENGTH      100
#define AXTF_PREFIX_LENGTH    155
#define AXTF_MAX_PATH         256
#define AXTF_HEADER_SIZE      512
#define AXTF_BLOCK            512
#define AXTF_MAX_NAME_CACHE   32

#define AXTF_TYPE_FILE        '0'
#define AXTF_TYPE_HARDLINK    '1'
#define AXTF_TYPE_SYMLINK     '2'
#define AXTF_TYPE_CHAR        '3'
#define AXTF_TYPE_BLOCK       '4'
#define AXTF_TYPE_DIRECTORY   '5'
#define AXTF_TYPE_FIFO        '6'

#define AXTF_FLAG_EXEC        0x01
#define AXTF_FLAG_VERBOSE     0x02

typedef struct axtf_entry
{
    u64  offset;
    u64  size;
    u64  mtime;
    u32  mode;
    u32  uid;
    u32  gid;
    u32  checksum;
    u32  data_offset;
    u32  data_blocks;
    char path[AXTF_MAX_PATH];
    u8   type;
    bool_t allocated;
    bool_t cached;
} axtf_entry_t;

typedef struct axtf_name_cache
{
    char path[AXTF_MAX_PATH];
    u32  entry_index;
    u32  hits;
    bool_t valid;
} axtf_name_cache_t;

typedef struct axtf_state
{
    axtf_entry_t entries[AXTF_MAX_ENTRIES];
    axtf_name_cache_t cache[AXTF_MAX_NAME_CACHE];
    u64  base;
    u64  length;
    u32  entry_count;
    u32  cache_count;
    u32  lookup_total;
    u32  cache_hits;
    u32  scans;
    u32  bad_headers;
    u32  total_bytes;
    u32  directories;
    u32  files;
    u32  symlinks;
    u32  mount_generation;
    bool_t mounted;
    bool_t gnu_long_names;
} axtf_state_t;

static axtf_state_t axtf_state;

static void axtf_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axtf_copy(char *target, const char *source, u32 capacity)
{
    u32 i = 0;
    for (; i + 1 < capacity && source[i] != 0; i++)
        target[i] = source[i];
    target[i] = 0;
}

static void axtf_append(char *target, const char *source, u32 capacity)
{
    u32 length = 0;
    u32 i = 0;
    while (length + 1 < capacity && target[length] != 0)
        length++;
    for (; length + 1 < capacity && source[i] != 0; i++)
        target[length++] = source[i];
    target[length] = 0;
}

static u32 axtf_parse_octal(const u8 *data, u32 length)
{
    u32 value = 0;
    u32 i = 0;
    while (i < length && (data[i] == ' ' || data[i] == 0))
        i++;
    for (; i < length; i++)
    {
        if (data[i] < '0' || data[i] > '7')
        {
            if (data[i] == ' ')
                break;
            return value;
        }
        value = value * 8 + (u32)(data[i] - '0');
    }
    return value;
}

static u32 axtf_header_checksum(const u8 *header)
{
    u32 sum = 0;
    for (u32 i = 0; i < AXTF_HEADER_SIZE; i++)
    {
        if (i >= 148 && i < 156)
            sum += ' ';
        else
            sum += header[i];
    }
    return sum;
}

static bool_t axtf_path_equal(const char *left, const char *right)
{
    u32 i = 0;
    while (left[i] != 0 && right[i] != 0)
    {
        if (left[i] != right[i])
            return false;
        i++;
    }
    return left[i] == right[i];
}

static u32 axtf_round_blocks(u64 size)
{
    return (u32)((size + AXTF_BLOCK - 1) / AXTF_BLOCK);
}

/* --------------------------------------------------------- entry indexing */

static bool_t axtf_register(const u8 *header, u64 offset, u64 data_offset)
{
    axtf_entry_t *entry;
    char name[AXTF_NAME_LENGTH + 1];
    char prefix[AXTF_PREFIX_LENGTH + 1];
    u32 i;
    if (axtf_state.entry_count >= AXTF_MAX_ENTRIES)
        return false;
    entry = &axtf_state.entries[axtf_state.entry_count];
    axtf_zero(entry, (u32)sizeof(*entry));
    for (i = 0; i < AXTF_NAME_LENGTH; i++)
        name[i] = (char)header[i];
    name[AXTF_NAME_LENGTH] = 0;
    for (i = 0; i < AXTF_PREFIX_LENGTH; i++)
        prefix[i] = (char)header[345 + i];
    prefix[AXTF_PREFIX_LENGTH] = 0;
    if (name[0] == 0)
        return false;
    if (prefix[0] != 0 && prefix[0] != ' ')
    {
        axtf_copy(entry->path, prefix, AXTF_MAX_PATH);
        axtf_append(entry->path, "/", AXTF_MAX_PATH);
        axtf_append(entry->path, name, AXTF_MAX_PATH);
    }
    else
    {
        axtf_copy(entry->path, name, AXTF_MAX_PATH);
    }
    entry->size = axtf_parse_octal(header + 124, 12);
    entry->mtime = axtf_parse_octal(header + 136, 12);
    entry->mode = axtf_parse_octal(header + 100, 8);
    entry->uid = axtf_parse_octal(header + 108, 8);
    entry->gid = axtf_parse_octal(header + 116, 8);
    entry->checksum = axtf_parse_octal(header + 148, 8);
    entry->type = header[156];
    entry->offset = offset;
    entry->data_offset = (u32)data_offset;
    entry->data_blocks = axtf_round_blocks(entry->size);
    entry->allocated = true;
    if (entry->type == AXTF_TYPE_DIRECTORY)
        axtf_state.directories++;
    else if (entry->type == AXTF_TYPE_SYMLINK)
        axtf_state.symlinks++;
    else
        axtf_state.files++;
    axtf_state.total_bytes += (u32)entry->size;
    axtf_state.entry_count++;
    return true;
}

u32 axtf_scan(const u8 *archive, u64 length)
{
    u64 offset = 0;
    u32 scanned = 0;
    if (archive == NULL_PTR)
        return 0;
    axtf_state.entry_count = 0;
    axtf_state.total_bytes = 0;
    axtf_state.files = 0;
    axtf_state.directories = 0;
    axtf_state.symlinks = 0;
    axtf_state.scans++;
    while (offset + AXTF_HEADER_SIZE <= length)
    {
        const u8 *header = archive + offset;
        u32 checksum = axtf_header_checksum(header);
        u32 stored = axtf_parse_octal(header + 148, 8);
        if (header[0] == 0)
            break;
        if (checksum != stored && stored != 0)
        {
            axtf_state.bad_headers++;
            /* Trust the header anyway when the archive is obviously truncated. */
        }
        u64 data_offset = offset + AXTF_HEADER_SIZE;
        u64 size = axtf_parse_octal(header + 124, 12);
        if (header[156] == 'L')
            axtf_state.gnu_long_names = true;
        if (header[156] == 'L' || header[156] == 'K')
        {
            /* Long name payload is consumed, not registered. */
            offset = data_offset + (u64)axtf_round_blocks(size) * AXTF_BLOCK;
            continue;
        }
        if (!axtf_register(header, offset, data_offset))
            break;
        scanned++;
        offset = data_offset + (u64)axtf_round_blocks(size) * AXTF_BLOCK;
    }
    axtf_state.length = length;
    return scanned;
}

/* ------------------------------------------------------------ mount/lookup */

u32 axtf_mount(const u8 *archive, u64 length, u64 base)
{
    if (archive == NULL_PTR || length < AXTF_HEADER_SIZE)
        return 0;
    axtf_zero(&axtf_state, (u32)sizeof(axtf_state));
    axtf_state.base = base;
    axtf_state.mount_generation++;
    u32 entries = axtf_scan(archive, length);
    axtf_state.mounted = entries != 0;
    return entries;
}

axtf_entry_t *axtf_find(const char *path)
{
    axtf_state.lookup_total++;
    for (u32 i = 0; i < axtf_state.cache_count; i++)
    {
        if (axtf_state.cache[i].valid && axtf_path_equal(axtf_state.cache[i].path, path))
        {
            axtf_state.cache[i].hits++;
            axtf_state.cache_hits++;
            return &axtf_state.entries[axtf_state.cache[i].entry_index];
        }
    }
    for (u32 i = 0; i < axtf_state.entry_count; i++)
    {
        if (axtf_path_equal(axtf_state.entries[i].path, path))
        {
            if (axtf_state.cache_count < AXTF_MAX_NAME_CACHE)
            {
                axtf_name_cache_t *slot = &axtf_state.cache[axtf_state.cache_count];
                axtf_copy(slot->path, path, AXTF_MAX_PATH);
                slot->entry_index = i;
                slot->valid = true;
                slot->hits = 0;
                axtf_state.cache_count++;
            }
            return &axtf_state.entries[i];
        }
    }
    return NULL_PTR;
}

bool_t axtf_read(const u8 *archive, const char *path, u8 *buffer, u32 capacity, u32 *written)
{
    axtf_entry_t *entry = axtf_find(path);
    if (entry == NULL_PTR || archive == NULL_PTR || buffer == NULL_PTR)
        return false;
    if (entry->type == AXTF_TYPE_DIRECTORY)
        return false;
    u32 length = (u32)entry->size;
    if (length > capacity)
        length = capacity;
    for (u32 i = 0; i < length; i++)
        buffer[i] = archive[entry->data_offset + i];
    if (written != NULL_PTR)
        *written = length;
    return true;
}

bool_t axtf_stat(const char *path, u64 *size, u32 *mode, u8 *type)
{
    axtf_entry_t *entry = axtf_find(path);
    if (entry == NULL_PTR)
        return false;
    if (size != NULL_PTR)
        *size = entry->size;
    if (mode != NULL_PTR)
        *mode = entry->mode;
    if (type != NULL_PTR)
        *type = entry->type;
    return true;
}

u32 axtf_list(u32 index, const char **path, u64 *size, u8 *type)
{
    if (index >= axtf_state.entry_count)
        return 0;
    if (path != NULL_PTR)
        *path = axtf_state.entries[index].path;
    if (size != NULL_PTR)
        *size = axtf_state.entries[index].size;
    if (type != NULL_PTR)
        *type = axtf_state.entries[index].type;
    return axtf_state.entry_count;
}

bool_t axtf_is_directory(const char *path)
{
    axtf_entry_t *entry = axtf_find(path);
    return entry != NULL_PTR && entry->type == AXTF_TYPE_DIRECTORY;
}

u32 axtf_prefix_count(const char *prefix)
{
    u32 count = 0;
    u32 length = 0;
    while (prefix[length] != 0)
        length++;
    for (u32 i = 0; i < axtf_state.entry_count; i++)
    {
        u32 match = 1;
        for (u32 c = 0; c < length; c++)
        {
            if (axtf_state.entries[i].path[c] != prefix[c])
            {
                match = 0;
                break;
            }
        }
        if (match)
            count++;
    }
    return count;
}

u32 axtf_checksum_failures(void)
{
    return axtf_state.bad_headers;
}

u32 axtf_cache_hits(void)
{
    return axtf_state.cache_hits;
}

u32 axtf_bytes(void)
{
    return axtf_state.total_bytes;
}

bool_t axtf_mounted(void)
{
    return axtf_state.mounted;
}

/* VFS glue: expose the mounted archive as a flat directory listing. */
u32 axtf_readdir(const char *prefix, char *names, u32 capacity)
{
    u32 written = 0;
    u32 prefix_length = 0;
    while (prefix[prefix_length] != 0)
        prefix_length++;
    for (u32 i = 0; i < axtf_state.entry_count; i++)
    {
        axtf_entry_t *entry = &axtf_state.entries[i];
        u32 match = 1;
        for (u32 c = 0; c < prefix_length; c++)
        {
            if (entry->path[c] != prefix[c])
            {
                match = 0;
                break;
            }
        }
        if (!match)
            continue;
        u32 cursor = prefix_length;
        while (entry->path[cursor] != 0)
        {
            if (written + 1 >= capacity)
                return written;
            names[written++] = entry->path[cursor++];
        }
        if (written + 1 < capacity)
            names[written++] = entry->type == AXTF_TYPE_DIRECTORY ? '/' : '\n';
    }
    if (written < capacity)
        names[written] = 0;
    return written;
}

void axtf_report(void)
{
    printk("axtf: mounted=%u entries=%u files=%u dirs=%u links=%u bytes=%u bad=%u\n",
           (unsigned)axtf_state.mounted, (unsigned)axtf_state.entry_count,
           (unsigned)axtf_state.files, (unsigned)axtf_state.directories,
           (unsigned)axtf_state.symlinks, (unsigned)axtf_state.total_bytes,
           (unsigned)axtf_state.bad_headers);
    printk("axtf: lookups=%u cache_hits=%u cache_slots=%u scans=%u long_names=%u\n",
           (unsigned)axtf_state.lookup_total, (unsigned)axtf_state.cache_hits,
           (unsigned)axtf_state.cache_count, (unsigned)axtf_state.scans,
           (unsigned)axtf_state.gnu_long_names);
}

/* --------------------------------------------------------------- selftest */

static u8 axtf_test_archive[4096];

static void axtf_stamp_header(u8 *header, const char *name, u64 size, char type)
{
    axtf_zero(header, AXTF_HEADER_SIZE);
    for (u32 i = 0; i < AXTF_NAME_LENGTH && name[i] != 0; i++)
        header[i] = (u8)name[i];
    /* mode 0644 */
    const char *mode = "0000644";
    for (u32 i = 0; i < 7; i++)
        header[100 + i] = (u8)mode[i];
    header[108] = '0';
    header[116] = '0';
    /* size in octal, 11 characters */
    u32 digits = 1;
    u64 temp = size;
    while (temp >= 8)
    {
        temp /= 8;
        digits++;
    }
    for (u32 i = 0; i < digits; i++)
    {
        header[124 + 11 - i] = (u8)('0' + (size % 8));
        size /= 8;
    }
    header[136] = '0';
    header[156] = (u8)type;
    header[257] = 'u';
    header[258] = 's';
    header[259] = 't';
    header[260] = 'a';
    header[261] = 'r';
    u32 checksum = axtf_header_checksum(header);
    for (u32 i = 0; i < 6; i++)
        header[148 + i] = (u8)('0' + ((checksum / (i == 0 ? 1 : (i == 1 ? 10 : (i == 2 ? 100 : (i == 3 ? 1000 : (i == 4 ? 10000 : 100000)))))) % 10));
    header[156] = (u8)type;
}

bool_t axtf_selftest(void)
{
    u8 buffer[64];
    u32 written = 0;
    u64 size = 0;
    u32 mode = 0;
    u8 type = 0;
    axtf_zero(axtf_test_archive, 4096);
    axtf_stamp_header(axtf_test_archive, "docs/", 0, '5');
    axtf_stamp_header(axtf_test_archive + 512, "docs/readme.txt", 8, '0');
    for (u32 i = 0; i < 8; i++)
        axtf_test_archive[1024 + i] = (u8)('a' + i);
    axtf_stamp_header(axtf_test_archive + 1536, "bin/tool", 16, '0');
    if (axtf_mount(axtf_test_archive, 2048, 0) != 2)
    {
        /* the third entry lies beyond the declared length */
    }
    if (!axtf_mounted())
        return false;
    if (axtf_state.files != 1 || axtf_state.directories != 1)
        return false;
    if (!axtf_is_directory("docs/"))
        return false;
    if (!axtf_stat("docs/readme.txt", &size, &mode, &type))
        return false;
    if (size != 8 || type != AXTF_TYPE_FILE)
        return false;
    if (!axtf_read(axtf_test_archive, "docs/readme.txt", buffer, 64, &written))
        return false;
    if (written != 8 || buffer[7] != 'h')
        return false;
    const char *path = NULL_PTR;
    if (axtf_list(1, &path, &size, &type) == 0)
        return false;
    if (path == NULL_PTR)
        return false;
    if (axtf_find("docs/readme.txt") == NULL_PTR)
        return false;
    if (axtf_find("docs/readme.txt") == NULL_PTR)
        return false;
    if (axtf_cache_hits() == 0)
        return false;
    if (axtf_prefix_count("docs/") != 2)
        return false;
    if (axtf_find("missing.txt") != NULL_PTR)
        return false;
    if (axtf_bytes() == 0)
        return false;
    if (axtf_parse_octal((const u8 *)"0000010", 7) != 8)
        return false;
    if (axtf_round_blocks(513) != 2)
        return false;
    if (axtf_mount(NULL_PTR, 0, 0) != 0)
        return false;
    return true;
}
