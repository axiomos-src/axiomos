/*
 * AxiomOS - VFS core: inode and dentry caches.
 *
 * All inodes are hashed by (superblock serial, inode number) and additionally
 * chained on an LRU list.  A dentry is hashed by (parent pointer, name) and
 * also kept in a child list so `readdir` can be served from cache when a
 * filesystem does not implement it.
 *
 * The caches are intentionally small and fixed: the kernel heap is a simple
 * bump allocator over a physical arena, so a fixed pool avoids fragmentation
 * during long uptimes.  Reference counting follows the classic rules:
 *
 *   - an inode with refs > 0 is pinned;
 *   - an inode with refs == 0 goes to the LRU tail and may be evicted;
 *   - eviction asks the superblock to write it back, then frees fs_data.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axfs.h>

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static axfs_inode_t *g_inode_hash[AXFS_INODE_BUCKETS];
static axfs_inode_t *g_inode_lru_head;
static axfs_inode_t *g_inode_lru_tail;
static i32           g_inode_count;

static axfs_dentry_t *g_dentry_hash[AXFS_DENTRY_BUCKETS];
static i32            g_dentry_count;

static u32 g_super_serial = 1;
static u32 g_mount_serial = 1;

static u64 g_inode_allocs;
static u64 g_inode_frees;
static u64 g_dentry_allocs;
static u64 g_dentry_frees;
static u64 g_inode_evictions;
static u64 g_dentry_evictions;

/* A monotonic clock the rest of the kernel fills in; before the timer is up
 * it reads zero, which filesystems interpret as "unknown time". */
static u64 g_now;

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

u64 axfs_now(void)
{
    return g_now;
}

void axfs_set_now(u64 seconds)
{
    g_now = seconds;
}

/* ------------------------------------------------------------------ */
/* Hash helpers                                                        */
/* ------------------------------------------------------------------ */

usize axfs_name_hash(const char *name, bool_t casefold)
{
    /* FNV-1a over the name; casefolding happens before mixing so two
     * spellings of the same name land in the same bucket. */
    u32 hash = 2166136261u;
    if (!name) {
        return 0;
    }
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (casefold && c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        hash ^= (u32)(u8)c;
        hash *= 16777619u;
    }
    return (usize)hash;
}

bool_t axfs_name_equals(const char *a, const char *b, bool_t casefold)
{
    if (!a || !b) {
        return (bool_t)0;
    }
    if (!casefold) {
        return (bool_t)(ax_strcmp(a, b) == 0);
    }
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb + 32);
        }
        if (ca != cb) {
            return (bool_t)0;
        }
        a++;
        b++;
    }
    return (bool_t)(*a == 0 && *b == 0);
}

/* Reject empty names, embedded slashes, and the two dot names when they are
 * about to be created as real entries. */
i32 axfs_name_valid(const char *name)
{
    if (!name || !name[0]) {
        return AXFS_EINVAL;
    }
    usize len = ax_strlen(name);
    if (len > AXFS_NAME_MAX) {
        return AXFS_ENAMETOOLONG;
    }
    if (ax_strcmp(name, ".") == 0 || ax_strcmp(name, "..") == 0) {
        return AXFS_EINVAL;
    }
    for (usize i = 0; i < len; i++) {
        if (name[i] == '/') {
            return AXFS_EINVAL;
        }
        if (name[i] == 0) {
            return AXFS_EINVAL;
        }
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Inode cache                                                         */
/* ------------------------------------------------------------------ */

static u32 inode_bucket(u32 serial, u64 ino)
{
    u32 mixed = serial * 31u + (u32)(ino ^ (ino >> 32));
    return mixed % AXFS_INODE_BUCKETS;
}

static void lru_unlink(axfs_inode_t *inode)
{
    if (inode->lru_prev) {
        inode->lru_prev->lru_next = inode->lru_next;
    } else if (g_inode_lru_head == inode) {
        g_inode_lru_head = inode->lru_next;
    }
    if (inode->lru_next) {
        inode->lru_next->lru_prev = inode->lru_prev;
    } else if (g_inode_lru_tail == inode) {
        g_inode_lru_tail = inode->lru_prev;
    }
    inode->lru_prev = 0;
    inode->lru_next = 0;
}

static void lru_push_front(axfs_inode_t *inode)
{
    inode->lru_prev = 0;
    inode->lru_next = g_inode_lru_head;
    if (g_inode_lru_head) {
        g_inode_lru_head->lru_prev = inode;
    }
    g_inode_lru_head = inode;
    if (!g_inode_lru_tail) {
        g_inode_lru_tail = inode;
    }
}

static void hash_insert(axfs_inode_t *inode)
{
    u32 bucket = inode_bucket(inode->super ? inode->super->serial : 0, inode->ino);
    inode->hash_next = g_inode_hash[bucket];
    g_inode_hash[bucket] = inode;
}

static void hash_remove(axfs_inode_t *inode)
{
    u32 bucket = inode_bucket(inode->super ? inode->super->serial : 0, inode->ino);
    axfs_inode_t **slot = &g_inode_hash[bucket];
    while (*slot) {
        if (*slot == inode) {
            *slot = inode->hash_next;
            inode->hash_next = 0;
            return;
        }
        slot = &(*slot)->hash_next;
    }
}

axfs_inode_t *axfs_alloc_inode(axfs_super_t *super, u64 ino, u8 type, u32 mode)
{
    axfs_inode_t *inode = (axfs_inode_t *)kzalloc(sizeof(*inode));
    if (!inode) {
        return 0;
    }
    inode->ino = ino;
    inode->type = type;
    inode->mode = mode;
    inode->uid = 0;
    inode->gid = 0;
    inode->nlink = type == AXFS_FT_DIR ? 2 : 1;
    inode->flags = AXFS_I_NEW;
    inode->refs = 1;
    inode->super = super;
    inode->mount = super ? super->mount : 0;
    inode->ops = super ? super->root_ops : 0;
    inode->atime = g_now;
    inode->mtime = g_now;
    inode->ctime = g_now;
    inode->version = 1;
    if (super) {
        if (!super->serial) {
            super->serial = g_super_serial++;
        }
        if (!super->mount) {
            super->mount = axfs_mount_add("/", super, 0, 0);
        }
        inode->mount = super->mount;
    }
    g_inode_allocs++;
    g_inode_count++;
    hash_insert(inode);
    lru_push_front(inode);
    return inode;
}

void axfs_ref_inode(axfs_inode_t *inode)
{
    if (!inode) {
        return;
    }
    inode->refs++;
    /* A newly referenced inode leaves the eviction list. */
    lru_unlink(inode);
}

/* Write back and free; called only when refs reach zero and the inode is the
 * LRU tail. */
static void evict_inode(axfs_inode_t *inode)
{
    if (!inode || inode->refs > 0) {
        return;
    }
    if (inode->flags & AXFS_I_DIRTY) {
        if (inode->super && inode->super->ops && inode->super->ops->write_inode) {
            inode->super->ops->write_inode(inode);
        }
        inode->flags &= ~AXFS_I_DIRTY;
    }
    if (inode->super && inode->super->ops && inode->super->ops->evict_inode) {
        inode->super->ops->evict_inode(inode);
    }
    hash_remove(inode);
    lru_unlink(inode);
    g_inode_count--;
    g_inode_evictions++;
    kfree(inode);
}

void axfs_unref_inode(axfs_inode_t *inode)
{
    if (!inode) {
        return;
    }
    if (inode->refs > 0) {
        inode->refs--;
    }
    if (inode->refs == 0) {
        /* Park at the front of the LRU so recent inodes survive the longest. */
        lru_unlink(inode);
        lru_push_front(inode);
        if (g_inode_count > AXFS_INODE_CACHE_MAX) {
            /* Evict from the tail, skipping any still referenced inode. */
            axfs_inode_t *victim = g_inode_lru_tail;
            while (victim && victim->refs > 0) {
                victim = victim->lru_prev;
            }
            if (victim) {
                evict_inode(victim);
            }
        }
    }
}

axfs_inode_t *axfs_iget(axfs_super_t *super, u64 ino)
{
    if (!super) {
        return 0;
    }
    u32 bucket = inode_bucket(super->serial, ino);
    for (axfs_inode_t *inode = g_inode_hash[bucket]; inode; inode = inode->hash_next) {
        if (inode->ino == ino && inode->super == super) {
            axfs_ref_inode(inode);
            return inode;
        }
    }
    return 0;
}

void axfs_iput(axfs_inode_t *inode)
{
    axfs_unref_inode(inode);
}

void axfs_mark_dirty(axfs_inode_t *inode)
{
    if (!inode) {
        return;
    }
    inode->flags |= AXFS_I_DIRTY;
    inode->version++;
    if (inode->super && inode->super->ops && inode->super->ops->write_inode) {
        /* Pseudo filesystems write synchronously; disk filesystems defer to
         * the flush path. */
        if ((inode->super->features & AXFS_F_JOURNAL) == 0) {
            inode->super->ops->write_inode(inode);
            inode->flags &= ~AXFS_I_DIRTY;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Dentry cache                                                        */
/* ------------------------------------------------------------------ */

static u32 dentry_bucket(axfs_dentry_t *parent, const char *name, bool_t casefold)
{
    usize h = axfs_name_hash(name, casefold);
    u32 mix = (u32)h ^ (u32)(((u64)parent) >> 3);
    return mix % AXFS_DENTRY_BUCKETS;
}

axfs_dentry_t *axfs_alloc_dentry(axfs_dentry_t *parent, const char *name,
                                 axfs_inode_t *inode)
{
    axfs_dentry_t *dentry = (axfs_dentry_t *)kzalloc(sizeof(*dentry));
    if (!dentry) {
        return 0;
    }
    usize len = name ? ax_strlen(name) : 0;
    if (len > AXFS_NAME_MAX) {
        kfree(dentry);
        return 0;
    }
    if (name) {
        for (usize i = 0; i < len; i++) {
            dentry->name[i] = name[i];
        }
        dentry->name[len] = 0;
    }
    dentry->name_len = (u16)len;
    dentry->parent = parent;
    dentry->refs = 1;
    if (inode) {
        axfs_ref_inode(inode);
        dentry->inode = inode;
        dentry->type = inode->type;
    }
    dentry->hash = dentry_bucket(parent, dentry->name,
                                 parent && parent->inode && parent->inode->super
                                     ? (bool_t)((parent->inode->super->features &
                                                 AXFS_F_CASEFOLD) != 0)
                                     : (bool_t)0);
    dentry->hash_next = g_dentry_hash[dentry->hash];
    g_dentry_hash[dentry->hash] = dentry;
    g_dentry_count++;
    g_dentry_allocs++;
    return dentry;
}

void axfs_ref_dentry(axfs_dentry_t *dentry)
{
    if (dentry) {
        dentry->refs++;
    }
}

void axfs_unref_dentry(axfs_dentry_t *dentry)
{
    if (!dentry) {
        return;
    }
    if (dentry->refs > 0) {
        dentry->refs--;
    }
    if (dentry->refs > 0) {
        return;
    }
    /* Remove from the parent's child list, from the hash, and release the
     * inode reference. */
    if (dentry->parent) {
        axfs_dentry_remove(dentry->parent, dentry);
    }
    axfs_dentry_t **slot = &g_dentry_hash[dentry->hash % AXFS_DENTRY_BUCKETS];
    while (*slot) {
        if (*slot == dentry) {
            *slot = dentry->hash_next;
            break;
        }
        slot = &(*slot)->hash_next;
    }
    if (dentry->inode) {
        axfs_unref_inode(dentry->inode);
        dentry->inode = 0;
    }
    g_dentry_count--;
    g_dentry_frees++;
    g_dentry_evictions++;
    kfree(dentry);
}

void axfs_dentry_add(axfs_dentry_t *parent, axfs_dentry_t *child)
{
    if (!parent || !child) {
        return;
    }
    child->parent = parent;
    child->sibling = 0;
    if (!parent->children) {
        parent->children = child;
        return;
    }
    axfs_dentry_t *tail = parent->children;
    while (tail->sibling) {
        tail = tail->sibling;
    }
    tail->sibling = child;
}

void axfs_dentry_remove(axfs_dentry_t *parent, axfs_dentry_t *child)
{
    if (!parent || !child) {
        return;
    }
    axfs_dentry_t **slot = &parent->children;
    while (*slot) {
        if (*slot == child) {
            *slot = child->sibling;
            child->sibling = 0;
            return;
        }
        slot = &(*slot)->sibling;
    }
}

/* Look up a cached child.  The fast path is the hash bucket; the slow path
 * walks the child list so a casefold filesystem still finds a match when the
 * hash was computed with different casing. */
axfs_dentry_t *axfs_lookup_dentry(axfs_dentry_t *parent, const char *name)
{
    if (!parent || !name) {
        return 0;
    }
    bool_t casefold = (bool_t)0;
    u32 bucket = dentry_bucket(parent, name, casefold);
    for (axfs_dentry_t *dentry = g_dentry_hash[bucket]; dentry; dentry = dentry->hash_next) {
        if (dentry->parent == parent && axfs_name_equals(dentry->name, name, casefold)) {
            axfs_ref_dentry(dentry);
            return dentry;
        }
    }
    for (axfs_dentry_t *dentry = parent->children; dentry; dentry = dentry->sibling) {
        if (axfs_name_equals(dentry->name, name, (bool_t)1)) {
            axfs_ref_dentry(dentry);
            return dentry;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cache statistics and teardown                                       */
/* ------------------------------------------------------------------ */

void axfs_cache_stats(axfs_cache_stats_t *out)
{
    if (!out) {
        return;
    }
    out->inode_allocs = g_inode_allocs;
    out->inode_frees = g_inode_frees;
    out->inode_evictions = g_inode_evictions;
    out->dentry_allocs = g_dentry_allocs;
    out->dentry_frees = g_dentry_frees;
    out->dentry_evictions = g_dentry_evictions;
    out->live_inodes = g_inode_count;
    out->live_dentries = g_dentry_count;
}

/* Shrink every unreferenced inode; returns how many were reclaimed. */
i32 axfs_shrink_inodes(i32 target)
{
    i32 reclaimed = 0;
    while (reclaimed < target && g_inode_lru_tail) {
        axfs_inode_t *victim = g_inode_lru_tail;
        if (victim->refs > 0) {
            break;
        }
        evict_inode(victim);
        reclaimed++;
    }
    return reclaimed;
}

/* Drop everything; used by the shutdown path.  Anything still referenced is
 * leaked deliberately, because freeing it would pull the rug from under a
 * caller that is mid-teardown. */
void axfs_cache_release(bool_t force)
{
    for (i32 i = 0; i < AXFS_DENTRY_BUCKETS; i++) {
        axfs_dentry_t *dentry = g_dentry_hash[i];
        while (dentry) {
            axfs_dentry_t *next = dentry->hash_next;
            if (dentry->refs == 1) {
                dentry->refs = 0;
                axfs_unref_dentry(dentry);
            } else if (force) {
                dentry->refs = 1;
                axfs_unref_dentry(dentry);
            }
            dentry = next;
        }
        g_dentry_hash[i] = 0;
    }
    for (i32 i = 0; i < AXFS_INODE_BUCKETS; i++) {
        axfs_inode_t *inode = g_inode_hash[i];
        while (inode) {
            axfs_inode_t *next = inode->hash_next;
            inode->refs = 1;
            axfs_unref_inode(inode);
            inode = next;
        }
        g_inode_hash[i] = 0;
    }
    if (force) {
        g_inode_lru_head = 0;
        g_inode_lru_tail = 0;
        g_inode_count = 0;
        g_dentry_count = 0;
    }
}

/* A compact textual dump for the diagnostics console: `fs cache`. */
void axfs_cache_dump(void)
{
    axfs_cache_stats_t stats;
    axfs_cache_stats(&stats);
    printk("vfs cache: inodes live=%d alloc=%llu free=%llu evict=%llu\n",
            stats.live_inodes, stats.inode_allocs, stats.inode_frees,
            stats.inode_evictions);
    printk("vfs cache: dentries live=%d alloc=%llu free=%llu evict=%llu\n",
            stats.live_dentries, stats.dentry_allocs, stats.dentry_frees,
            stats.dentry_evictions);
    printk("vfs cache: super serials=%u mounts=%u time=%llu\n", g_super_serial,
            g_mount_serial, g_now);
}

u32 axfs_next_super_serial(void)
{
    return g_super_serial++;
}

u32 axfs_next_mount_serial(void)
{
    return g_mount_serial++;
}
