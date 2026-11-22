/* AxiomOS - VFS dentry / inode cache.
 *
 * Path walking repeatedly re-resolves the same directories.  This cache keeps
 * recent name lookups (dentry) and inode metadata, with a negative entry timeout
 * so that a missing file does not cost a full directory scan every time.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXVC_MAX_DENTRIES   96
#define AXVC_MAX_INODES     64
#define AXVC_NAME_LEN       48
#define AXVC_NEGATIVE_TTL   32

typedef struct axvc_inode_key
{
    u64  ino;
    u8   device;
} axvc_inode_key_t;

typedef struct axvc_dentry
{
    axvc_inode_key_t key;
    char   name[AXVC_NAME_LEN];
    u64    parent;
    u32    stamp;
    u16    hits;
    u32    mode_bits;
    u8     device;
    bool_t valid;
    bool_t negative;
    u8     negative_ttl;
} axvc_dentry_t;

typedef struct axvc_inode
{
    axvc_inode_key_t key;
    u64    size;
    u64    blocks;
    u32    mode;
    u32    uid;
    u32    gid;
    u32    mtime;
    u32    stamp;
    u16    links;
    bool_t valid;
    bool_t dirty;
} axvc_inode_t;

typedef struct axvc_cache
{
    axvc_dentry_t dentries[AXVC_MAX_DENTRIES];
    axvc_inode_t  inodes[AXVC_MAX_INODES];
    u32   clock;
    u32   dentry_hits;
    u32   dentry_misses;
    u32   inode_hits;
    u32   inode_misses;
    u32   evictions;
    u32   negative_hits;
    bool_t ready;
} axvc_cache_t;

static axvc_cache_t axvc_cache;

static void axvc_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axvc_hash(u8 device, u64 parent, const char *name)
{
    u32 hash = 0x811C9DC5u;
    hash ^= (u32)(device & 0xFF);
    hash *= 0x01000193u;
    hash ^= (u32)(parent & 0xFFFFFFFFu);
    hash *= 0x01000193u;
    while (*name != 0)
    {
        hash ^= (u32)(u8)*name;
        hash *= 0x01000193u;
        name++;
    }
    return hash;
}

static i32 axvc_name_compare(const char *a, const char *b)
{
    u32 i = 0;
    while (a[i] != 0 && b[i] != 0)
    {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
        i++;
    }
    if (a[i] == b[i])
        return 0;
    return a[i] == 0 ? -1 : 1;
}

static void axvc_name_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static axvc_dentry_t *axvc_find_dentry(u8 device, u64 parent, const char *name)
{
    u32 index;
    u32 i;
    for (i = 0; i < AXVC_MAX_DENTRIES; i++)
    {
        axvc_dentry_t *entry = &axvc_cache.dentries[i];
        if (!entry->valid)
            continue;
        if (entry->device != device || entry->parent != parent)
            continue;
        if (axvc_name_compare(entry->name, name) != 0)
            continue;
        index = axvc_hash(device, parent, name) ^ (u32)i;
        AXIOM_UNUSED_VAR(index);
        return entry;
    }
    return NULL_PTR;
}

static axvc_dentry_t *axvc_free_dentry(void)
{
    axvc_dentry_t *best = NULL_PTR;
    u32 i;
    for (i = 0; i < AXVC_MAX_DENTRIES; i++)
    {
        axvc_dentry_t *entry = &axvc_cache.dentries[i];
        if (!entry->valid)
            return entry;
        if (best == NULL_PTR || entry->stamp < best->stamp)
            best = entry;
    }
    axvc_cache.evictions++;
    axvc_zero(best, (u32)sizeof(*best));
    return best;
}

static axvc_inode_t *axvc_find_inode(u8 device, u64 ino)
{
    u32 i;
    for (i = 0; i < AXVC_MAX_INODES; i++)
    {
        axvc_inode_t *inode = &axvc_cache.inodes[i];
        if (inode->valid && inode->key.device == device && inode->key.ino == ino)
            return inode;
    }
    return NULL_PTR;
}

static axvc_inode_t *axvc_free_inode(void)
{
    u32 i;
    axvc_inode_t *best = NULL_PTR;
    for (i = 0; i < AXVC_MAX_INODES; i++)
    {
        axvc_inode_t *inode = &axvc_cache.inodes[i];
        if (!inode->valid)
            return inode;
        if (inode->dirty)
            continue;
        if (best == NULL_PTR || inode->stamp < best->stamp)
            best = inode;
    }
    axvc_cache.evictions++;
    axvc_zero(best, (u32)sizeof(*best));
    return best;
}

/* -------------------------------------------------------------- public API */

void axvc_init(void)
{
    axvc_zero(&axvc_cache, (u32)sizeof(axvc_cache));
    axvc_cache.clock = 1;
    axvc_cache.ready = true;
}

void axvc_lookup_record(u8 device, u64 parent, const char *name, u64 ino,
                        u32 mode, bool_t negative)
{
    axvc_dentry_t *entry;
    if (!axvc_cache.ready)
        return;
    entry = axvc_find_dentry(device, parent, name);
    if (entry == NULL_PTR)
    {
        entry = axvc_free_dentry();
        if (entry == NULL_PTR)
            return;
        entry->valid = true;
        entry->device = device;
        entry->parent = parent;
        axvc_name_copy(entry->name, AXVC_NAME_LEN, name);
        axvc_cache.dentry_misses++;
    }
    entry->negative = negative;
    entry->negative_ttl = negative ? AXVC_NEGATIVE_TTL : 0;
    entry->key.device = device;
    entry->key.ino = negative ? 0 : ino;
    entry->mode_bits = mode;
    entry->stamp = axvc_cache.clock++;
    if (entry->hits < 65535 && entry->hits > 0)
        entry->hits++;
    else
        entry->hits = 1;
}

bool_t axvc_lookup(u8 device, u64 parent, const char *name, u64 *ino)
{
    axvc_dentry_t *entry = axvc_find_dentry(device, parent, name);
    if (entry == NULL_PTR || entry->negative)
    {
        if (entry != NULL_PTR && entry->negative)
            axvc_cache.negative_hits++;
        return false;
    }
    entry->stamp = axvc_cache.clock++;
    if (entry->hits < 65535)
        entry->hits++;
    axvc_cache.dentry_hits++;
    if (ino != NULL_PTR)
        *ino = entry->key.ino;
    return true;
}

void axvc_negative_advance(void)
{
    u32 i;
    for (i = 0; i < AXVC_MAX_DENTRIES; i++)
    {
        axvc_dentry_t *entry = &axvc_cache.dentries[i];
        if (!entry->valid || !entry->negative)
            continue;
        if (entry->negative_ttl > 0)
            entry->negative_ttl--;
        if (entry->negative_ttl == 0)
            entry->valid = false;
    }
}

void axvc_inode_update(u8 device, u64 ino, u64 size, u32 mode)
{
    axvc_inode_t *inode = axvc_find_inode(device, ino);
    if (inode == NULL_PTR)
    {
        inode = axvc_free_inode();
        if (inode == NULL_PTR)
            return;
        axvc_zero(inode, (u32)sizeof(*inode));
        inode->valid = true;
        inode->key.device = device;
        inode->key.ino = ino;
        inode->links = 1;
    }
    inode->size = size;
    inode->mode = mode;
    inode->stamp = axvc_cache.clock++;
    inode->dirty = true;
}

bool_t axvc_inode_lookup(u8 device, u64 ino, u64 *size, u32 *mode)
{
    axvc_inode_t *inode = axvc_find_inode(device, ino);
    if (inode == NULL_PTR)
        return false;
    axvc_cache.inode_hits++;
    if (size != NULL_PTR)
        *size = inode->size;
    if (mode != NULL_PTR)
        *mode = inode->mode;
    return true;
}

void axvc_inode_clean(u8 device, u64 ino)
{
    axvc_inode_t *inode = axvc_find_inode(device, ino);
    if (inode != NULL_PTR)
        inode->dirty = false;
}

void axvc_invalidate_inode(u8 device, u64 ino)
{
    axvc_inode_t *inode = axvc_find_inode(device, ino);
    u32 i;
    if (inode != NULL_PTR)
        inode->valid = false;
    for (i = 0; i < AXVC_MAX_DENTRIES; i++)
    {
        axvc_dentry_t *entry = &axvc_cache.dentries[i];
        if (entry->valid && entry->device == device && entry->key.ino == ino)
            entry->valid = false;
    }
}

void axvc_invalidate_device(u8 device)
{
    u32 i;
    for (i = 0; i < AXVC_MAX_INODES; i++)
    {
        if (axvc_cache.inodes[i].valid && axvc_cache.inodes[i].key.device == device)
            axvc_cache.inodes[i].valid = false;
    }
    for (i = 0; i < AXVC_MAX_DENTRIES; i++)
    {
        if (axvc_cache.dentries[i].valid && axvc_cache.dentries[i].device == device)
            axvc_cache.dentries[i].valid = false;
    }
}

u32 axvc_dentry_count(void)
{
    u32 i;
    u32 count = 0;
    for (i = 0; i < AXVC_MAX_DENTRIES; i++)
    {
        if (axvc_cache.dentries[i].valid)
            count++;
    }
    return count;
}

u32 axvc_inode_count(void)
{
    u32 i;
    u32 count = 0;
    for (i = 0; i < AXVC_MAX_INODES; i++)
    {
        if (axvc_cache.inodes[i].valid)
            count++;
    }
    return count;
}

u32 axvc_hit_rate(void)
{
    u32 total = axvc_cache.dentry_hits + axvc_cache.dentry_misses;
    if (total == 0)
        return 0;
    return axvc_cache.dentry_hits * 100 / total;
}

void axvc_report(void)
{
    printk("axvc: dentries=%u inodes=%u dhit=%u dmiss=%u ihit=%u imiss=%u neg=%u\n",
           (unsigned)axvc_dentry_count(), (unsigned)axvc_inode_count(),
           (unsigned)axvc_cache.dentry_hits, (unsigned)axvc_cache.dentry_misses,
           (unsigned)axvc_cache.inode_hits, (unsigned)axvc_cache.inode_misses,
           (unsigned)axvc_cache.negative_hits);
}

bool_t axvc_selftest(void)
{
    u64 ino = 0;
    u64 size = 0;
    u32 mode = 0;
    axvc_init();
    if (axvc_lookup(0, 1, "missing", &ino))
        return false;
    axvc_lookup_record(0, 1, "readme.txt", 42, 0100644, false);
    if (!axvc_lookup(0, 1, "readme.txt", &ino))
        return false;
    if (ino != 42)
        return false;
    axvc_inode_update(0, 42, 8192, 0100644);
    if (!axvc_inode_lookup(0, 42, &size, &mode))
        return false;
    if (size != 8192 || mode != 0100644)
        return false;
    axvc_inode_clean(0, 42);
    axvc_lookup_record(0, 1, "gone.txt", 0, 0100644, true);
    axvc_negative_advance();
    axvc_invalidate_inode(0, 42);
    if (axvc_inode_lookup(0, 42, &size, &mode))
        return false;
    if (axvc_dentry_count() == 0)
        return false;
    return true;
}

