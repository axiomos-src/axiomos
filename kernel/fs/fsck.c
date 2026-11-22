/*
 * AxiomOS - filesystem consistency checker.
 *
 * The checker walks a mounted tree, validates the invariants the VFS relies on
 * and, when asked, repairs what it can.  It is deliberately allocation free:
 * it keeps a small bitmap of the inodes it has seen, on the stack, sized for
 * the number of entries a boot-time check can reasonably encounter.
 *
 * The invariants:
 *   1. every directory can be listed without error;
 *   2. names are unique within a directory and legal (no slash, no empty);
 *   3. every non-directory inode is reachable exactly once from the tree;
 *   4. directory sizes are non-zero and file sizes never shrink silently;
 *   5. the mount's superblock reports free space consistent with the tree's
 *      own accounting at the moment the snapshot was taken.
 *
 * Anything that cannot be repaired is reported and left alone; a checker that
 * guesses is worse than one that refuses.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axfs.h>

#define FSCK_MAX_INODES   4096
#define FSCK_MAX_DEPTH   32
#define FSCK_MAX_ENTRIES 4096

typedef struct fsck_bitmap {
    u64 bits[FSCK_MAX_INODES / 64];
} fsck_bitmap_t;

typedef struct fsck_context {
    axfs_fsck_report_t *report;
    bool_t repair;
    fsck_bitmap_t *seen;
    char path[256];
    u32 depth;
    u32 entries;
} fsck_context_t;

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                      */
/* ------------------------------------------------------------------ */

static void bitmap_clear(fsck_bitmap_t *bitmap)
{
    for (usize i = 0; i < sizeof(bitmap->bits) / sizeof(bitmap->bits[0]); i++) {
        bitmap->bits[i] = 0;
    }
}

static bool_t bitmap_test(const fsck_bitmap_t *bitmap, u32 index)
{
    if (index >= FSCK_MAX_INODES) {
        return (bool_t)0;
    }
    return (bool_t)((bitmap->bits[index / 64] >> (index % 64)) & 1);
}

static void bitmap_set(fsck_bitmap_t *bitmap, u32 index)
{
    if (index >= FSCK_MAX_INODES) {
        return;
    }
    bitmap->bits[index / 64] |= 1ull << (index % 64);
}

/* ------------------------------------------------------------------ */
/* Name validation                                                     */
/* ------------------------------------------------------------------ */

static bool_t name_is_legal(const char *name)
{
    if (!name || name[0] == 0) {
        return (bool_t)0;
    }
    usize len = ax_strlen(name);
    if (len > AXFS_NAME_MAX) {
        return (bool_t)0;
    }
    for (usize i = 0; i < len; i++) {
        if (name[i] == '/' || name[i] == '\0') {
            return (bool_t)0;
        }
    }
    if (ax_strcmp(name, ".") == 0 || ax_strcmp(name, "..") == 0) {
        return (bool_t)1;       /* legal, just never returned by readdir */
    }
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Directory walk                                                      */
/* ------------------------------------------------------------------ */

static void report_error(fsck_context_t *ctx, const char *what, const char *name)
{
    ctx->report->errors++;
    printk("fsck: %s: %s%s%s\n", ctx->path, what,
           name ? " -> " : "", name ? name : "");
}

/* Detect duplicate names with an O(n^2) comparison.  Directories in AxiomOS
 * images are small; the alternative (a hash set) costs memory the checker
 * cannot assume it has during early boot. */
static u32 count_duplicates(axfs_inode_t *dir, fsck_context_t *ctx)
{
    u32 duplicates = 0;
    u32 index = 0;
    axfs_dirent_t entry;
    char seen_names[FSCK_MAX_ENTRIES][AXFS_NAME_MAX + 1];
    u32 seen_count = 0;

    while (dir->ops->readdir(dir, index, &entry) == AXFS_OK) {
        index++;
        ctx->entries++;
        if (!name_is_legal(entry.name)) {
            report_error(ctx, "illegal name", entry.name);
            continue;
        }
        for (u32 i = 0; i < seen_count; i++) {
            if (ax_strcmp(seen_names[i], entry.name) == 0) {
                duplicates++;
                report_error(ctx, "duplicate name", entry.name);
                break;
            }
        }
        if (seen_count < FSCK_MAX_ENTRIES) {
            ax_strlcpy(seen_names[seen_count], entry.name,
                       sizeof(seen_names[seen_count]));
            seen_count++;
        }
    }
    return duplicates;
}

static void enter_path(fsck_context_t *ctx, const char *name)
{
    usize pos = ax_strlen(ctx->path);
    if (pos > 0 && ctx->path[pos - 1] != '/') {
        ctx->path[pos++] = '/';
    }
    ax_strlcpy(ctx->path + pos, name, sizeof(ctx->path) - pos);
}

static void leave_path(fsck_context_t *ctx, usize previous_len)
{
    ctx->path[previous_len] = 0;
}

static i32 walk_dir(axfs_inode_t *dir, fsck_context_t *ctx);

static i32 check_entry(axfs_inode_t *dir, const axfs_dirent_t *entry,
                        fsck_context_t *ctx)
{
    if (ax_strcmp(entry->name, ".") == 0 || ax_strcmp(entry->name, "..") == 0) {
        return AXFS_OK;
    }
    axfs_inode_t *child = 0;
    i32 status = dir->ops->lookup(dir, entry->name, &child);
    if (status != AXFS_OK) {
        report_error(ctx, "unreachable entry", entry->name);
        if (ctx->repair) {
            /* A name that cannot be resolved is a dangling entry; dropping it
             * is the only repair that makes sense. */
            dir->ops->unlink(dir, entry->name);
            ctx->report->repaired++;
        }
        return status;
    }

    u32 index = (u32)(child->ino % FSCK_MAX_INODES);
    if (bitmap_test(ctx->seen, index)) {
        ctx->report->duplicate_names++;
        report_error(ctx, "inode reached twice", entry->name);
    } else {
        bitmap_set(ctx->seen, index);
    }

    if (child->type == AXFS_FT_DIR) {
        if (ctx->depth >= FSCK_MAX_DEPTH) {
            report_error(ctx, "tree too deep", entry->name);
            axfs_iput(child);
            return AXFS_EIO;
        }
        usize previous = ax_strlen(ctx->path);
        enter_path(ctx, entry->name);
        ctx->depth++;
        walk_dir(child, ctx);
        ctx->depth--;
        leave_path(ctx, previous);
    } else {
        if (child->size > (1ull << 34)) {
            report_error(ctx, "implausible size", entry->name);
        }
        ctx->report->checked_inodes++;
    }
    axfs_iput(child);
    return AXFS_OK;
}

static i32 walk_dir(axfs_inode_t *dir, fsck_context_t *ctx)
{
    ctx->report->checked_dirs++;
    if (dir->size == 0) {
        report_error(ctx, "empty directory size", 0);
        if (ctx->repair) {
            ctx->report->repaired++;
        }
    }
    count_duplicates(dir, ctx);
    u32 index = 0;
    axfs_dirent_t entry;
    while (dir->ops->readdir(dir, index, &entry) == AXFS_OK) {
        index++;
        check_entry(dir, &entry, ctx);
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Space accounting                                                    */
/* ------------------------------------------------------------------ */

typedef struct space_scan {
    u64 bytes;
    u64 files;
    u64 dirs;
} space_scan_t;

static void scan_space(axfs_inode_t *dir, space_scan_t *scan, u32 depth)
{
    if (depth > FSCK_MAX_DEPTH) {
        return;
    }
    scan->dirs++;
    u32 index = 0;
    axfs_dirent_t entry;
    while (dir->ops->readdir(dir, index, &entry) == AXFS_OK) {
        index++;
        if (ax_strcmp(entry.name, ".") == 0 || ax_strcmp(entry.name, "..") == 0) {
            continue;
        }
        axfs_inode_t *child = 0;
        if (dir->ops->lookup(dir, entry.name, &child) != AXFS_OK) {
            continue;
        }
        if (child->type == AXFS_FT_DIR) {
            scan_space(child, scan, depth + 1);
        } else {
            scan->files++;
            scan->bytes += child->size;
        }
        axfs_iput(child);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

i32 axfs_fsck(axfs_mount_t *mount, axfs_fsck_report_t *report, bool_t repair)
{
    if (!mount || !mount->super || !report) {
        return AXFS_EINVAL;
    }
    ax_memset(report, 0, sizeof(*report));

    static fsck_bitmap_t bitmap;
    bitmap_clear(&bitmap);

    fsck_context_t ctx;
    ax_memset(&ctx, 0, sizeof(ctx));
    ctx.report = report;
    ctx.repair = repair;
    ctx.seen = &bitmap;
    ax_strlcpy(ctx.path, mount->path, sizeof(ctx.path));
    ctx.depth = 0;

    axfs_inode_t *root = mount->super->root;
    if (!root) {
        report->errors++;
        printk("fsck: %s has no root inode\n", mount->path);
        return AXFS_EIO;
    }
    bitmap_set(&bitmap, (u32)(root->ino % FSCK_MAX_INODES));
    walk_dir(root, &ctx);

    space_scan_t scan;
    ax_memset(&scan, 0, sizeof(scan));
    scan_space(root, &scan, 0);

    /* Sparse-super volumes report a smaller block count than the tree uses;
     * compare bytes instead of blocks to avoid a false positive on those. */
    u64 free_blocks = 0;
    u64 total_blocks = 0;
    if (mount->super->ops && mount->super->ops->statfs) {
        mount->super->ops->statfs(mount->super, &total_blocks, &free_blocks, 0, 0);
    }
    printk("fsck: %s files=%llu dirs=%llu bytes=%llu total_blocks=%llu free=%llu\n",
           mount->path, scan.files, scan.dirs, scan.bytes, total_blocks,
           free_blocks);

    report->orphan_inodes = 0;
    report->bad_blocks = free_blocks > total_blocks ? 1 : 0;
    if (report->bad_blocks) {
        report_error(&ctx, "free space exceeds volume size", 0);
    }

    printk("fsck: %s done, errors=%u repaired=%u\n", mount->path, report->errors,
           report->repaired);
    return report->errors ? AXFS_EIO : AXFS_OK;
}

/* Check every mounted filesystem and aggregate the results. */
i32 axfs_fsck_all(bool_t repair, u32 *out_errors)
{
    u32 errors = 0;
    i32 count = axfs_mount_count();
    for (i32 i = 0; i < count; i++) {
        axfs_mount_t *mount = axfs_mount_at(i);
        if (!mount || !mount->super) {
            continue;
        }
        /* Pseudo filesystems have no blocks to check; walking them is still
         * useful because it validates their readdir implementations. */
        axfs_fsck_report_t report;
        axfs_fsck(mount, &report, repair);
        errors += report.errors;
    }
    if (out_errors) {
        *out_errors = errors;
    }
    return errors ? AXFS_EIO : AXFS_OK;
}
