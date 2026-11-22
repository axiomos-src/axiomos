/*
 * AxiomOS - VFS file objects and descriptor tables.
 *
 * A `axfs_file_t` is the kernel's open file description: it owns the inode
 * reference, the byte offset and the open flags.  Descriptor tables hold
 * pointers to descriptions, so dup() shares the offset the way the POSIX model
 * requires while open() twice produces two independent offsets.
 *
 * Reads and writes that a filesystem has no custom implementation for fall
 * back to the page cache, which is the path tmpfs and FAT both use.  Sparse
 * reads past EOF return zero bytes rather than an error, so a tool that seeks
 * beyond the end and writes is handled by file_write extending the inode.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axfs.h>

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static axfs_fdtable_t g_kernel_fds;
static bool_t         g_fds_ready;

/* Lifetime counters, surfaced by `fs stats`. */
static u64 g_opens;
static u64 g_closes;
static u64 g_reads;
static u64 g_writes;
static u64 g_bytes_read;
static u64 g_bytes_written;
static u64 g_seeks;

/* ------------------------------------------------------------------ */
/* File objects                                                        */
/* ------------------------------------------------------------------ */

axfs_file_t *axfs_file_alloc(axfs_inode_t *inode, axfs_dentry_t *dentry, u32 flags)
{
    axfs_file_t *file = (axfs_file_t *)kzalloc(sizeof(*file));
    if (!file) {
        return 0;
    }
    file->inode = inode;
    axfs_ref_inode(inode);
    if (dentry) {
        axfs_ref_dentry(dentry);
        file->dentry = dentry;
    }
    file->flags = flags;
    file->refs = 1;
    file->fd = -1;
    g_opens++;
    return file;
}

i32 axfs_file_close(axfs_file_t *file)
{
    if (!file) {
        return AXFS_EBADF;
    }
    if (file->refs > 1) {
        file->refs--;
        return AXFS_OK;
    }
    if (file->flags & (AXFS_O_WRONLY | AXFS_O_RDWR)) {
        axfs_file_fsync(file);
    }
    if (file->dentry) {
        axfs_unref_dentry(file->dentry);
        file->dentry = 0;
    }
    if (file->inode) {
        axfs_unref_inode(file->inode);
        file->inode = 0;
    }
    g_closes++;
    kfree(file);
    return AXFS_OK;
}

/* Open: resolve, enforce flags, then hand the inode to the filesystem. */
axfs_file_t *axfs_file_open(axfs_mount_t *mount, const char *path, u32 flags,
                            u32 mode)
{
    axfs_inode_t *inode = 0;
    axfs_dentry_t *dentry = 0;
    i32 rc = axfs_lookup(mount, path, &inode, &dentry, flags);

    if (rc == AXFS_ENOENT && (flags & AXFS_O_CREAT)) {
        char dir_path[AXFS_PATH_MAX];
        char base[AXFS_NAME_MAX + 1];
        if (axfs_path_dirname(path, dir_path, sizeof(dir_path)) != AXFS_OK ||
            axfs_path_basename(path, base, sizeof(base)) != AXFS_OK) {
            return 0;
        }
        axfs_inode_t *dir = 0;
        rc = axfs_lookup(mount, dir_path, &dir, 0, 0);
        if (rc != AXFS_OK || !dir) {
            return 0;
        }
        if (!dir->ops || !dir->ops->create) {
            axfs_unref_inode(dir);
            return 0;
        }
        rc = dir->ops->create(dir, base, mode ? mode : 0644, &inode);
        axfs_unref_inode(dir);
        if (rc != AXFS_OK || !inode) {
            return 0;
        }
        inode->type = AXFS_FT_FILE;
    } else if (rc != AXFS_OK || !inode) {
        return 0;
    }

    if ((flags & AXFS_O_DIRECTORY) && inode->type != AXFS_FT_DIR) {
        axfs_unref_inode(inode);
        return 0;
    }
    if ((flags & AXFS_O_EXCL) && rc == AXFS_OK && (flags & AXFS_O_CREAT) &&
        dentry && dentry->inode) {
        /* O_CREAT|O_EXCL on an existing name must fail. */
        axfs_unref_inode(inode);
        if (dentry) {
            axfs_unref_dentry(dentry);
        }
        return 0;
    }
    if ((flags & AXFS_O_TRUNC) && inode->type == AXFS_FT_FILE) {
        if (!inode->ops || !inode->ops->truncate) {
            axfs_unref_inode(inode);
            if (dentry) {
                axfs_unref_dentry(dentry);
            }
            return 0;
        }
        i32 trc = inode->ops->truncate(inode, 0);
        if (trc != AXFS_OK) {
            axfs_unref_inode(inode);
            if (dentry) {
                axfs_unref_dentry(dentry);
            }
            return 0;
        }
        inode->size = 0;
        axfs_mark_dirty(inode);
    }
    if (inode->type == AXFS_FT_DIR && (flags & AXFS_O_WRONLY)) {
        axfs_unref_inode(inode);
        if (dentry) {
            axfs_unref_dentry(dentry);
        }
        return 0;
    }

    /* A read-only mount refuses any write intent. */
    if (inode->mount && (inode->mount->flags & AXFS_MNT_RDONLY) &&
        (flags & (AXFS_O_WRONLY | AXFS_O_RDWR | AXFS_O_CREAT))) {
        axfs_unref_inode(inode);
        if (dentry) {
            axfs_unref_dentry(dentry);
        }
        return 0;
    }

    axfs_file_t *file = axfs_file_alloc(inode, dentry, flags);
    if (!file) {
        axfs_unref_inode(inode);
        if (dentry) {
            axfs_unref_dentry(dentry);
        }
        return 0;
    }
    axfs_unref_inode(inode);     /* the file owns its own reference */
    if (dentry) {
        axfs_unref_dentry(dentry);
    }
    if (flags & AXFS_O_APPEND) {
        file->offset = inode->size;
    }
    return file;
}

i64 axfs_file_read(axfs_file_t *file, void *buffer, u64 count)
{
    if (!file || !buffer) {
        return AXFS_EINVAL;
    }
    if ((file->flags & AXFS_O_ACCMODE) == AXFS_O_WRONLY) {
        return AXFS_EBADF;
    }
    if (file->inode->type == AXFS_FT_DIR) {
        return AXFS_EISDIR;
    }
    if (count == 0) {
        return 0;
    }
    /* Shorten the request at EOF: the traditional read semantics. */
    if (file->offset >= file->inode->size) {
        return 0;
    }
    u64 remaining = file->inode->size - file->offset;
    if (count > remaining) {
        count = remaining;
    }

    i64 result;
    if (file->inode->ops && file->inode->ops->read) {
        result = file->inode->ops->read(file, buffer, count);
    } else {
        result = axfs_page_read(file->inode, file->offset, buffer, count);
    }
    if (result > 0) {
        file->offset += (u64)result;
        file->inode->atime = axfs_now();
        file->read_bytes += (u32)result;
        g_reads++;
        g_bytes_read += (u64)result;
    }
    return result;
}

i64 axfs_file_write(axfs_file_t *file, const void *buffer, u64 count)
{
    if (!file || !buffer) {
        return AXFS_EINVAL;
    }
    if ((file->flags & AXFS_O_ACCMODE) == AXFS_O_RDONLY) {
        return AXFS_EBADF;
    }
    if (file->inode->type == AXFS_FT_DIR) {
        return AXFS_EISDIR;
    }
    if (file->inode->mount && (file->inode->mount->flags & AXFS_MNT_RDONLY)) {
        return AXFS_EROFS;
    }
    if (count == 0) {
        return 0;
    }
    if (file->flags & AXFS_O_APPEND) {
        file->offset = file->inode->size;
    }
    /* A filesystem ceiling, typically 4 GiB minus one for FAT32. */
    if (file->inode->super && file->inode->super->max_file_size &&
        file->offset + count > file->inode->super->max_file_size) {
        return AXFS_EFBIG;
    }

    i64 result;
    if (file->inode->ops && file->inode->ops->write) {
        result = file->inode->ops->write(file, buffer, count);
    } else {
        result = axfs_page_write(file->inode, file->offset, buffer, count);
    }
    if (result > 0) {
        file->offset += (u64)result;
        if (file->offset > file->inode->size) {
            file->inode->size = file->offset;
        }
        file->inode->mtime = axfs_now();
        file->inode->ctime = file->inode->mtime;
        axfs_mark_dirty(file->inode);
        file->write_bytes += (u32)result;
        g_writes++;
        g_bytes_written += (u64)result;
    }
    return result;
}

i64 axfs_file_seek(axfs_file_t *file, i64 offset, i32 whence)
{
    if (!file) {
        return AXFS_EBADF;
    }
    i64 base;
    switch (whence) {
    case 0:     /* SEEK_SET */
        base = 0;
        break;
    case 1:     /* SEEK_CUR */
        base = (i64)file->offset;
        break;
    case 2:     /* SEEK_END */
        base = (i64)file->inode->size;
        break;
    default:
        return AXFS_EINVAL;
    }
    i64 target = base + offset;
    if (target < 0) {
        return AXFS_EINVAL;
    }
    /* Seeking far past the end is allowed for regular files; the write that
     * follows materialises the gap as zeros or holes. */
    if (file->inode->super && file->inode->super->max_file_size &&
        (u64)target > file->inode->super->max_file_size) {
        return AXFS_EFBIG;
    }
    file->offset = (u64)target;
    g_seeks++;
    return target;
}

i64 axfs_file_size(axfs_file_t *file)
{
    return file ? (i64)file->inode->size : AXFS_EBADF;
}

i32 axfs_file_fsync(axfs_file_t *file)
{
    if (!file) {
        return AXFS_EBADF;
    }
    if (file->inode->ops && file->inode->ops->fsync) {
        return file->inode->ops->fsync(file);
    }
    return axfs_page_flush(file->inode);
}

i32 axfs_file_readdir(axfs_file_t *file, axfs_dirent_t *out)
{
    if (!file || !out) {
        return AXFS_EINVAL;
    }
    if (file->inode->type != AXFS_FT_DIR) {
        return AXFS_ENOTDIR;
    }
    if (!file->inode->ops || !file->inode->ops->readdir) {
        return AXFS_ENOSYS;
    }
    /* The offset is a directory index for directory handles. */
    i32 rc = file->inode->ops->readdir(file->inode, file->offset, out);
    if (rc == AXFS_OK) {
        file->offset++;
    }
    return rc;
}

/* Duplicate an open file description: both descriptors then share the offset,
 * flags and inode, matching dup() semantics. */
axfs_file_t *axfs_file_dup(axfs_file_t *file)
{
    if (!file) {
        return 0;
    }
    file->refs++;
    return file;
}

/* ------------------------------------------------------------------ */
/* Descriptor tables                                                   */
/* ------------------------------------------------------------------ */

void axfs_fdtable_init(axfs_fdtable_t *table)
{
    if (!table) {
        return;
    }
    ax_memset(table, 0, sizeof(*table));
    table->max_fd = 0;
}

i32 axfs_fdtable_alloc(axfs_fdtable_t *table, axfs_file_t *file)
{
    if (!table || !file) {
        return AXFS_EBADF;
    }
    for (i32 fd = 0; fd < AXFS_FD_MAX; fd++) {
        if (!table->files[fd]) {
            table->files[fd] = file;
            file->fd = fd;
            file->refs++;
            table->count++;
            if (fd > table->max_fd) {
                table->max_fd = fd;
            }
            if (file->flags & AXFS_O_CLOEXEC) {
                table->close_on_exec[(u32)fd / 32] |= 1u << ((u32)fd % 32);
            }
            return fd;
        }
    }
    return AXFS_EMFILE;
}

axfs_file_t *axfs_fdtable_get(axfs_fdtable_t *table, i32 fd)
{
    if (!table || fd < 0 || fd >= AXFS_FD_MAX) {
        return 0;
    }
    return table->files[fd];
}

i32 axfs_fdtable_put(axfs_fdtable_t *table, i32 fd)
{
    if (!table || fd < 0 || fd >= AXFS_FD_MAX) {
        return AXFS_EBADF;
    }
    axfs_file_t *file = table->files[fd];
    if (!file) {
        return AXFS_EBADF;
    }
    table->files[fd] = 0;
    table->close_on_exec[(u32)fd / 32] &= ~(1u << ((u32)fd % 32));
    table->count--;
    file->fd = -1;
    if (file->refs > 0) {
        file->refs--;
    }
    if (file->refs == 0) {
        axfs_file_close(file);
    }
    return AXFS_OK;
}

i32 axfs_fdtable_dup(axfs_fdtable_t *table, i32 fd)
{
    axfs_file_t *file = axfs_fdtable_get(table, fd);
    if (!file) {
        return AXFS_EBADF;
    }
    return axfs_fdtable_alloc(table, file);
}

i32 axfs_fdtable_dup2(axfs_fdtable_t *table, i32 fd, i32 target)
{
    if (!table || fd < 0 || target < 0 || fd >= AXFS_FD_MAX || target >= AXFS_FD_MAX) {
        return AXFS_EBADF;
    }
    if (fd == target) {
        return axfs_fdtable_get(table, fd) ? target : AXFS_EBADF;
    }
    axfs_file_t *file = axfs_fdtable_get(table, fd);
    if (!file) {
        return AXFS_EBADF;
    }
    if (table->files[target]) {
        axfs_fdtable_put(table, target);
    }
    return axfs_fdtable_alloc(table, file) >= 0 ? target : AXFS_EMFILE;
}

i32 axfs_fdtable_close_all(axfs_fdtable_t *table)
{
    if (!table) {
        return AXFS_EINVAL;
    }
    for (i32 fd = 0; fd < AXFS_FD_MAX; fd++) {
        if (table->files[fd]) {
            axfs_fdtable_put(table, fd);
        }
    }
    table->max_fd = 0;
    return AXFS_OK;
}

/* Clone the descriptor table for fork: each descriptor gets its own
 * description pointer with a bumped reference, so offsets are shared until
 * one side calls exec. */
i32 axfs_fdtable_clone(axfs_fdtable_t *dst, const axfs_fdtable_t *src)
{
    if (!dst || !src) {
        return AXFS_EINVAL;
    }
    axfs_fdtable_init(dst);
    for (i32 fd = 0; fd < AXFS_FD_MAX; fd++) {
        if (src->files[fd]) {
            axfs_file_t *file = src->files[fd];
            file->refs++;
            dst->files[fd] = file;
            dst->count++;
            if (fd > dst->max_fd) {
                dst->max_fd = fd;
            }
        }
        dst->close_on_exec[(u32)fd / 32] = src->close_on_exec[(u32)fd / 32];
    }
    return AXFS_OK;
}

/* Close everything marked close-on-exec, as execve does. */
i32 axfs_fdtable_exec_close(axfs_fdtable_t *table)
{
    if (!table) {
        return AXFS_EINVAL;
    }
    for (i32 fd = 0; fd < AXFS_FD_MAX; fd++) {
        if (table->close_on_exec[(u32)fd / 32] & (1u << ((u32)fd % 32))) {
            axfs_fdtable_put(table, fd);
        }
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Convenience wrappers                                                */
/* ------------------------------------------------------------------ */

static axfs_fdtable_t *kernel_table(void)
{
    if (!g_fds_ready) {
        axfs_fdtable_init(&g_kernel_fds);
        g_fds_ready = (bool_t)1;
    }
    return &g_kernel_fds;
}

i32 axfs_open(const char *path, u32 flags, u32 mode)
{
    axfs_file_t *file = axfs_file_open(axfs_mount_root(), path, flags, mode);
    if (!file) {
        return AXFS_ENOENT;
    }
    return axfs_fdtable_alloc(kernel_table(), file);
}

i64 axfs_read(i32 fd, void *buffer, u64 count)
{
    axfs_file_t *file = axfs_fdtable_get(kernel_table(), fd);
    if (!file) {
        return AXFS_EBADF;
    }
    return axfs_file_read(file, buffer, count);
}

i64 axfs_write(i32 fd, const void *buffer, u64 count)
{
    axfs_file_t *file = axfs_fdtable_get(kernel_table(), fd);
    if (!file) {
        return AXFS_EBADF;
    }
    return axfs_file_write(file, buffer, count);
}

i32 axfs_close(i32 fd)
{
    return axfs_fdtable_put(kernel_table(), fd);
}

i32 axfs_stat_path(const char *path, axfs_attr_t *out)
{
    axfs_inode_t *inode = 0;
    i32 rc = axfs_lookup(axfs_mount_root(), path, &inode, 0, 0);
    if (rc != AXFS_OK || !inode) {
        return rc;
    }
    i32 src = AXFS_OK;
    if (inode->ops && inode->ops->stat) {
        src = inode->ops->stat(inode, out);
    } else if (out) {
        out->mode = inode->mode;
        out->uid = inode->uid;
        out->gid = inode->gid;
        out->size = inode->size;
        out->blocks = inode->blocks;
        out->ino = inode->ino;
        out->atime = inode->atime;
        out->mtime = inode->mtime;
        out->ctime = inode->ctime;
        out->nlink = inode->nlink;
        out->type = inode->type;
        src = AXFS_OK;
    }
    axfs_unref_inode(inode);
    return src;
}

i32 axfs_readdir_path(const char *path, axfs_dirent_t *out, u64 index)
{
    axfs_inode_t *inode = 0;
    i32 rc = axfs_lookup(axfs_mount_root(), path, &inode, 0, 0);
    if (rc != AXFS_OK || !inode) {
        return rc;
    }
    if (inode->type != AXFS_FT_DIR) {
        axfs_unref_inode(inode);
        return AXFS_ENOTDIR;
    }
    if (!inode->ops || !inode->ops->readdir) {
        axfs_unref_inode(inode);
        return AXFS_ENOSYS;
    }
    rc = inode->ops->readdir(inode, index, out);
    axfs_unref_inode(inode);
    return rc;
}

i32 axfs_statfs(const char *path, axfs_statfs_t *out)
{
    axfs_inode_t *inode = 0;
    i32 rc = axfs_lookup(axfs_mount_root(), path, &inode, 0, 0);
    if (rc != AXFS_OK || !inode) {
        return rc;
    }
    axfs_super_t *super = inode->super;
    axfs_unref_inode(inode);
    if (!super || !out) {
        return AXFS_EINVAL;
    }
    out->blocks = super->total_blocks;
    out->free_blocks = super->free_blocks;
    out->files = super->total_inodes;
    out->free_files = super->free_inodes;
    out->block_size = super->block_size;
    out->name_max = super->max_name;
    if (super->ops && super->ops->statfs) {
        u64 free_blocks = super->free_blocks;
        u64 total = super->total_blocks;
        u64 files = super->total_inodes;
        u64 free_files = super->free_inodes;
        i32 src = super->ops->statfs(super, &total, &free_blocks, &files, &free_files);
        if (src == AXFS_OK) {
            out->blocks = total;
            out->free_blocks = free_blocks;
            out->files = files;
            out->free_files = free_files;
        }
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

typedef struct axfs_io_stats {
    u64 opens;
    u64 closes;
    u64 reads;
    u64 writes;
    u64 bytes_read;
    u64 bytes_written;
    u64 seeks;
    i32 open_files;
} axfs_io_stats_t;

void axfs_io_stats(axfs_io_stats_t *out)
{
    if (!out) {
        return;
    }
    out->opens = g_opens;
    out->closes = g_closes;
    out->reads = g_reads;
    out->writes = g_writes;
    out->bytes_read = g_bytes_read;
    out->bytes_written = g_bytes_written;
    out->seeks = g_seeks;
    out->open_files = g_kernel_fds.count;
}

void axfs_file_dump(axfs_file_t *file)
{
    if (!file || !file->inode) {
        printk("file: (null)\n");
        return;
    }
    printk("file: ino=%llu type=%u off=%llu/%llu flags=%x refs=%d\n",
           file->inode->ino, file->inode->type, file->offset, file->inode->size,
           file->flags, file->refs);
}
