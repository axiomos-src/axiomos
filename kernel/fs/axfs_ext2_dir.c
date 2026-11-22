/*
 * AxiomOS - ext2 directory operations.
 *
 * ext2 stores a directory as a linked list of variable-length records inside
 * its data blocks.  Each record has a 32-bit inode number, a record length that
 * includes any slack, a name length, an optional file type and the name
 * itself.  Deleted entries are absorbed by growing the previous record's
 * length, which is why removing a name never leaves a hole.
 *
 * This file implements the list manipulation and the axfs namespace hooks.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include "ext2_int.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

i32 ext2_name_valid(const char *name)
{
    if (!name || name[0] == 0) {
        return AXFS_EINVAL;
    }
    usize len = ax_strlen(name);
    if (len > EXT2_DIRENT_NAME_MAX) {
        return AXFS_ENAMETOOLONG;
    }
    if (ax_strcmp(name, ".") == 0 || ax_strcmp(name, "..") == 0) {
        return AXFS_EINVAL;
    }
    for (usize i = 0; i < len; i++) {
        if (name[i] == '/') {
            return AXFS_EINVAL;
        }
    }
    return AXFS_OK;
}

u8 ext2_mode_to_filetype(u16 mode)
{
    if (mode & EXT2_S_IFDIR) {
        return EXT2_FT_DIR;
    }
    if (mode & EXT2_S_IFLNK) {
        return EXT2_FT_SYMLINK;
    }
    if (mode & EXT2_S_IFCHR) {
        return EXT2_FT_CHRDEV;
    }
    if (mode & EXT2_S_IFBLK) {
        return EXT2_FT_BLKDEV;
    }
    if (mode & EXT2_S_IFIFO) {
        return EXT2_FT_FIFO;
    }
    if (mode & EXT2_S_IFSOCK) {
        return EXT2_FT_SOCK;
    }
    return EXT2_FT_REG_FILE;
}

static u32 dira_round(u32 len)
{
    return (len + 3u) & ~3u;
}

/* Record fields, read and written directly in a block buffer. */
static u32 rec_ino(const u8 *raw)
{
    return (u32)raw[0] | ((u32)raw[1] << 8) | ((u32)raw[2] << 16) |
           ((u32)raw[3] << 24);
}

static void rec_set_ino(u8 *raw, u32 ino)
{
    raw[0] = (u8)(ino & 0xFF);
    raw[1] = (u8)((ino >> 8) & 0xFF);
    raw[2] = (u8)((ino >> 16) & 0xFF);
    raw[3] = (u8)((ino >> 24) & 0xFF);
}

static u16 rec_len(const u8 *raw)
{
    return (u16)(raw[4] | ((u16)raw[5] << 8));
}

static void rec_set_len(u8 *raw, u16 len)
{
    raw[4] = (u8)(len & 0xFF);
    raw[5] = (u8)((len >> 8) & 0xFF);
}

static bool_t name_matches(const u8 *raw, const char *name)
{
    u8 name_len = raw[6];
    usize len = ax_strlen(name);
    if (name_len != len) {
        return (bool_t)0;
    }
    for (usize i = 0; i < len; i++) {
        if (raw[8 + i] != (u8)name[i]) {
            return (bool_t)0;
        }
    }
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Reading a block of directory records                                */
/* ------------------------------------------------------------------ */

typedef struct dir_block {
    u32 block;
    u8 *data;
    bool_t dirty;
} dir_block_t;

static i32 load_dir_block(ext2_volume_t *volume, const ext2_inode_t *inode,
                          u32 logical, dir_block_t *out)
{
    u32 physical = 0;
    i32 status = ext2_bmap(volume, inode, logical, &physical, (bool_t)0);
    if (status != AXFS_OK) {
        return status;
    }
    if (physical == 0) {
        return AXFS_ENOENT;
    }
    out->block = physical;
    out->data = (u8 *)kmalloc(volume->block_size);
    out->dirty = (bool_t)0;
    if (!out->data) {
        return AXFS_ENOMEM;
    }
    status = ext2_read_block(volume, physical, out->data);
    if (status != AXFS_OK) {
        kfree(out->data);
        out->data = 0;
        return status;
    }
    return AXFS_OK;
}

static void unload_dir_block(ext2_volume_t *volume, dir_block_t *block)
{
    if (!block->data) {
        return;
    }
    if (block->dirty) {
        ext2_write_block(volume, block->block, block->data);
    }
    kfree(block->data);
    block->data = 0;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

i32 ext2_dir_lookup(ext2_volume_t *volume, ext2_handle_t *dir, const char *name,
                     u32 *out_ino, u8 *out_type)
{
    if (!volume || !dir || !name) {
        return AXFS_EINVAL;
    }
    u32 blocks = (dir->inode.size + volume->block_size - 1) / volume->block_size;
    for (u32 logical = 0; logical < blocks; logical++) {
        dir_block_t block;
        i32 status = load_dir_block(volume, &dir->inode, logical, &block);
        if (status == AXFS_ENOENT) {
            continue;
        }
        if (status != AXFS_OK) {
            return status;
        }
        u32 offset = 0;
        while (offset < volume->block_size) {
            const u8 *raw = block.data + offset;
            u16 len = rec_len(raw);
            if (len < 8 || offset + len > volume->block_size) {
                break;
            }
            if (raw[0] && name_matches(raw, name)) {
                if (out_ino) {
                    *out_ino = rec_ino(raw);
                }
                if (out_type) {
                    *out_type = volume->has_filetype
                                    ? raw[7]
                                    : (u8)EXT2_FT_UNKNOWN;
                }
                unload_dir_block(volume, &block);
                return AXFS_OK;
            }
            offset += len;
        }
        unload_dir_block(volume, &block);
    }
    return AXFS_ENOENT;
}

i32 ext2_dir_list(ext2_volume_t *volume, ext2_handle_t *dir, u32 index,
                   char *name, u32 name_cap, u32 *out_ino, u8 *out_type)
{
    if (!volume || !dir || !name || name_cap == 0) {
        return AXFS_EINVAL;
    }
    u32 seen = 0;
    u32 blocks = (dir->inode.size + volume->block_size - 1) / volume->block_size;
    for (u32 logical = 0; logical < blocks; logical++) {
        dir_block_t block;
        i32 status = load_dir_block(volume, &dir->inode, logical, &block);
        if (status == AXFS_ENOENT) {
            continue;
        }
        if (status != AXFS_OK) {
            return status;
        }
        u32 offset = 0;
        while (offset < volume->block_size) {
            const u8 *raw = block.data + offset;
            u16 len = rec_len(raw);
            if (len < 8 || offset + len > volume->block_size) {
                break;
            }
            if (raw[0]) {
                if (seen == index) {
                    u8 name_len = raw[6];
                    u32 copy = name_len < name_cap - 1 ? name_len : name_cap - 1;
                    for (u32 i = 0; i < copy; i++) {
                        name[i] = (char)raw[8 + i];
                    }
                    name[copy] = 0;
                    if (out_ino) {
                        *out_ino = rec_ino(raw);
                    }
                    if (out_type) {
                        *out_type = volume->has_filetype ? raw[7]
                                                          : EXT2_FT_UNKNOWN;
                    }
                    unload_dir_block(volume, &block);
                    return AXFS_OK;
                }
                seen++;
            }
            offset += len;
        }
        unload_dir_block(volume, &block);
    }
    return AXFS_ENOENT;
}

/* ------------------------------------------------------------------ */
/* Adding a name                                                       */
/* ------------------------------------------------------------------ */

/* Write a record into a block at `offset`, returning the slack that follows. */
static u16 fill_record(u8 *raw, u32 ino, const char *name, u8 type, u16 rec_space)
{
    u8 name_len = (u8)ax_strlen(name);
    rec_set_ino(raw, ino);
    rec_set_len(raw, rec_space);
    raw[6] = name_len;
    raw[7] = type;
    for (u8 i = 0; i < name_len; i++) {
        raw[8 + i] = (u8)name[i];
    }
    u16 actual = (u16)dira_round(8 + name_len);
    return (u16)(rec_space - actual);
}

i32 ext2_dir_add(ext2_volume_t *volume, ext2_handle_t *dir, const char *name,
                  u32 ino, u8 type)
{
    if (!volume || !dir || !name) {
        return AXFS_EINVAL;
    }
    i32 valid = ext2_name_valid(name);
    if (valid != AXFS_OK) {
        return valid;
    }
    u32 blocks = (dir->inode.size + volume->block_size - 1) / volume->block_size;
    u32 need = dira_round(8 + (u32)ax_strlen(name));

    /* First pass: reuse existing slack. */
    for (u32 logical = 0; logical < blocks; logical++) {
        dir_block_t block;
        i32 status = load_dir_block(volume, &dir->inode, logical, &block);
        if (status == AXFS_ENOENT) {
            continue;
        }
        if (status != AXFS_OK) {
            return status;
        }
        u32 offset = 0;
        while (offset < volume->block_size) {
            u8 *raw = block.data + offset;
            u16 len = rec_len(raw);
            if (len < 8 || offset + len > volume->block_size) {
                break;
            }
            u16 used = (u16)dira_round(8 + raw[6]);
            u16 slack = (u16)(len - used);
            if (raw[0] == 0 && len >= need) {
                fill_record(raw, ino, name, type, len);
                block.dirty = (bool_t)1;
                unload_dir_block(volume, &block);
                return AXFS_OK;
            }
            if (slack >= need) {
                /* Split the record: shrink this one, place the new record
                 * after it. */
                rec_set_len(raw, used);
                u8 *next = raw + used;
                fill_record(next, ino, name, type, slack);
                block.dirty = (bool_t)1;
                unload_dir_block(volume, &block);
                return AXFS_OK;
            }
            offset += len;
        }
        unload_dir_block(volume, &block);
    }

    /* Second pass: append a fresh block. */
    u32 block_index = blocks;
    u32 physical = 0;
    i32 status = ext2_bmap(volume, &dir->inode, block_index, &physical, (bool_t)1);
    if (status != AXFS_OK) {
        return status;
    }
    u8 *fresh = (u8 *)kzalloc(volume->block_size);
    if (!fresh) {
        return AXFS_ENOMEM;
    }
    fill_record(fresh, ino, name, type, (u16)volume->block_size);
    status = ext2_write_block(volume, physical, fresh);
    kfree(fresh);
    if (status != AXFS_OK) {
        return status;
    }
    dir->inode.size += volume->block_size;
    dir->inode.blocks += volume->block_size / 512;
    dir->dirty = (bool_t)1;
    if (dir->inode.block[EXT2_IND_BLOCK] || block_index < EXT2_NDIR_BLOCKS) {
        dir->inode.blocks = dir->inode.blocks;
    }
    ext2_write_inode(volume, dir->ino, &dir->inode);
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Removing a name                                                     */
/* ------------------------------------------------------------------ */

i32 ext2_dir_remove(ext2_volume_t *volume, ext2_handle_t *dir, const char *name)
{
    if (!volume || !dir || !name) {
        return AXFS_EINVAL;
    }
    u32 blocks = (dir->inode.size + volume->block_size - 1) / volume->block_size;
    for (u32 logical = 0; logical < blocks; logical++) {
        dir_block_t block;
        i32 status = load_dir_block(volume, &dir->inode, logical, &block);
        if (status == AXFS_ENOENT) {
            continue;
        }
        if (status != AXFS_OK) {
            return status;
        }
        u32 offset = 0;
        u32 previous = 0;
        bool_t have_previous = (bool_t)0;
        while (offset < volume->block_size) {
            u8 *raw = block.data + offset;
            u16 len = rec_len(raw);
            if (len < 8 || offset + len > volume->block_size) {
                break;
            }
            if (raw[0] && name_matches(raw, name)) {
                if (have_previous) {
                    /* Absorb this record into the previous one. */
                    u8 *prev = block.data + previous;
                    rec_set_len(prev, (u16)(rec_len(prev) + len));
                    block.dirty = (bool_t)1;
                } else {
                    /* The first record of the block cannot be merged upwards,
                     * so clear it instead. */
                    rec_set_ino(raw, 0);
                    raw[6] = 0;
                    block.dirty = (bool_t)1;
                }
                unload_dir_block(volume, &block);
                return AXFS_OK;
            }
            previous = offset;
            have_previous = (bool_t)1;
            offset += len;
        }
        unload_dir_block(volume, &block);
    }
    return AXFS_ENOENT;
}

/* ------------------------------------------------------------------ */
/* Directory initialisation                                            */
/* ------------------------------------------------------------------ */

i32 ext2_dir_init(ext2_volume_t *volume, u32 ino, u32 parent_ino)
{
    if (!volume || ino == 0) {
        return AXFS_EINVAL;
    }
    ext2_inode_t inode;
    i32 status = ext2_read_inode(volume, ino, &inode);
    if (status != AXFS_OK) {
        return status;
    }
    u32 physical = 0;
    status = ext2_bmap(volume, &inode, 0, &physical, (bool_t)1);
    if (status != AXFS_OK) {
        return status;
    }
    u8 *block = (u8 *)kzalloc(volume->block_size);
    if (!block) {
        return AXFS_ENOMEM;
    }
    u16 first = (u16)dira_round(8 + 1);
    fill_record(block, ino, ".", EXT2_FT_DIR, first);
    u16 second = (u16)(volume->block_size - first);
    fill_record(block + first, parent_ino, "..", EXT2_FT_DIR, second);
    status = ext2_write_block(volume, physical, block);
    kfree(block);
    if (status != AXFS_OK) {
        return status;
    }
    inode.size = volume->block_size;
    inode.blocks += volume->block_size / 512;
    inode.links_count = (u16)(inode.links_count ? inode.links_count : 2);
    return ext2_write_inode(volume, ino, &inode);
}

/* When a directory becomes empty except for dot entries, collapse it back to a
 * single block.  Called by rmdir before the inode is freed. */
i32 ext2_dir_merge(ext2_volume_t *volume, ext2_handle_t *dir)
{
    if (!volume || !dir) {
        return AXFS_EINVAL;
    }
    u32 blocks = (dir->inode.size + volume->block_size - 1) / volume->block_size;
    if (blocks <= 1) {
        return AXFS_OK;
    }
    for (u32 logical = 1; logical < blocks; logical++) {
        u32 physical = 0;
        i32 status = ext2_bmap(volume, &dir->inode, logical, &physical, (bool_t)0);
        if (status != AXFS_OK) {
            return status;
        }
        if (physical) {
            ext2_free_block(volume, physical);
        }
    }
    dir->inode.size = volume->block_size;
    dir->dirty = (bool_t)1;
    return ext2_write_inode(volume, dir->ino, &dir->inode);
}

/* ------------------------------------------------------------------ */
/* Namespace hooks                                                     */
/* ------------------------------------------------------------------ */

static ext2_volume_t *volume_of(axfs_inode_t *inode)
{
    if (!inode || !inode->super) {
        return 0;
    }
    return (ext2_volume_t *)inode->super->fs_data;
}

typedef struct ext2_payload_view {
    u32 ino;
    u16 mode;
} ext2_payload_view_t;

static u32 ino_of(axfs_inode_t *inode)
{
    if (!inode || !inode->fs_data) {
        return 0;
    }
    return ((ext2_payload_view_t *)inode->fs_data)->ino;
}

/* Load a handle for a directory inode.  Handles are stack objects here because
 * every operation is short-lived and the inode table is only ever a few
 * thousand entries. */
static i32 load_handle(axfs_inode_t *node, ext2_handle_t *out)
{
    ext2_volume_t *volume = volume_of(node);
    if (!volume || !out) {
        return AXFS_EIO;
    }
    ax_memset(out, 0, sizeof(*out));
    out->volume = volume;
    out->ino = ino_of(node);
    i32 status = ext2_read_inode(volume, out->ino, &out->inode);
    return status;
}

static i32 ext2_lookup_op(axfs_inode_t *dir, const char *name, axfs_inode_t **out)
{
    if (!dir || !name || !out) {
        return AXFS_EINVAL;
    }
    ext2_handle_t handle;
    i32 status = load_handle(dir, &handle);
    if (status != AXFS_OK) {
        return status;
    }
    u32 ino = 0;
    u8 type = 0;
    status = ext2_dir_lookup(handle.volume, &handle, name, &ino, &type);
    if (status != AXFS_OK) {
        return status;
    }
    ext2_inode_t child;
    status = ext2_read_inode(handle.volume, ino, &child);
    if (status != AXFS_OK) {
        return status;
    }
    axfs_inode_t *inode = axfs_ext2_make_inode(dir->super, ino, child.mode,
                                                child.size, name);
    if (!inode) {
        return AXFS_ENOMEM;
    }
    *out = inode;
    return AXFS_OK;
}

static i32 ext2_readdir_op(axfs_inode_t *dir, u64 index, axfs_dirent_t *out)
{
    if (!dir || !out) {
        return AXFS_EINVAL;
    }
    ext2_handle_t handle;
    i32 status = load_handle(dir, &handle);
    if (status != AXFS_OK) {
        return status;
    }
    if (index == 0) {
        ax_strlcpy(out->name, ".", sizeof(out->name));
        out->type = AXFS_FT_DIR;
        out->ino = handle.ino;
        return AXFS_OK;
    }
    if (index == 1) {
        ax_strlcpy(out->name, "..", sizeof(out->name));
        out->type = AXFS_FT_DIR;
        out->ino = handle.ino;
        return AXFS_OK;
    }
    u32 ino = 0;
    u8 type = 0;
    status = ext2_dir_list(handle.volume, &handle, (u32)(index - 2), out->name,
                           sizeof(out->name), &ino, &type);
    if (status != AXFS_OK) {
        return status;
    }
    out->ino = ino;
    switch (type) {
    case EXT2_FT_DIR:
        out->type = AXFS_FT_DIR;
        break;
    case EXT2_FT_SYMLINK:
        out->type = AXFS_FT_SYMLINK;
        break;
    case EXT2_FT_CHRDEV:
        out->type = AXFS_FT_CHAR;
        break;
    case EXT2_FT_BLKDEV:
        out->type = AXFS_FT_BLOCK;
        break;
    case EXT2_FT_FIFO:
        out->type = AXFS_FT_FIFO;
        break;
    case EXT2_FT_SOCK:
        out->type = AXFS_FT_SOCKET;
        break;
    default:
        out->type = AXFS_FT_FILE;
        break;
    }
    return AXFS_OK;
}

/* Allocate a fresh inode of the given mode. */
static i32 create_inode(ext2_volume_t *volume, u16 mode, u16 links, u32 *out_ino)
{
    u32 ino = 0;
    i32 status = ext2_alloc_inode(volume, &ino);
    if (status != AXFS_OK) {
        return status;
    }
    ext2_inode_t inode;
    ax_memset(&inode, 0, sizeof(inode));
    inode.mode = mode;
    inode.links_count = links;
    inode.size = 0;
    inode.atime = (u32)axfs_now();
    inode.mtime = inode.atime;
    inode.ctime = inode.atime;
    status = ext2_write_inode(volume, ino, &inode);
    if (status != AXFS_OK) {
        ext2_free_inode(volume, ino);
        return status;
    }
    *out_ino = ino;
    return AXFS_OK;
}

static i32 ext2_create_op(axfs_inode_t *dir, const char *name, u32 mode,
                          axfs_inode_t **out)
{
    if (!dir || !name) {
        return AXFS_EINVAL;
    }
    ext2_handle_t handle;
    i32 status = load_handle(dir, &handle);
    if (status != AXFS_OK) {
        return status;
    }
    if (handle.volume->read_only) {
        return AXFS_EROFS;
    }
    u32 existing = 0;
    u8 type = 0;
    if (ext2_dir_lookup(handle.volume, &handle, name, &existing, &type) == AXFS_OK) {
        return AXFS_EEXIST;
    }
    u32 ino = 0;
    status = create_inode(handle.volume, (u16)(EXT2_S_IFREG | (mode & 0777)), 1, &ino);
    if (status != AXFS_OK) {
        return status;
    }
    status = ext2_dir_add(handle.volume, &handle, name, ino, EXT2_FT_REG_FILE);
    if (status != AXFS_OK) {
        ext2_free_inode(handle.volume, ino);
        return status;
    }
    return ext2_lookup_op(dir, name, out);
}

static i32 ext2_mkdir_op(axfs_inode_t *dir, const char *name, u32 mode,
                         axfs_inode_t **out)
{
    if (!dir || !name) {
        return AXFS_EINVAL;
    }
    ext2_handle_t handle;
    i32 status = load_handle(dir, &handle);
    if (status != AXFS_OK) {
        return status;
    }
    if (handle.volume->read_only) {
        return AXFS_EROFS;
    }
    u32 existing = 0;
    u8 type = 0;
    if (ext2_dir_lookup(handle.volume, &handle, name, &existing, &type) == AXFS_OK) {
        return AXFS_EEXIST;
    }
    u32 ino = 0;
    status = create_inode(handle.volume, (u16)(EXT2_S_IFDIR | (mode & 0777)), 2, &ino);
    if (status != AXFS_OK) {
        return status;
    }
    status = ext2_dir_init(handle.volume, ino, handle.ino);
    if (status != AXFS_OK) {
        ext2_free_inode(handle.volume, ino);
        return status;
    }
    status = ext2_dir_add(handle.volume, &handle, name, ino, EXT2_FT_DIR);
    if (status != AXFS_OK) {
        ext2_free_inode(handle.volume, ino);
        return status;
    }
    /* The parent gains a link for its new child directory. */
    handle.inode.links_count++;
    ext2_write_inode(handle.volume, handle.ino, &handle.inode);
    return ext2_lookup_op(dir, name, out);
}

static i32 ext2_unlink_op(axfs_inode_t *dir, const char *name)
{
    if (!dir || !name) {
        return AXFS_EINVAL;
    }
    ext2_handle_t handle;
    i32 status = load_handle(dir, &handle);
    if (status != AXFS_OK) {
        return status;
    }
    if (handle.volume->read_only) {
        return AXFS_EROFS;
    }
    u32 ino = 0;
    u8 type = 0;
    status = ext2_dir_lookup(handle.volume, &handle, name, &ino, &type);
    if (status != AXFS_OK) {
        return status;
    }
    ext2_inode_t child;
    status = ext2_read_inode(handle.volume, ino, &child);
    if (status != AXFS_OK) {
        return status;
    }
    if (child.mode & EXT2_S_IFDIR) {
        return AXFS_EISDIR;
    }
    status = ext2_dir_remove(handle.volume, &handle, name);
    if (status != AXFS_OK) {
        return status;
    }
    if (child.links_count > 1) {
        child.links_count--;
        ext2_write_inode(handle.volume, ino, &child);
        return AXFS_OK;
    }
    ext2_truncate_blocks(handle.volume, &child);
    child.dtime = (u32)axfs_now();
    child.links_count = 0;
    child.size = 0;
    ext2_write_inode(handle.volume, ino, &child);
    return ext2_free_inode(handle.volume, ino);
}

static i32 ext2_rmdir_op(axfs_inode_t *dir, const char *name)
{
    if (!dir || !name) {
        return AXFS_EINVAL;
    }
    ext2_handle_t handle;
    i32 status = load_handle(dir, &handle);
    if (status != AXFS_OK) {
        return status;
    }
    if (handle.volume->read_only) {
        return AXFS_EROFS;
    }
    u32 ino = 0;
    u8 type = 0;
    status = ext2_dir_lookup(handle.volume, &handle, name, &ino, &type);
    if (status != AXFS_OK) {
        return status;
    }
    ext2_inode_t child;
    status = ext2_read_inode(handle.volume, ino, &child);
    if (status != AXFS_OK) {
        return status;
    }
    if (!(child.mode & EXT2_S_IFDIR)) {
        return AXFS_ENOTDIR;
    }
    ext2_handle_t child_handle;
    ax_memset(&child_handle, 0, sizeof(child_handle));
    child_handle.volume = handle.volume;
    child_handle.ino = ino;
    child_handle.inode = child;

    /* Refuse when anything other than `.` and `..` is present. */
    u32 index = 0;
    char entry[EXT2_DIRENT_NAME_MAX + 1];
    while (ext2_dir_list(handle.volume, &child_handle, index, entry,
                          sizeof(entry), 0, 0) == AXFS_OK) {
        if (ax_strcmp(entry, ".") != 0 && ax_strcmp(entry, "..") != 0) {
            return AXFS_ENOTEMPTY;
        }
        index++;
    }
    status = ext2_dir_remove(handle.volume, &handle, name);
    if (status != AXFS_OK) {
        return status;
    }
    ext2_dir_merge(handle.volume, &child_handle);
    ext2_truncate_blocks(handle.volume, &child);
    child.links_count = 0;
    child.dtime = (u32)axfs_now();
    ext2_write_inode(handle.volume, ino, &child);
    ext2_free_inode(handle.volume, ino);
    if (handle.inode.links_count > 2) {
        handle.inode.links_count--;
        ext2_write_inode(handle.volume, handle.ino, &handle.inode);
    }
    return AXFS_OK;
}

static i32 ext2_rename_op(axfs_inode_t *old_dir, const char *old_name,
                           axfs_inode_t *new_dir, const char *new_name, u32 flags)
{
    (void)flags;
    if (!old_dir || !new_dir || !old_name || !new_name) {
        return AXFS_EINVAL;
    }
    ext2_handle_t old_handle;
    ext2_handle_t new_handle;
    i32 status = load_handle(old_dir, &old_handle);
    if (status != AXFS_OK) {
        return status;
    }
    status = load_handle(new_dir, &new_handle);
    if (status != AXFS_OK) {
        return status;
    }
    if (old_handle.volume != new_handle.volume) {
        return AXFS_EXDEV;
    }
    if (old_handle.volume->read_only) {
        return AXFS_EROFS;
    }
    u32 ino = 0;
    u8 type = 0;
    status = ext2_dir_lookup(old_handle.volume, &old_handle, old_name, &ino, &type);
    if (status != AXFS_OK) {
        return status;
    }
    if (ext2_dir_lookup(new_handle.volume, &new_handle, new_name, 0, 0) == AXFS_OK) {
        return AXFS_EEXIST;
    }
    u8 filetype = type ? type : (u8)EXT2_FT_REG_FILE;
    status = ext2_dir_add(new_handle.volume, &new_handle, new_name, ino, filetype);
    if (status != AXFS_OK) {
        return status;
    }
    status = ext2_dir_remove(old_handle.volume, &old_handle, old_name);
    return status;
}

void axfs_ext2_dir_install(axfs_inode_ops_t *ops)
{
    if (!ops) {
        return;
    }
    ops->lookup = ext2_lookup_op;
    ops->readdir = ext2_readdir_op;
    ops->create = ext2_create_op;
    ops->mkdir = ext2_mkdir_op;
    ops->unlink = ext2_unlink_op;
    ops->rmdir = ext2_rmdir_op;
    ops->rename = ext2_rename_op;
}

/* A self-test that walks the root directory: it exercises block mapping,
 * record iteration and name decoding in one pass. */
i32 axfs_ext2_selftest(void)
{
    axfs_mount_t *mount = axfs_mount_find("/ext");
    if (!mount || !mount->super) {
        return AXFS_ENOENT;
    }
    axfs_inode_t *root = mount->super->root;
    if (!root) {
        return AXFS_ENOENT;
    }
    ext2_handle_t handle;
    i32 status = load_handle(root, &handle);
    if (status != AXFS_OK) {
        return status;
    }
    char name[EXT2_DIRENT_NAME_MAX + 1];
    u32 index = 0;
    u32 count = 0;
    while (ext2_dir_list(handle.volume, &handle, index, name, sizeof(name), 0, 0)
           == AXFS_OK) {
        if (ax_strcmp(name, ".") != 0 && ax_strcmp(name, "..") != 0) {
            count++;
        }
        index++;
    }
    printk("ext2: selftest listed %u entries in /\n", count);
    return AXFS_OK;
}
