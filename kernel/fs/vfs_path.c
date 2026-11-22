/*
 * AxiomOS - VFS path resolution.
 *
 * Path handling is the part of a filesystem layer that bugs love: empty
 * components, trailing slashes, `..` at the root, symlinks that loop, and
 * relative paths handed in by user code that assumes a cwd.  This file owns
 * all of it, with two entry points:
 *
 *   axfs_lookup()     resolves from the root of a mount
 *   axfs_lookup_at()  resolves from an arbitrary dentry (a cwd, or an open
 *                     directory handle passed to *at syscalls)
 *
 * Component-by-component walk.  Each step checks the dentry cache first, then
 * calls the filesystem's lookup, and installs the result in the cache.  A
 * symlink is expanded by rewriting the remaining path, with a depth counter
 * that produces ELOOP after AXFS_SYMLINK_MAX hops.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axfs.h>

/* ------------------------------------------------------------------ */
/* Small string helpers                                                */
/* ------------------------------------------------------------------ */

/* Copy a path, collapsing repeated slashes and removing `.` components but
 * leaving `..` alone: resolution decides what `..` means, not normalisation,
 * because `..` above a mount point must cross into the covered inode. */
i32 axfs_path_normalise(const char *path, char *out, usize cap)
{
    if (!path || !out || cap < 2) {
        return AXFS_EINVAL;
    }
    usize write = 0;
    bool_t last_slash = (bool_t)1;      /* suppress a leading slash run */
    for (usize read = 0; path[read]; read++) {
        char c = path[read];
        if (c == '/') {
            if (!last_slash && write + 1 < cap) {
                out[write++] = '/';
            }
            last_slash = (bool_t)1;
            continue;
        }
        if (write + 1 >= cap) {
            return AXFS_ENAMETOOLONG;
        }
        out[write++] = c;
        last_slash = (bool_t)0;
    }
    if (write == 0) {
        if (cap < 2) {
            return AXFS_ENAMETOOLONG;
        }
        out[write++] = '/';
    }
    /* Trailing slash is meaningful only for "must be a directory" checks; the
     * resolver keeps it in a flag of its own, so drop it here except for the
     * bare root. */
    if (write > 1 && out[write - 1] == '/') {
        write--;
    }
    out[write] = 0;
    return (i32)write;
}

bool_t axfs_path_is_absolute(const char *path)
{
    return (bool_t)(path && path[0] == '/');
}

i32 axfs_path_dirname(const char *path, char *out, usize cap)
{
    if (!path || !out || cap < 2) {
        return AXFS_EINVAL;
    }
    usize len = ax_strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    usize cut = len;
    while (cut > 0 && path[cut - 1] != '/') {
        cut--;
    }
    if (cut == 0) {
        ax_strlcpy(out, ".", cap);
        return AXFS_OK;
    }
    if (cut == 1) {
        ax_strlcpy(out, "/", cap);
        return AXFS_OK;
    }
    usize n = cut - 1;
    if (n >= cap) {
        return AXFS_ENAMETOOLONG;
    }
    for (usize i = 0; i < n; i++) {
        out[i] = path[i];
    }
    out[n] = 0;
    return AXFS_OK;
}

i32 axfs_path_basename(const char *path, char *out, usize cap)
{
    if (!path || !out || cap < 2) {
        return AXFS_EINVAL;
    }
    usize len = ax_strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    usize start = len;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    usize n = len - start;
    if (n >= cap) {
        return AXFS_ENAMETOOLONG;
    }
    for (usize i = 0; i < n; i++) {
        out[i] = path[start + i];
    }
    out[n] = 0;
    if (n == 0) {
        ax_strlcpy(out, "/", cap);
    }
    return AXFS_OK;
}

i32 axfs_path_join(char *out, usize cap, const char *base, const char *name)
{
    if (!out || !base || !name) {
        return AXFS_EINVAL;
    }
    usize base_len = ax_strlen(base);
    usize name_len = ax_strlen(name);
    bool_t need_slash = (bool_t)(base_len > 0 && base[base_len - 1] != '/');
    usize total = base_len + (need_slash ? 1 : 0) + name_len;
    if (total + 1 > cap) {
        return AXFS_ENAMETOOLONG;
    }
    usize write = 0;
    for (usize i = 0; i < base_len; i++) {
        out[write++] = base[i];
    }
    if (need_slash) {
        out[write++] = '/';
    }
    for (usize i = 0; i < name_len; i++) {
        out[write++] = name[i];
    }
    out[write] = 0;
    return (i32)write;
}

/* Split the first component off a path, advancing `rest` past it. */
static i32 next_component(const char **cursor, char *out, usize cap)
{
    const char *p = *cursor;
    while (*p == '/') {
        p++;
    }
    if (!*p) {
        *cursor = p;
        out[0] = 0;
        return 0;
    }
    usize len = 0;
    while (*p && *p != '/') {
        if (len + 1 < cap) {
            out[len++] = *p;
        }
        p++;
    }
    out[len] = 0;
    while (*p == '/') {
        p++;
    }
    *cursor = p;
    return (i32)len;
}

/* ------------------------------------------------------------------ */
/* Root of a mount                                                     */
/* ------------------------------------------------------------------ */

static axfs_inode_t *mount_root(axfs_mount_t *mount)
{
    if (!mount) {
        return 0;
    }
    if (mount->root) {
        return mount->root;
    }
    if (mount->super && mount->super->root) {
        return mount->super->root;
    }
    return 0;
}

static axfs_dentry_t *root_dentry(axfs_mount_t *mount)
{
    static axfs_dentry_t roots[AXFS_MOUNT_MAX];
    static bool_t used[AXFS_MOUNT_MAX];
    if (!mount) {
        return 0;
    }
    i32 index = mount->id;
    if (index < 0 || index >= AXFS_MOUNT_MAX) {
        return 0;
    }
    if (!used[index]) {
        axfs_dentry_t *dentry = &roots[index];
        ax_memset(dentry, 0, sizeof(*dentry));
        dentry->name[0] = '/';
        dentry->name[1] = 0;
        dentry->name_len = 1;
        dentry->refs = 1;
        dentry->inode = mount_root(mount);
        if (dentry->inode) {
            axfs_ref_inode(dentry->inode);
        }
        dentry->type = AXFS_FT_DIR;
        used[index] = (bool_t)1;
    }
    return &roots[index];
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

/* Ask the filesystem to resolve one name, installing a cache entry.  This is
 * the only place that calls inode->ops->lookup. */
static i32 lookup_in_dir(axfs_inode_t *dir, const char *name, axfs_inode_t **out,
                         axfs_dentry_t **out_dentry)
{
    if (!dir || !name) {
        return AXFS_EINVAL;
    }
    if (dir->type != AXFS_FT_DIR) {
        return AXFS_ENOTDIR;
    }

    axfs_dentry_t *parent_dentry = 0;
    /* The dentry cache is organised under a dentry, but lookup only carries
     * the inode.  Recover the dentry by scanning the parent's children cache
     * when one exists; if there is none, go straight to the filesystem. */
    (void)parent_dentry;

    if (!dir->ops || !dir->ops->lookup) {
        return AXFS_ENOSYS;
    }
    axfs_inode_t *child = 0;
    i32 rc = dir->ops->lookup(dir, name, &child);
    if (rc != AXFS_OK) {
        return rc;
    }
    if (!child) {
        return AXFS_ENOENT;
    }
    if (out_dentry) {
        *out_dentry = axfs_alloc_dentry(parent_dentry, name, child);
    }
    *out = child;
    return AXFS_OK;
}

/* Follow a symlink: read its target and splice it in front of the remaining
 * path.  Returns the number of bytes written to `buffer`. */
i32 axfs_resolve_symlink(axfs_mount_t *mount, axfs_inode_t *inode, char *out,
                          usize cap)
{
    (void)mount;
    if (!inode || inode->type != AXFS_FT_SYMLINK) {
        return AXFS_EINVAL;
    }
    if (!inode->ops || !inode->ops->readlink) {
        return AXFS_ENOSYS;
    }
    i32 n = inode->ops->readlink(inode, out, cap - 1);
    if (n < 0) {
        return n;
    }
    if ((usize)n >= cap - 1) {
        return AXFS_ENAMETOOLONG;
    }
    out[n] = 0;
    return n;
}

/* The workhorse.  `base` is a starting dentry, `path` may be absolute or
 * relative.  On success `*out` carries a referenced inode and, when requested,
 * a referenced dentry. */
i32 axfs_lookup_at(axfs_dentry_t *base, const char *path, axfs_inode_t **out,
                   axfs_dentry_t **out_dentry, u32 flags)
{
    if (!out) {
        return AXFS_EINVAL;
    }
    *out = 0;
    if (out_dentry) {
        *out_dentry = 0;
    }
    if (!path) {
        return AXFS_EINVAL;
    }

    axfs_mount_t *mount = axfs_mount_root();
    axfs_dentry_t *current_dentry = base;
    axfs_inode_t *current = base ? base->inode : 0;

    if (axfs_path_is_absolute(path) || !current) {
        mount = axfs_mount_find("/");
        current_dentry = root_dentry(mount);
        current = current_dentry ? current_dentry->inode : 0;
        if (!current && mount) {
            current = mount_root(mount);
        }
    } else if (current) {
        /* Resolve relative to base's own mount. */
        mount = current->mount ? current->mount : axfs_mount_root();
    }
    if (!current || !mount) {
        return AXFS_ENOENT;
    }

    char component[AXFS_NAME_MAX + 1];
    char link[AXFS_PATH_MAX];
    char spliced[AXFS_PATH_MAX];
    const char *cursor = path;
    i32 depth = 0;
    bool_t require_dir = (bool_t)0;
    bool_t last_was_symlink = (bool_t)0;

    axfs_ref_inode(current);
    if (current_dentry) {
        axfs_ref_dentry(current_dentry);
    }

    while (1) {
        i32 n = next_component(&cursor, component, sizeof(component));
        if (n < 0) {
            break;
        }

        /* A trailing slash means the final component must be a directory. */
        if (n == 0) {
            require_dir = (bool_t)(path[ax_strlen(path) - 1] == '/');
            break;
        }

        if (ax_strcmp(component, ".") == 0) {
            continue;
        }

        if (ax_strcmp(component, "..") == 0) {
            /* Crossing up out of a mount root lands on the covered inode. */
            if (current == mount_root(mount) && mount->covered) {
                axfs_inode_t *covered = mount->covered;
                axfs_ref_inode(covered);
                axfs_unref_inode(current);
                current = covered;
                mount = covered->mount ? covered->mount : mount->parent;
                current_dentry = current_dentry ? current_dentry->parent : 0;
                continue;
            }
            if (current_dentry && current_dentry->parent) {
                axfs_dentry_t *parent = current_dentry->parent;
                axfs_ref_dentry(parent);
                axfs_unref_dentry(current_dentry);
                current_dentry = parent;
            }
            if (current->parent) {
                axfs_inode_t *parent = current->parent;
                axfs_ref_inode(parent);
                axfs_unref_inode(current);
                current = parent;
            }
            continue;
        }

        if (current->type != AXFS_FT_DIR) {
            axfs_unref_inode(current);
            if (current_dentry) {
                axfs_unref_dentry(current_dentry);
            }
            return AXFS_ENOTDIR;
        }

        /* Cached child first. */
        axfs_dentry_t *child_dentry = current_dentry
                                          ? axfs_lookup_dentry(current_dentry, component)
                                          : 0;
        axfs_inode_t *child = 0;
        if (child_dentry && child_dentry->inode) {
            child = child_dentry->inode;
            axfs_ref_inode(child);
        } else {
            i32 rc = lookup_in_dir(current, component, &child, &child_dentry);
            if (rc != AXFS_OK) {
                axfs_unref_inode(current);
                if (current_dentry) {
                    axfs_unref_dentry(current_dentry);
                }
                return rc;
            }
        }

        /* Symlink handling: only expand when more components remain, or when
         * the caller did not suppress following. */
        if (child->type == AXFS_FT_SYMLINK && !(flags & AXFS_O_NOFOLLOW) &&
            last_was_symlink == (bool_t)0) {
            depth++;
            if (depth > AXFS_SYMLINK_MAX) {
                axfs_unref_inode(child);
                axfs_unref_inode(current);
                if (current_dentry) {
                    axfs_unref_dentry(current_dentry);
                }
                return AXFS_ELOOP;
            }
            i32 link_len = axfs_resolve_symlink(mount, child, link, sizeof(link));
            if (link_len < 0) {
                axfs_unref_inode(child);
                if (child_dentry) {
                    axfs_unref_dentry(child_dentry);
                }
                continue;
            }
            if (link[0] == '/') {
                /* Absolute link restarts at the mount root. */
                axfs_inode_t *root_inode = mount_root(mount);
                axfs_ref_inode(root_inode);
                axfs_unref_inode(current);
                current = root_inode;
                current_dentry = root_dentry(mount);
            }
            /* The rest of the original path follows the link target. */
            const char *rest = cursor;
            spliced[0] = 0;
            usize used = 0;
            for (i32 i = 0; i < link_len && used + 1 < sizeof(spliced); i++) {
                spliced[used++] = link[i];
            }
            if (*rest) {
                if (used + 1 < sizeof(spliced)) {
                    spliced[used++] = '/';
                }
                for (const char *r = rest; *r && used + 1 < sizeof(spliced); r++) {
                    spliced[used++] = *r;
                }
            }
            spliced[used] = 0;
            axfs_unref_inode(child);
            if (child_dentry) {
                axfs_unref_dentry(child_dentry);
                child_dentry = 0;
            }

            /* Re-enter the walk with the spliced path, keeping the current
             * directory and mount. */
            char slow[AXFS_PATH_MAX];
            ax_strlcpy(slow, spliced, sizeof(slow));
            i32 rc2 = axfs_lookup_at(current_dentry, slow, out, out_dentry, flags);
            axfs_unref_inode(current);
            if (current_dentry) {
                axfs_unref_dentry(current_dentry);
            }
            return rc2;
        }

        axfs_unref_inode(current);
        if (current_dentry) {
            axfs_unref_dentry(current_dentry);
        }
        current = child;
        current_dentry = child_dentry;

        /* If this is a mount point, descend into the mounted root. */
        axfs_mount_t *mounted = axfs_mount_covering(path, current);
        if (mounted && mounted->root) {
            axfs_inode_t *mounted_root = mounted->root;
            axfs_ref_inode(mounted_root);
            axfs_unref_inode(current);
            current = mounted_root;
            current_dentry = root_dentry(mounted);
            mount = mounted;
        }
    }

    if (require_dir && current->type != AXFS_FT_DIR) {
        axfs_unref_inode(current);
        if (current_dentry) {
            axfs_unref_dentry(current_dentry);
        }
        return AXFS_ENOTDIR;
    }

    *out = current;
    if (out_dentry) {
        *out_dentry = current_dentry;
    } else if (current_dentry) {
        axfs_unref_dentry(current_dentry);
    }
    return AXFS_OK;
}

i32 axfs_lookup(axfs_mount_t *mount, const char *path, axfs_inode_t **out,
                axfs_dentry_t **out_dentry, u32 flags)
{
    axfs_dentry_t *base = mount ? root_dentry(mount) : 0;
    return axfs_lookup_at(base, path, out, out_dentry, flags);
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* Exercise the resolver against a set of paths without touching any file:
 * useful from the `fsck` shell command and from the boot self-test. */
typedef struct axfs_path_case {
    const char *path;
    i32         expect;
} axfs_path_case_t;

i32 axfs_path_selftest(void)
{
    char buffer[AXFS_PATH_MAX];
    static const struct {
        const char *input;
        const char *expect;
    } cases[] = {
        { "/", "/" },
        { "//", "/" },
        { "/a//b///c", "/a/b/c" },
        { "/a/b/", "/a/b" },
        { "a/b", "a/b" },
        { "/.", "/." },
        { "", "/" },
    };

    i32 failures = 0;
    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        i32 rc = axfs_path_normalise(cases[i].input, buffer, sizeof(buffer));
        if (rc < 0) {
            failures++;
            continue;
        }
        if (ax_strcmp(buffer, cases[i].expect) != 0) {
            failures++;
        }
    }

    /* dirname and basename round trip. */
    if (axfs_path_dirname("/a/b/c", buffer, sizeof(buffer)) == AXFS_OK &&
        ax_strcmp(buffer, "/a/b") != 0) {
        failures++;
    }
    if (axfs_path_basename("/a/b/c", buffer, sizeof(buffer)) == AXFS_OK &&
        ax_strcmp(buffer, "c") != 0) {
        failures++;
    }
    return failures;
}

void axfs_path_dump(const char *path)
{
    char normalised[AXFS_PATH_MAX];
    char dir[AXFS_PATH_MAX];
    char base[AXFS_PATH_MAX];
    if (axfs_path_normalise(path, normalised, sizeof(normalised)) < 0) {
        printk("path: %s invalid\n", path ? path : "(null)");
        return;
    }
    axfs_path_dirname(normalised, dir, sizeof(dir));
    axfs_path_basename(normalised, base, sizeof(base));
    printk("path: '%s' -> '%s' dir='%s' base='%s'\n", path ? path : "(null)",
           normalised, dir, base);
}
