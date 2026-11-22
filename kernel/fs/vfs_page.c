/*
 * AxiomOS - page cache.
 *
 * A single cache shared by every filesystem that does not implement its own
 * read/write path.  Pages are 4 KiB and keyed by (inode pointer, index); the
 * list is a hash chain with a fixed bucket count so lookups never allocate.
 *
 * Because AxiomOS has one address space per process and no swap yet, a cached
 * page is simply a kernel-heap buffer that mirrors a file range.  Reads fill
 * the cache from the filesystem's `read` op; writes update the buffer and mark
 * it dirty; `fsync` walks a dirty list and pushes the ranges back through
 * `write`.
 *
 * Stats are exposed because the settings page shows hit rate as a graph.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axfs.h>

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

#define PAGE_BUCKETS 128

static axfs_page_t *g_page_hash[PAGE_BUCKETS];
static axfs_page_t *g_dirty_list;
static i32         g_page_count;
static u64         g_hits;
static u64         g_misses;
static u64         g_evictions;
static u64         g_bytes_in;
static u64         g_bytes_out;

static u32 bucket_for(axfs_inode_t *inode, u64 index)
{
    u64 mix = (u64)inode;
    mix ^= index * 0x9E3779B97F4A7C15ull;
    mix ^= mix >> 29;
    return (u32)(mix % PAGE_BUCKETS);
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

static axfs_page_t *page_alloc(axfs_inode_t *inode, u64 index)
{
    axfs_page_t *page = (axfs_page_t *)kzalloc(sizeof(*page));
    if (!page) {
        return 0;
    }
    page->data = (u8 *)kzalloc(AXFS_PAGE_MAX);
    if (!page->data) {
        kfree(page);
        return 0;
    }
    page->inode = inode;
    page->index = index;
    page->refs = 1;
    if (inode) {
        axfs_ref_inode(inode);
    }
    u32 bucket = bucket_for(inode, index);
    page->next = g_page_hash[bucket];
    g_page_hash[bucket] = page;
    g_page_count++;
    return page;
}

static void page_unlink(axfs_page_t *page)
{
    u32 bucket = bucket_for(page->inode, page->index);
    axfs_page_t **slot = &g_page_hash[bucket];
    while (*slot) {
        if (*slot == page) {
            *slot = page->next;
            break;
        }
        slot = &(*slot)->next;
    }
    axfs_page_t **dirty = &g_dirty_list;
    while (*dirty) {
        if (*dirty == page) {
            *dirty = page->next_dirty;
            break;
        }
        dirty = &(*dirty)->next_dirty;
    }
}

static void page_free(axfs_page_t *page)
{
    if (!page) {
        return;
    }
    page_unlink(page);
    if (page->data) {
        kfree(page->data);
    }
    if (page->inode) {
        axfs_unref_inode(page->inode);
    }
    g_page_count--;
    kfree(page);
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

axfs_page_t *axfs_page_get(axfs_inode_t *inode, u64 index, bool_t create)
{
    if (!inode) {
        return 0;
    }
    u32 bucket = bucket_for(inode, index);
    for (axfs_page_t *page = g_page_hash[bucket]; page; page = page->next) {
        if (page->inode == inode && page->index == index) {
            page->refs++;
            g_hits++;
            return page;
        }
    }
    g_misses++;
    if (!create) {
        return 0;
    }
    axfs_page_t *page = page_alloc(inode, index);
    if (!page) {
        return 0;
    }
    /* Newly created pages are zero filled by kzalloc; they are only marked
     * up to date once they have been read from the filesystem. */
    return page;
}

void axfs_page_put(axfs_page_t *page)
{
    if (!page) {
        return;
    }
    if (page->refs > 0) {
        page->refs--;
    }
    if (page->refs == 0 && !(page->flags & AXFS_PAGE_DIRTY)) {
        /* Keep a clean page around only while the cache has room. */
        if (g_page_count > AXFS_PAGE_MAX) {
            page_free(page);
            g_evictions++;
        }
    }
}

void axfs_page_mark_dirty(axfs_page_t *page)
{
    if (!page) {
        return;
    }
    if (page->flags & AXFS_PAGE_DIRTY) {
        return;
    }
    page->flags |= AXFS_PAGE_DIRTY;
    page->next_dirty = g_dirty_list;
    g_dirty_list = page;
}

/* ------------------------------------------------------------------ */
/* Fill and flush                                                      */
/* ------------------------------------------------------------------ */

/* Bring a page in from the filesystem.  A page past EOF is valid and zeroed,
 * because a sparse file reads as zeros there. */
static i32 page_fill(axfs_page_t *page)
{
    if (!page || !page->inode) {
        return AXFS_EINVAL;
    }
    if (page->flags & AXFS_PAGE_UPTODATE) {
        return AXFS_OK;
    }
    axfs_inode_t *inode = page->inode;
    u64 base = page->index * AXFS_PAGE_MAX;
    i32 rc = AXFS_OK;
    if (base < inode->size) {
        u64 span = inode->size - base;
        if (span > AXFS_PAGE_MAX) {
            span = AXFS_PAGE_MAX;
        }
        if (inode->ops && inode->ops->read) {
            axfs_file_t temp;
            ax_memset(&temp, 0, sizeof(temp));
            temp.inode = inode;
            temp.offset = base;
            i64 got = inode->ops->read(&temp, page->data, span);
            if (got < 0) {
                rc = (i32)got;
            }
            g_bytes_in += (u64)(got > 0 ? got : 0);
        }
    }
    if (rc == AXFS_OK) {
        page->flags |= AXFS_PAGE_UPTODATE;
    }
    return rc;
}

/* Write one page back through the filesystem. */
static i32 page_flush(axfs_page_t *page)
{
    if (!page || !page->inode) {
        return AXFS_EINVAL;
    }
    if (!(page->flags & AXFS_PAGE_DIRTY)) {
        return AXFS_OK;
    }
    axfs_inode_t *inode = page->inode;
    u64 base = page->index * AXFS_PAGE_MAX;
    u64 span = AXFS_PAGE_MAX;
    if (base + span > inode->size) {
        span = inode->size > base ? inode->size - base : 0;
    }
    if (span == 0) {
        page->flags &= ~AXFS_PAGE_DIRTY;
        return AXFS_OK;
    }
    if (!inode->ops || !inode->ops->write) {
        return AXFS_ENOSYS;
    }
    axfs_file_t temp;
    ax_memset(&temp, 0, sizeof(temp));
    temp.inode = inode;
    temp.offset = base;
    i64 wrote = inode->ops->write(&temp, page->data, span);
    if (wrote < 0) {
        return (i32)wrote;
    }
    g_bytes_out += (u64)wrote;
    page->flags &= ~AXFS_PAGE_DIRTY;
    return AXFS_OK;
}

i32 axfs_page_flush(axfs_inode_t *inode)
{
    i32 failures = 0;
    axfs_page_t *page = g_dirty_list;
    while (page) {
        axfs_page_t *next = page->next_dirty;
        if (!inode || page->inode == inode) {
            if (page_flush(page) != AXFS_OK) {
                failures++;
            }
        }
        page = next;
    }
    return failures ? AXFS_EIO : AXFS_OK;
}

/* Rebuild the dirty list after a flush, since flushed pages unlink
 * themselves lazily. */
i32 axfs_page_flush_all(void)
{
    i32 failures = 0;
    axfs_page_t *page = g_dirty_list;
    g_dirty_list = 0;
    while (page) {
        axfs_page_t *next = page->next_dirty;
        page->next_dirty = 0;
        if (page_flush(page) != AXFS_OK) {
            failures++;
        }
        if (page->flags & AXFS_PAGE_DIRTY) {
            page->next_dirty = g_dirty_list;
            g_dirty_list = page;
        }
        page = next;
    }
    return failures ? AXFS_EIO : AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Range read/write                                                    */
/* ------------------------------------------------------------------ */

i32 axfs_page_read(axfs_inode_t *inode, u64 offset, void *buffer, u64 count)
{
    if (!inode || !buffer) {
        return AXFS_EINVAL;
    }
    u8 *out = (u8 *)buffer;
    u64 done = 0;
    while (done < count) {
        u64 position = offset + done;
        u64 index = position / AXFS_PAGE_MAX;
        u64 inner = position % AXFS_PAGE_MAX;
        axfs_page_t *page = axfs_page_get(inode, index, (bool_t)1);
        if (!page) {
            return AXFS_ENOMEM;
        }
        i32 rc = page_fill(page);
        if (rc != AXFS_OK) {
            axfs_page_put(page);
            return rc;
        }
        u64 span = AXFS_PAGE_MAX - inner;
        if (span > count - done) {
            span = count - done;
        }
        for (u64 i = 0; i < span; i++) {
            out[done + i] = page->data[inner + i];
        }
        done += span;
        axfs_page_put(page);
    }
    return (i32)done;
}

i32 axfs_page_write(axfs_inode_t *inode, u64 offset, const void *buffer, u64 count)
{
    if (!inode || !buffer) {
        return AXFS_EINVAL;
    }
    const u8 *in = (const u8 *)buffer;
    u64 done = 0;
    while (done < count) {
        u64 position = offset + done;
        u64 index = position / AXFS_PAGE_MAX;
        u64 inner = position % AXFS_PAGE_MAX;
        axfs_page_t *page = axfs_page_get(inode, index, (bool_t)1);
        if (!page) {
            return AXFS_ENOMEM;
        }
        /* A partially written page must be filled first so the untouched
         * bytes keep their old contents. */
        if (!(page->flags & AXFS_PAGE_UPTODATE)) {
            i32 rc = page_fill(page);
            if (rc != AXFS_OK) {
                axfs_page_put(page);
                return rc;
            }
        }
        u64 span = AXFS_PAGE_MAX - inner;
        if (span > count - done) {
            span = count - done;
        }
        for (u64 i = 0; i < span; i++) {
            page->data[inner + i] = in[done + i];
        }
        axfs_page_mark_dirty(page);
        axfs_dirty(inode);
        done += span;
        axfs_page_put(page);
    }
    return (i32)done;
}

/* Drop cached pages in a range after a truncate. */
void axfs_page_invalidate(axfs_inode_t *inode, u64 offset, u64 count)
{
    if (!inode) {
        return;
    }
    u64 first = offset / AXFS_PAGE_MAX;
    u64 last = (offset + count - 1) / AXFS_PAGE_MAX;
    if (count == 0) {
        return;
    }
    for (i32 bucket = 0; bucket < PAGE_BUCKETS; bucket++) {
        axfs_page_t *page = g_page_hash[bucket];
        while (page) {
            axfs_page_t *next = page->next;
            if (page->inode == inode && page->index >= first && page->index <= last) {
                page->flags &= ~AXFS_PAGE_UPTODATE;
                if (page->refs == 0) {
                    page_free(page);
                    g_evictions++;
                }
            }
            page = next;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

void axfs_page_stats(u64 *hits, u64 *misses, u64 *pages)
{
    if (hits) {
        *hits = g_hits;
    }
    if (misses) {
        *misses = g_misses;
    }
    if (pages) {
        *pages = (u64)g_page_count;
    }
}

u32 axfs_page_count(void)
{
    return (u32)g_page_count;
}

/* Hit rate in percent, for the graph. */
i32 axfs_page_hit_rate(void)
{
    u64 total = g_hits + g_misses;
    if (total == 0) {
        return 0;
    }
    return (i32)((g_hits * 100) / total);
}

void axfs_page_dump(void)
{
    printk("page cache: pages=%d hits=%llu misses=%llu evict=%llu\n", g_page_count,
           g_hits, g_misses, g_evictions);
    printk("page cache: in=%llu bytes out=%llu bytes hitrate=%d%%\n", g_bytes_in,
           g_bytes_out, axfs_page_hit_rate());
}
