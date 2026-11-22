/*
 * AxiomOS - VFS mount table.
 *
 * Mounts are a tree, not a flat list: `/`, `/mnt`, `/mnt/usb` nest, and
 * unmounting `/mnt` while `/mnt/usb` is mounted must fail with EBUSY.  Each
 * mount remembers the inode it covers, so `..` from the mounted root can step
 * back into the parent filesystem's directory.
 *
 * Names are compared after normalisation, so `/mnt/` and `/mnt` are the same
 * mount point and a second mount on the same path is rejected.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axfs.h>

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static axfs_mount_t *g_root_mount;
static axfs_mount_t *g_mounts[AXFS_MOUNT_MAX];
static i32           g_mount_count;
static u32           g_dirty_gen;

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

static bool_t same_point(const char *a, const char *b)
{
    char na[AXFS_PATH_MAX];
    char nb[AXFS_PATH_MAX];
    if (!a || !b) {
        return (bool_t)0;
    }
    if (axfs_path_normalise(a, na, sizeof(na)) < 0) {
        ax_strlcpy(na, a, sizeof(na));
    }
    if (axfs_path_normalise(b, nb, sizeof(nb)) < 0) {
        ax_strlcpy(nb, b, sizeof(nb));
    }
    return (bool_t)(ax_strcmp(na, nb) == 0);
}

axfs_mount_t *axfs_mount_root(void)
{
    return g_root_mount;
}

i32 axfs_mount_count(void)
{
    return g_mount_count;
}

axfs_mount_t *axfs_mount_find(const char *path)
{
    if (!path) {
        return 0;
    }
    /* Longest matching prefix wins so `/mnt/usb` is found before `/mnt`. */
    axfs_mount_t *best = 0;
    usize best_len = 0;
    for (i32 i = 0; i < g_mount_count; i++) {
        axfs_mount_t *mount = g_mounts[i];
        if (!mount) {
            continue;
        }
        usize len = ax_strlen(mount->path);
        if (same_point(path, mount->path)) {
            return mount;
        }
        if (ax_strncmp(path, mount->path, len) == 0 &&
            (path[len] == '/' || mount->path[len - 1] == '/')) {
            if (len > best_len) {
                best_len = len;
                best = mount;
            }
        }
    }
    return best ? best : g_root_mount;
}

/* The mount whose root is exactly this inode.  Used by the resolver to descend
 * when a path lands on a mount point. */
axfs_mount_t *axfs_mount_covering(const char *path, axfs_inode_t *inode)
{
    if (!inode) {
        return 0;
    }
    for (i32 i = 0; i < g_mount_count; i++) {
        axfs_mount_t *mount = g_mounts[i];
        if (mount && mount->root == inode && mount->path[0] == '/') {
            /* Only descend when the path actually reaches this mount point. */
            if (mount->covered == inode || same_point(path, mount->path)) {
                return mount;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Add / remove                                                        */
/* ------------------------------------------------------------------ */

axfs_mount_t *axfs_mount_add(const char *path, axfs_super_t *super, u32 flags,
                             axfs_inode_t *covered)
{
    if (!path || !super) {
        return 0;
    }
    if (g_mount_count >= AXFS_MOUNT_MAX) {
        return 0;
    }
    /* A root mount is created during superblock construction, before any other
     * mount exists.  A second `/` would be ambiguous. */
    if (g_root_mount && same_point(path, "/") && path[0] == '/') {
        if (super != g_root_mount->super) {
            return 0;
        }
    }

    axfs_mount_t *mount = (axfs_mount_t *)kzalloc(sizeof(*mount));
    if (!mount) {
        return 0;
    }
    char normalised[AXFS_PATH_MAX];
    if (axfs_path_normalise(path, normalised, sizeof(normalised)) < 0) {
        ax_strlcpy(normalised, path, sizeof(normalised));
    }
    ax_strlcpy(mount->path, normalised, sizeof(mount->path));
    mount->super = super;
    mount->root = super->root;
    mount->covered = covered;
    mount->flags = flags;
    mount->id = (i32)axfs_next_mount_serial();
    mount->mount_time = axfs_now();
    if (mount->id >= AXFS_MOUNT_MAX) {
        mount->id = g_mount_count;
    }

    /* Link into the tree by finding the nearest enclosing mount. */
    axfs_mount_t *parent = 0;
    usize best = 0;
    for (i32 i = 0; i < g_mount_count; i++) {
        axfs_mount_t *candidate = g_mounts[i];
        if (!candidate) {
            continue;
        }
        usize len = ax_strlen(candidate->path);
        if (len > best && ax_strncmp(normalised, candidate->path, len) == 0 &&
            (normalised[len] == '/' || normalised[len] == 0)) {
            best = len;
            parent = candidate;
        }
    }
    mount->parent = parent;
    if (parent) {
        mount->next = parent->children;
        parent->children = mount;
    }

    g_mounts[g_mount_count++] = mount;
    if (!g_root_mount && ax_strcmp(normalised, "/") == 0) {
        g_root_mount = mount;
    }
    if (super->mount == 0) {
        super->mount = mount;
    }
    if (super->root) {
        super->root->mount = mount;
    }
    g_dirty_gen++;
    return mount;
}

i32 axfs_mount_del(const char *path, u32 flags)
{
    axfs_mount_t *mount = axfs_mount_find(path);
    if (!mount || mount == g_root_mount) {
        return AXFS_EBUSY;
    }
    if (!same_point(mount->path, path)) {
        /* The caller asked for a path inside the mount, not its root. */
        return AXFS_EINVAL;
    }
    if (mount->children && (flags & AXFS_MNT_REMOUNT) == 0) {
        return AXFS_EBUSY;
    }
    /* Sync before detaching. */
    if (mount->super && mount->super->ops && mount->super->ops->sync) {
        mount->super->ops->sync(mount->super);
    }

    for (i32 i = 0; i < g_mount_count; i++) {
        if (g_mounts[i] == mount) {
            for (i32 j = i; j + 1 < g_mount_count; j++) {
                g_mounts[j] = g_mounts[j + 1];
            }
            g_mounts[g_mount_count - 1] = 0;
            g_mount_count--;
            break;
        }
    }
    if (mount->parent) {
        axfs_mount_t **slot = &mount->parent->children;
        while (*slot) {
            if (*slot == mount) {
                *slot = mount->next;
                break;
            }
            slot = &(*slot)->next;
        }
    }
    if (mount->super && mount->super->mount == mount) {
        mount->super->mount = mount->parent;
    }
    kfree(mount);
    g_dirty_gen++;
    return AXFS_OK;
}

/* Bind mount: the target path is covered by the same superblock and root as
 * the source, without copying any filesystem state. */
i32 axfs_mount_bind(const char *source, const char *target)
{
    axfs_mount_t *src = axfs_mount_find(source);
    if (!src || !src->super) {
        return AXFS_ENOENT;
    }
    if (same_point(source, target)) {
        return AXFS_EBUSY;
    }
    axfs_inode_t *covered = 0;
    axfs_lookup(g_root_mount, target, &covered, 0, 0);
    axfs_mount_t *mount = axfs_mount_add(target, src->super, src->flags, covered);
    if (!mount) {
        if (covered) {
            axfs_unref_inode(covered);
        }
        return AXFS_ENOMEM;
    }
    return AXFS_OK;
}

/* Remount with new flags; refuses to flip read-only on a superblock that has
 * dirty pages, because the filesystem may not be able to flush them. */
i32 axfs_remount(const char *path, u32 flags)
{
    axfs_mount_t *mount = axfs_mount_find(path);
    if (!mount) {
        return AXFS_ENOENT;
    }
    if ((flags & AXFS_MNT_RDONLY) && mount->super && mount->super->ops &&
        mount->super->ops->sync) {
        mount->super->ops->sync(mount->super);
    }
    u32 old = mount->flags;
    mount->flags = flags;
    if (mount->super && mount->super->ops && mount->super->ops->remount) {
        i32 rc = mount->super->ops->remount(mount->super, flags);
        if (rc != AXFS_OK) {
            mount->flags = old;
            return rc;
        }
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Enumeration and diagnostics                                         */
/* ------------------------------------------------------------------ */

axfs_mount_t *axfs_mount_at(i32 index)
{
    if (index < 0 || index >= g_mount_count) {
        return 0;
    }
    return g_mounts[index];
}

/* Format one mount as a line for `mount` output. */
void axfs_mount_format(i32 index, char *buffer, usize cap)
{
    axfs_mount_t *mount = axfs_mount_at(index);
    if (!mount || !buffer) {
        return;
    }
    const char *fs_name = mount->super && mount->super->name ? mount->super->name : "?";
    char flags[16];
    i32 used = 0;
    flags[used++] = (mount->flags & AXFS_MNT_RDONLY) ? 'r' : 'w';
    if (mount->super && mount->super->read_only) {
        flags[used - 1] = 'R';
    }
    if (mount->super && (mount->super->features & AXFS_F_JOURNAL)) {
        flags[used++] = 'j';
    }
    if (mount->flags & AXFS_MNT_NODEV) {
        flags[used++] = 'd';
    }
    if (mount->flags & AXFS_MNT_NOEXEC) {
        flags[used++] = 'x';
    }
    flags[used] = 0;
    ax_snprintf(buffer, cap, "%-12s %-8s %s", mount->path, fs_name, flags);
}

void axfs_mount_dump(void)
{
    printk("mounts: %d\n", g_mount_count);
    for (i32 i = 0; i < g_mount_count; i++) {
        char line[64];
        axfs_mount_format(i, line, sizeof(line));
        printk("  %s\n", line);
    }
}

/* ------------------------------------------------------------------ */
/* Tree walking                                                        */
/* ------------------------------------------------------------------ */

/* Depth of a mount in the tree; used to decide whether an unmount would
 * orphan a child mount. */
i32 axfs_mount_depth(axfs_mount_t *mount)
{
    i32 depth = 0;
    while (mount && mount->parent) {
        depth++;
        mount = mount->parent;
    }
    return depth;
}

/* True when `child` is below `ancestor`. */
bool_t axfs_mount_is_below(axfs_mount_t *child, axfs_mount_t *ancestor)
{
    axfs_mount_t *walk = child;
    while (walk) {
        if (walk == ancestor) {
            return (bool_t)1;
        }
        walk = walk->parent;
    }
    return (bool_t)0;
}

/* Sync every mount.  Called on shutdown and by the `sync` shell command. */
i32 axfs_mount_sync_all(void)
{
    i32 failures = 0;
    for (i32 i = 0; i < g_mount_count; i++) {
        axfs_mount_t *mount = g_mounts[i];
        if (mount && mount->super && mount->super->ops && mount->super->ops->sync) {
            if (mount->super->ops->sync(mount->super) != AXFS_OK) {
                failures++;
            }
        }
    }
    return failures;
}
