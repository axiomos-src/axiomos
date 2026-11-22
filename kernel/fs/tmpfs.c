/*
 * AxiomOS - tmpfs.
 *
 * An in-memory filesystem used for the root, for /tmp, and for any scratch
 * space the shell needs.  Two backends share this code:
 *
 *   - unlimited: `tmpfs_create(name, 0)` grows until kmalloc fails.  This is
 *     the root filesystem, which holds a few hundred kilobytes at most.
 *   - bounded:  a byte budget is enforced on write, so /tmp cannot starve the
 *     kernel heap.
 *
 * Data is kept in a single flat buffer per file and grown by doubling, a
 * deliberate trade: files here are small (shell output, config), and one
 * buffer means a read is a memcpy rather than a page walk.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axfs.h>

typedef struct tmp_node {
    char          *name;        /* NULL for the root */
    axfs_inode_t  *inode;       /* back pointer */
    struct tmp_node *parent;
    struct tmp_node *children;  /* sibling list */
    struct tmp_node *next;      /* next sibling under parent */
    u8            *data;
    u64            capacity;
    u64            used;
    char          *link;        /* symlink target, NULL otherwise */
    u64            rdev;
} tmp_node_t;

typedef struct tmp_super {
    const char *name;
    u64         budget;         /* 0 = unlimited */
    u64         used;
    u64         files;
    u64         next_ino;
    tmp_node_t *root;
} tmp_super_t;

static u64 g_tmpfs_total_used;

/* ------------------------------------------------------------------ */
/* Node helpers                                                        */
/* ------------------------------------------------------------------ */

static tmp_node_t *tmp_new_node(tmp_super_t *super, tmp_node_t *parent,
                                const char *name, u8 type, u32 mode)
{
    tmp_node_t *node = (tmp_node_t *)kzalloc(sizeof(*node));
    if (!node) {
        return 0;
    }
    if (name) {
        usize len = ax_strlen(name);
        node->name = (char *)kmalloc(len + 1);
        if (!node->name) {
            kfree(node);
            return 0;
        }
        ax_strlcpy(node->name, name, len + 1);
    }
    node->parent = parent;

    axfs_super_t *super_block = (axfs_super_t *)super->root;   /* unused */
    (void)super_block;

    node->inode = axfs_alloc_inode(0, super->next_ino++, type, mode);
    if (!node->inode) {
        if (node->name) {
            kfree(node->name);
        }
        kfree(node);
        return 0;
    }
    node->inode->fs_data = node;
    node->inode->size = 0;
    node->inode->mode = mode | (type == AXFS_FT_DIR ? AXFS_S_IFDIR
                               : type == AXFS_FT_SYMLINK ? AXFS_S_IFLNK
                                                          : AXFS_S_IFREG);
    super->files++;
    return node;
}

static void tmp_free_node(tmp_super_t *super, tmp_node_t *node)
{
    if (!node) {
        return;
    }
    /* Children first. */
    tmp_node_t *child = node->children;
    while (child) {
        tmp_node_t *next = child->next;
        tmp_free_node(super, child);
        child = next;
    }
    if (node->data) {
        super->used -= node->used;
        if (g_tmpfs_total_used >= node->used) {
            g_tmpfs_total_used -= node->used;
        }
        kfree(node->data);
    }
    if (node->link) {
        kfree(node->link);
    }
    if (node->inode) {
        node->inode->fs_data = 0;
        node->inode->flags |= AXFS_I_DELETED;
    }
    if (node->name) {
        kfree(node->name);
    }
    if (super->files > 0) {
        super->files--;
    }
    kfree(node);
}

static tmp_node_t *tmp_child(tmp_node_t *dir, const char *name)
{
    if (!dir || !name) {
        return 0;
    }
    for (tmp_node_t *child = dir->children; child; child = child->next) {
        if (child->name && ax_strcmp(child->name, name) == 0) {
            return child;
        }
    }
    return 0;
}

static void tmp_add_child(tmp_node_t *dir, tmp_node_t *child)
{
    child->next = dir->children;
    dir->children = child;
    child->parent = dir;
}

static void tmp_detach_child(tmp_node_t *dir, tmp_node_t *child)
{
    tmp_node_t **slot = &dir->children;
    while (*slot) {
        if (*slot == child) {
            *slot = child->next;
            child->next = 0;
            return;
        }
        slot = &(*slot)->next;
    }
}

/* Grow the data buffer to hold `size` bytes.  Returns false when the budget
 * or the allocator refuses. */
static bool_t tmp_reserve(tmp_super_t *super, tmp_node_t *node, u64 size)
{
    if (size <= node->capacity) {
        return (bool_t)1;
    }
    if (super->budget) {
        u64 extra = size - node->capacity;
        if (super->used + extra > super->budget) {
            return (bool_t)0;
        }
    }
    u64 capacity = node->capacity ? node->capacity : 4096;
    while (capacity < size) {
        capacity *= 2;
        if (capacity > 64ull * 1024 * 1024) {
            capacity = size;
            break;
        }
    }
    u8 *data = (u8 *)kmalloc(capacity);
    if (!data) {
        return (bool_t)0;
    }
    ax_memset(data, 0, capacity);
    if (node->data) {
        for (u64 i = 0; i < node->used; i++) {
            data[i] = node->data[i];
        }
        kfree(node->data);
    }
    node->data = data;
    node->capacity = capacity;
    return (bool_t)1;
}

/* Account a size change against the budget. */
static i32 tmp_account(tmp_super_t *super, tmp_node_t *node, u64 size)
{
    if (!tmp_reserve(super, node, size)) {
        return AXFS_ENOSPC;
    }
    if (size > node->used) {
        u64 extra = size - node->used;
        super->used += extra;
        g_tmpfs_total_used += extra;
    } else {
        u64 shrink = node->used - size;
        super->used -= shrink;
        if (g_tmpfs_total_used >= shrink) {
            g_tmpfs_total_used -= shrink;
        }
    }
    node->used = size;
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Inode ops                                                           */
/* ------------------------------------------------------------------ */

static tmp_super_t *super_of(axfs_inode_t *inode)
{
    if (!inode || !inode->super) {
        return 0;
    }
    return (tmp_super_t *)inode->super->fs_data;
}

static tmp_node_t *node_of(axfs_inode_t *inode)
{
    return inode ? (tmp_node_t *)inode->fs_data : 0;
}

static i32 tmp_lookup(axfs_inode_t *dir, const char *name, axfs_inode_t **out)
{
    tmp_node_t *node = node_of(dir);
    if (!node) {
        return AXFS_EIO;
    }
    tmp_node_t *child = tmp_child(node, name);
    if (!child || !child->inode) {
        return AXFS_ENOENT;
    }
    axfs_ref_inode(child->inode);
    *out = child->inode;
    return AXFS_OK;
}

static i32 tmp_create(axfs_inode_t *dir, const char *name, u32 mode,
                      axfs_inode_t **out)
{
    tmp_super_t *super = super_of(dir);
    tmp_node_t *parent = node_of(dir);
    if (!super || !parent) {
        return AXFS_EIO;
    }
    if (axfs_name_valid(name) != AXFS_OK) {
        return AXFS_EINVAL;
    }
    if (tmp_child(parent, name)) {
        return AXFS_EEXIST;
    }
    tmp_node_t *node = tmp_new_node(super, parent, name, AXFS_FT_FILE, mode);
    if (!node) {
        return AXFS_ENOMEM;
    }
    axfs_super_t *sb = dir->super;
    node->inode->super = sb;
    node->inode->mount = dir->mount;
    node->inode->ops = dir->ops;
    tmp_add_child(parent, node);
    axfs_ref_inode(node->inode);
    *out = node->inode;
    return AXFS_OK;
}

static i32 tmp_mkdir(axfs_inode_t *dir, const char *name, u32 mode,
                     axfs_inode_t **out)
{
    tmp_super_t *super = super_of(dir);
    tmp_node_t *parent = node_of(dir);
    if (!super || !parent) {
        return AXFS_EIO;
    }
    if (axfs_name_valid(name) != AXFS_OK) {
        return AXFS_EINVAL;
    }
    if (tmp_child(parent, name)) {
        return AXFS_EEXIST;
    }
    tmp_node_t *node = tmp_new_node(super, parent, name, AXFS_FT_DIR, mode);
    if (!node) {
        return AXFS_ENOMEM;
    }
    node->inode->super = dir->super;
    node->inode->mount = dir->mount;
    node->inode->ops = dir->ops;
    node->inode->nlink = 2;
    tmp_add_child(parent, node);
    axfs_ref_inode(node->inode);
    if (out) {
        *out = node->inode;
    } else {
        axfs_unref_inode(node->inode);
    }
    return AXFS_OK;
}

static i32 tmp_unlink(axfs_inode_t *dir, const char *name)
{
    tmp_super_t *super = super_of(dir);
    tmp_node_t *parent = node_of(dir);
    if (!super || !parent) {
        return AXFS_EIO;
    }
    tmp_node_t *child = tmp_child(parent, name);
    if (!child) {
        return AXFS_ENOENT;
    }
    if (child->inode && child->inode->type == AXFS_FT_DIR) {
        return AXFS_EISDIR;
    }
    tmp_detach_child(parent, child);
    tmp_free_node(super, child);
    return AXFS_OK;
}

static i32 tmp_rmdir(axfs_inode_t *dir, const char *name)
{
    tmp_super_t *super = super_of(dir);
    tmp_node_t *parent = node_of(dir);
    if (!super || !parent) {
        return AXFS_EIO;
    }
    tmp_node_t *child = tmp_child(parent, name);
    if (!child) {
        return AXFS_ENOENT;
    }
    if (!child->inode || child->inode->type != AXFS_FT_DIR) {
        return AXFS_ENOTDIR;
    }
    if (child->children) {
        return AXFS_ENOTEMPTY;
    }
    tmp_detach_child(parent, child);
    tmp_free_node(super, child);
    return AXFS_OK;
}

static i32 tmp_rename(axfs_inode_t *old_dir, const char *old_name,
                      axfs_inode_t *new_dir, const char *new_name, u32 flags)
{
    (void)flags;
    tmp_node_t *source_dir = node_of(old_dir);
    tmp_node_t *target_dir = node_of(new_dir);
    if (!source_dir || !target_dir) {
        return AXFS_EIO;
    }
    tmp_node_t *child = tmp_child(source_dir, old_name);
    if (!child) {
        return AXFS_ENOENT;
    }
    /* Replacing an existing target is allowed for files, refused for dirs. */
    tmp_node_t *existing = tmp_child(target_dir, new_name);
    if (existing && existing != child) {
        if (child->inode->type == AXFS_FT_DIR && existing->children) {
            return AXFS_ENOTEMPTY;
        }
        tmp_detach_child(target_dir, existing);
        tmp_free_node(super_of(new_dir), existing);
    }
    tmp_detach_child(source_dir, child);
    char *name = (char *)kmalloc(ax_strlen(new_name) + 1);
    if (!name) {
        return AXFS_ENOMEM;
    }
    ax_strlcpy(name, new_name, ax_strlen(new_name) + 1);
    if (child->name) {
        kfree(child->name);
    }
    child->name = name;
    tmp_add_child(target_dir, child);
    if (child->inode) {
        child->inode->mount = new_dir->mount;
        axfs_mark_dirty(child->inode);
    }
    return AXFS_OK;
}

static i32 tmp_readlink(axfs_inode_t *inode, char *buffer, usize cap)
{
    tmp_node_t *node = node_of(inode);
    if (!node || !node->link) {
        return AXFS_EINVAL;
    }
    usize len = ax_strlen(node->link);
    if (len >= cap) {
        len = cap - 1;
    }
    for (usize i = 0; i < len; i++) {
        buffer[i] = node->link[i];
    }
    buffer[len] = 0;
    return (i32)len;
}

static i32 tmp_symlink(axfs_inode_t *dir, const char *name, const char *target)
{
    tmp_super_t *super = super_of(dir);
    tmp_node_t *parent = node_of(dir);
    if (!super || !parent || !target) {
        return AXFS_EIO;
    }
    if (tmp_child(parent, name)) {
        return AXFS_EEXIST;
    }
    tmp_node_t *node = tmp_new_node(super, parent, name, AXFS_FT_SYMLINK, 0777);
    if (!node) {
        return AXFS_ENOMEM;
    }
    node->inode->super = dir->super;
    node->inode->mount = dir->mount;
    node->inode->ops = dir->ops;
    usize len = ax_strlen(target);
    node->link = (char *)kmalloc(len + 1);
    if (!node->link) {
        tmp_free_node(super, node);
        return AXFS_ENOMEM;
    }
    ax_strlcpy(node->link, target, len + 1);
    node->inode->size = len;
    tmp_add_child(parent, node);
    return AXFS_OK;
}

static i32 tmp_link(axfs_inode_t *inode, axfs_inode_t *dir, const char *name)
{
    tmp_super_t *super = super_of(dir);
    tmp_node_t *parent = node_of(dir);
    tmp_node_t *source = node_of(inode);
    if (!super || !parent || !source) {
        return AXFS_EIO;
    }
    if (axfs_name_valid(name) != AXFS_OK) {
        return AXFS_EINVAL;
    }
    if (tmp_child(parent, name)) {
        return AXFS_EEXIST;
    }
    /* mknod with a raw copy keeps hard links trivial. */
    return AXFS_EINVAL;
}

static i32 tmp_mknod(axfs_inode_t *dir, const char *name, u32 mode, u64 rdev,
                     axfs_inode_t **out)
{
    tmp_super_t *super = super_of(dir);
    tmp_node_t *parent = node_of(dir);
    if (!super || !parent) {
        return AXFS_EIO;
    }
    if (tmp_child(parent, name)) {
        return AXFS_EEXIST;
    }
    u8 type = AXFS_FT_FILE;
    u32 perm = mode & 0777;
    switch (mode & AXFS_S_IFMT) {
    case AXFS_S_IFCHR:
        type = AXFS_FT_CHAR;
        break;
    case AXFS_S_IFBLK:
        type = AXFS_FT_BLOCK;
        break;
    case AXFS_S_IFIFO:
        type = AXFS_FT_FIFO;
        break;
    case AXFS_S_IFSOCK:
        type = AXFS_FT_SOCKET;
        break;
    default:
        break;
    }
    tmp_node_t *node = tmp_new_node(super, parent, name, type, perm);
    if (!node) {
        return AXFS_ENOMEM;
    }
    node->rdev = rdev;
    node->inode->super = dir->super;
    node->inode->mount = dir->mount;
    node->inode->ops = dir->ops;
    node->inode->mode = mode;
    tmp_add_child(parent, node);
    axfs_ref_inode(node->inode);
    if (out) {
        *out = node->inode;
    } else {
        axfs_unref_inode(node->inode);
    }
    return AXFS_OK;
}

static i64 tmp_read(axfs_file_t *file, void *buffer, u64 count)
{
    tmp_node_t *node = node_of(file->inode);
    if (!node || !buffer) {
        return AXFS_EINVAL;
    }
    if (!node->data) {
        return 0;
    }
    u64 remaining = node->used > file->offset ? node->used - file->offset : 0;
    if (count > remaining) {
        count = remaining;
    }
    for (u64 i = 0; i < count; i++) {
        ((u8 *)buffer)[i] = node->data[file->offset + i];
    }
    return (i64)count;
}

static i64 tmp_write(axfs_file_t *file, const void *buffer, u64 count)
{
    tmp_super_t *super = super_of(file->inode);
    tmp_node_t *node = node_of(file->inode);
    if (!node || !super || !buffer) {
        return AXFS_EINVAL;
    }
    u64 end = file->offset + count;
    u64 target = end > node->used ? end : node->used;
    i32 rc = tmp_account(super, node, target);
    if (rc != AXFS_OK) {
        return rc;
    }
    for (u64 i = 0; i < count; i++) {
        node->data[file->offset + i] = ((const u8 *)buffer)[i];
    }
    return (i64)count;
}

static i32 tmp_readdir(axfs_inode_t *dir, u64 index, axfs_dirent_t *out)
{
    tmp_node_t *node = node_of(dir);
    if (!node || !out) {
        return AXFS_EINVAL;
    }
    if (index == 0) {
        out->ino = dir->ino;
        out->type = AXFS_FT_DIR;
        ax_strlcpy(out->name, ".", sizeof(out->name));
        out->name_len = 1;
        return AXFS_OK;
    }
    if (index == 1) {
        out->ino = node->parent && node->parent->inode ? node->parent->inode->ino
                                                       : dir->ino;
        out->type = AXFS_FT_DIR;
        ax_strlcpy(out->name, "..", sizeof(out->name));
        out->name_len = 2;
        return AXFS_OK;
    }
    tmp_node_t *child = node->children;
    for (u64 i = 2; child && i < index; i++) {
        child = child->next;
    }
    if (!child) {
        return AXFS_ENOENT;
    }
    out->ino = child->inode ? child->inode->ino : 0;
    out->type = child->inode ? child->inode->type : AXFS_FT_UNKNOWN;
    ax_strlcpy(out->name, child->name ? child->name : "", sizeof(out->name));
    out->name_len = (u16)ax_strlen(out->name);
    return AXFS_OK;
}

static i32 tmp_truncate(axfs_inode_t *inode, u64 size)
{
    tmp_super_t *super = super_of(inode);
    tmp_node_t *node = node_of(inode);
    if (!node || !super) {
        return AXFS_EIO;
    }
    i32 rc = tmp_account(super, node, size);
    if (rc != AXFS_OK) {
        return rc;
    }
    for (u64 i = size; i < node->capacity; i++) {
        node->data[i] = 0;
    }
    inode->size = size;
    axfs_page_invalidate(inode, 0, ~0ull);
    axfs_mark_dirty(inode);
    return AXFS_OK;
}

static i32 tmp_stat(axfs_inode_t *inode, axfs_attr_t *out)
{
    if (!inode || !out) {
        return AXFS_EINVAL;
    }
    tmp_node_t *node = node_of(inode);
    out->mode = inode->mode;
    out->uid = inode->uid;
    out->gid = inode->gid;
    out->size = inode->size;
    out->blocks = node ? (node->used + 511) / 512 : 0;
    out->ino = inode->ino;
    out->atime = inode->atime;
    out->mtime = inode->mtime;
    out->ctime = inode->ctime;
    out->nlink = inode->nlink;
    out->type = inode->type;
    if (node && node->rdev) {
        out->blocks = node->rdev;
    }
    return AXFS_OK;
}

static i32 tmp_chmod(axfs_inode_t *inode, u32 mode)
{
    if (!inode) {
        return AXFS_EINVAL;
    }
    inode->mode = (inode->mode & AXFS_S_IFMT) | (mode & 07777);
    inode->ctime = axfs_now();
    axfs_mark_dirty(inode);
    return AXFS_OK;
}

static i32 tmp_chown(axfs_inode_t *inode, u32 uid, u32 gid)
{
    if (!inode) {
        return AXFS_EINVAL;
    }
    if (uid != (u32)-1) {
        inode->uid = uid;
    }
    if (gid != (u32)-1) {
        inode->gid = gid;
    }
    inode->ctime = axfs_now();
    axfs_mark_dirty(inode);
    return AXFS_OK;
}

static i32 tmp_utime(axfs_inode_t *inode, const axfs_timespec_t *times)
{
    if (!inode || !times) {
        return AXFS_EINVAL;
    }
    inode->atime = times[0].sec;
    inode->mtime = times[1].sec;
    inode->ctime = axfs_now();
    return AXFS_OK;
}

static i32 tmp_poll(axfs_file_t *file, u32 events)
{
    /* Everything is always ready: memory never blocks. */
    (void)file;
    return (i32)(events | 0x1 | 0x4);
}

static i32 tmp_getxattr(axfs_inode_t *inode, const char *name, void *buffer,
                        usize cap)
{
    (void)inode;
    (void)name;
    (void)buffer;
    (void)cap;
    return AXFS_ENOSYS;
}

static i32 tmp_setxattr(axfs_inode_t *inode, const char *name, const void *value,
                        usize size)
{
    (void)inode;
    (void)name;
    (void)value;
    (void)size;
    return AXFS_ENOSYS;
}

/* ------------------------------------------------------------------ */
/* Superblock ops                                                      */
/* ------------------------------------------------------------------ */

static i32 tmp_alloc_inode(axfs_super_t *super, axfs_inode_t **out)
{
    tmp_super_t *tmp = super ? (tmp_super_t *)super->fs_data : 0;
    if (!tmp || !out) {
        return AXFS_EINVAL;
    }
    tmp_node_t *node = tmp_new_node(tmp, tmp->root, 0, AXFS_FT_FILE, 0644);
    if (!node) {
        return AXFS_ENOMEM;
    }
    node->inode->super = super;
    node->inode->ops = super->root_ops;
    axfs_ref_inode(node->inode);
    *out = node->inode;
    return AXFS_OK;
}

static void tmp_destroy_inode(axfs_inode_t *inode)
{
    tmp_super_t *super = super_of(inode);
    tmp_node_t *node = node_of(inode);
    if (super && node) {
        tmp_detach_child(node->parent ? node->parent : super->root, node);
        tmp_free_node(super, node);
    }
}

static i32 tmp_write_inode(axfs_inode_t *inode)
{
    (void)inode;
    return AXFS_OK;     /* memory is the storage */
}

static void tmp_evict_inode(axfs_inode_t *inode)
{
    (void)inode;
}

static i32 tmp_statfs(axfs_super_t *super, u64 *blocks, u64 *free_blocks,
                      u64 *files, u64 *free_files)
{
    tmp_super_t *tmp = super ? (tmp_super_t *)super->fs_data : 0;
    if (!tmp || !blocks || !free_blocks || !files || !free_files) {
        return AXFS_EINVAL;
    }
    u64 block = 4096;
    u64 total = tmp->budget ? tmp->budget / block : 262144;    /* 1 GiB nominal */
    *blocks = total;
    *free_blocks = total > tmp->used / block ? total - tmp->used / block : 0;
    *files = 0;
    *free_files = 0xFFFFFFFFull;
    return AXFS_OK;
}

static i32 tmp_sync_super(axfs_super_t *super)
{
    (void)super;
    return AXFS_OK;
}

static i32 tmp_remount(axfs_super_t *super, u32 flags)
{
    if (!super) {
        return AXFS_EINVAL;
    }
    super->read_only = (bool_t)((flags & AXFS_MNT_RDONLY) != 0);
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static axfs_inode_ops_t g_tmp_ops;
static axfs_super_ops_t g_tmp_super_ops;
static bool_t g_tmp_ready;

void axfs_tmpfs_init(void)
{
    if (g_tmp_ready) {
        return;
    }
    ax_memset(&g_tmp_ops, 0, sizeof(g_tmp_ops));
    g_tmp_ops.lookup = tmp_lookup;
    g_tmp_ops.create = tmp_create;
    g_tmp_ops.mkdir = tmp_mkdir;
    g_tmp_ops.unlink = tmp_unlink;
    g_tmp_ops.rmdir = tmp_rmdir;
    g_tmp_ops.rename = tmp_rename;
    g_tmp_ops.symlink = tmp_symlink;
    g_tmp_ops.readlink = tmp_readlink;
    g_tmp_ops.link = tmp_link;
    g_tmp_ops.mknod = tmp_mknod;
    g_tmp_ops.read = tmp_read;
    g_tmp_ops.write = tmp_write;
    g_tmp_ops.readdir = tmp_readdir;
    g_tmp_ops.truncate = tmp_truncate;
    g_tmp_ops.stat = tmp_stat;
    g_tmp_ops.chmod = tmp_chmod;
    g_tmp_ops.chown = tmp_chown;
    g_tmp_ops.utime = tmp_utime;
    g_tmp_ops.poll = tmp_poll;
    g_tmp_ops.getxattr = tmp_getxattr;
    g_tmp_ops.setxattr = tmp_setxattr;

    ax_memset(&g_tmp_super_ops, 0, sizeof(g_tmp_super_ops));
    g_tmp_super_ops.alloc_inode = tmp_alloc_inode;
    g_tmp_super_ops.destroy_inode = tmp_destroy_inode;
    g_tmp_super_ops.write_inode = tmp_write_inode;
    g_tmp_super_ops.evict_inode = tmp_evict_inode;
    g_tmp_super_ops.statfs = tmp_statfs;
    g_tmp_super_ops.sync = tmp_sync_super;
    g_tmp_super_ops.remount = tmp_remount;
    g_tmp_ready = (bool_t)1;
}

axfs_super_t *axfs_tmpfs_create(const char *name, u64 max_bytes)
{
    axfs_tmpfs_init();

    axfs_super_t *super = (axfs_super_t *)kzalloc(sizeof(*super));
    tmp_super_t *tmp = (tmp_super_t *)kzalloc(sizeof(*tmp));
    if (!super || !tmp) {
        if (super) {
            kfree(super);
        }
        if (tmp) {
            kfree(tmp);
        }
        return 0;
    }
    tmp->name = name;
    tmp->budget = max_bytes;
    tmp->next_ino = 2;      /* 1 is reserved for the root */

    super->name = name;
    super->magic = 0x544D5046;      /* "TMPF" */
    super->features = AXFS_F_SYMLINKS | AXFS_F_HARDLINKS | AXFS_F_XATTR;
    super->block_size = 4096;
    super->total_blocks = max_bytes ? max_bytes / 4096 : 262144;
    super->free_blocks = super->total_blocks;
    super->total_inodes = 0;
    super->free_inodes = 0xFFFFFFFFull;
    super->max_name = AXFS_NAME_MAX;
    super->max_file_size = 0x7FFFFFFFFFFFFFFFull;
    super->ops = &g_tmp_super_ops;
    super->root_ops = &g_tmp_ops;
    super->fs_data = tmp;
    super->serial = axfs_next_super_serial();

    tmp_node_t *root = (tmp_node_t *)kzalloc(sizeof(*root));
    if (!root) {
        kfree(tmp);
        kfree(super);
        return 0;
    }
    tmp->root = root;
    root->inode = axfs_alloc_inode(super, 1, AXFS_FT_DIR, 0755);
    if (!root->inode) {
        kfree(root);
        kfree(tmp);
        kfree(super);
        return 0;
    }
    root->inode->fs_data = root;
    root->inode->flags |= AXFS_I_ROOT;
    root->inode->ops = &g_tmp_ops;
    root->inode->nlink = 2;
    super->root = root->inode;
    tmp->files = 1;
    return super;
}

/* Total bytes held by every tmpfs instance, for the memory page. */
u64 axfs_tmpfs_bytes(void)
{
    return g_tmpfs_total_used;
}

/* Free space on a tmpfs, in bytes. */
u64 axfs_tmpfs_free(const axfs_super_t *super)
{
    if (!super || !super->fs_data) {
        return 0;
    }
    const tmp_super_t *tmp = (const tmp_super_t *)super->fs_data;
    if (!tmp->budget) {
        return 0;       /* unbounded */
    }
    return tmp->budget > tmp->used ? tmp->budget - tmp->used : 0;
}

void axfs_tmpfs_dump(const axfs_super_t *super)
{
    if (!super || !super->fs_data) {
        return;
    }
    const tmp_super_t *tmp = (const tmp_super_t *)super->fs_data;
    printk("tmpfs '%s': used=%llu budget=%llu files=%llu\n",
           tmp->name ? tmp->name : "?", tmp->used, tmp->budget, tmp->files);
    if (!super->root) {
        return;
    }
    axfs_dirent_t entry;
    for (u64 i = 2; i < 64; i++) {
        if (tmp_readdir(super->root, i, &entry) != AXFS_OK) {
            break;
        }
        printk("  %s ino=%llu type=%u\n", entry.name, entry.ino, entry.type);
    }
}
