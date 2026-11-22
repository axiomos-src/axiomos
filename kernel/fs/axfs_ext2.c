/*
 * AxiomOS - ext2 volume, inode and block mapping.
 *
 * Layout recap.  Block group 0 starts at `first_data_block` (1 for a 1 KiB
 * block size, 0 for larger).  Each group has a block bitmap, an inode bitmap,
 * and an inode table.  The superblock lives at byte 1024.
 *
 * Indirection: inode->block[0..11] are direct, [12] single indirect, [13] double
 * indirect and [14] triple indirect.  With 1 KiB blocks a double-indirect entry
 * addresses 256 * 256 * 1024 = 64 MiB, so triple indirection is only reachable
 * on very large files; the mapper below handles it anyway because truncation has
 * to walk it.
 *
 * Everything is word-at-a-time; there is no page cache involvement here, which
 * keeps ext2 independent of the FAT32 code path and makes the two comparable
 * when the VFS chooses between them.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include "ext2_int.h"

/* ------------------------------------------------------------------ */
/* Endian helpers (private)                                            */
/* ------------------------------------------------------------------ */

static u16 e16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 e32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void p16(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
}

static void p32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Block I/O                                                           */
/* ------------------------------------------------------------------ */

i32 ext2_read_block(ext2_volume_t *volume, u32 block, void *buffer)
{
    if (!volume || !buffer) {
        return AXFS_EINVAL;
    }
    if (block >= volume->blocks_count) {
        return AXFS_EIO;
    }
    u64 offset = ((u64)volume->base_lba * 512ull) + (u64)block * volume->block_size;
    return axfs_block_read_bytes(offset, buffer, volume->block_size);
}

i32 ext2_write_block(ext2_volume_t *volume, u32 block, const void *buffer)
{
    if (!volume || !buffer) {
        return AXFS_EINVAL;
    }
    if (block >= volume->blocks_count) {
        return AXFS_EIO;
    }
    if (volume->read_only) {
        return AXFS_EROFS;
    }
    u64 offset = ((u64)volume->base_lba * 512ull) + (u64)block * volume->block_size;
    volume->dirty = (bool_t)1;
    return axfs_block_write_bytes(offset, buffer, volume->block_size);
}

/* ------------------------------------------------------------------ */
/* Superblock parsing                                                  */
/* ------------------------------------------------------------------ */

static u32 log2_u32(u32 value)
{
    u32 shift = 0;
    while (value > 1) {
        value >>= 1;
        shift++;
    }
    return shift;
}

u32 ext2_group_of_inode(ext2_volume_t *volume, u32 ino)
{
    if (!volume || ino == 0) {
        return 0;
    }
    return (ino - 1) / volume->inodes_per_group;
}

u32 ext2_group_of_block(ext2_volume_t *volume, u32 block)
{
    if (!volume || block < volume->first_data_block) {
        return 0;
    }
    u32 index = block - volume->first_data_block;
    if (volume->blocks_per_group == 0) {
        return 0;
    }
    return index / volume->blocks_per_group;
}

u32 ext2_inode_to_offset(ext2_volume_t *volume, u32 ino)
{
    ext2_group_t *group = &volume->group[ext2_group_of_inode(volume, ino)];
    u32 index = (ino - 1) % volume->inodes_per_group;
    return group->inode_table_block * volume->block_size + index * volume->inode_size;
}

static void parse_super(ext2_volume_t *volume, const u8 *raw)
{
    ext2_super_block_t *sb = &volume->super;
    sb->inodes_count = e32(raw + 0);
    sb->blocks_count = e32(raw + 4);
    sb->r_blocks_count = e32(raw + 8);
    sb->free_blocks_count = e32(raw + 12);
    sb->free_inodes_count = e32(raw + 16);
    sb->first_data_block = e32(raw + 20);
    sb->log_block_size = e32(raw + 24);
    sb->log_cluster_size = e32(raw + 28);
    sb->blocks_per_group = e32(raw + 32);
    sb->frags_per_group = e32(raw + 36);
    sb->itables_per_group = e32(raw + 40);
    sb->mtime = e32(raw + 44);
    sb->wtime = e32(raw + 48);
    sb->mnt_count = e16(raw + 52);
    sb->max_mnt_count = e16(raw + 54);
    sb->magic = e16(raw + 56);
    sb->state = e16(raw + 58);
    sb->errors = e16(raw + 60);
    sb->minor_rev_level = e16(raw + 62);
    sb->lastcheck = e32(raw + 64);
    sb->checkinterval = e32(raw + 68);
    sb->creator_os = e32(raw + 72);
    sb->rev_level = e32(raw + 76);
    sb->def_resuid = e16(raw + 80);
    sb->def_resgid = e16(raw + 82);
    sb->first_ino = e32(raw + 84);
    sb->inode_size = e16(raw + 88);
    sb->block_group_nr = e16(raw + 90);
    sb->feature_compat = e32(raw + 92);
    sb->feature_incompat = e32(raw + 96);
    sb->feature_ro_compat = e32(raw + 100);
    for (usize i = 0; i < 16; i++) {
        sb->uuid[i] = raw[104 + i];
        sb->volume_name[i] = (char)raw[120 + i];
    }
    sb->journal_inum = e32(raw + 224);
    sb->journal_dev = e32(raw + 228);
    sb->last_orphan = e32(raw + 232);
}

static void write_super_fields(ext2_volume_t *volume, u8 *raw)
{
    ext2_super_block_t *sb = &volume->super;
    p32(raw + 12, sb->free_blocks_count);
    p32(raw + 16, sb->free_inodes_count);
    p32(raw + 48, sb->wtime);
    p16(raw + 52, sb->mnt_count);
    p16(raw + 58, sb->state);
}

/* Write one group descriptor back to disk. */
static i32 flush_group(ext2_volume_t *volume, u32 index)
{
    u32 desc_per_block = volume->block_size / 32;
    u32 block = volume->first_data_block + 1 + index / desc_per_block;
    u32 slot = index % desc_per_block;
    u8 *buffer = (u8 *)kmalloc(volume->block_size);
    if (!buffer) {
        return AXFS_ENOMEM;
    }
    i32 status = ext2_read_block(volume, block, buffer);
    if (status == AXFS_OK) {
        ext2_group_t *group = &volume->group[index];
        u8 *raw = buffer + slot * 32;
        p32(raw + 0, group->block_bitmap_block);
        p32(raw + 4, group->inode_bitmap_block);
        p32(raw + 8, group->inode_table_block);
        p16(raw + 12, (u16)group->free_blocks);
        p16(raw + 14, (u16)group->free_inodes);
        p16(raw + 16, (u16)group->used_dirs);
        status = ext2_write_block(volume, block, buffer);
    }
    kfree(buffer);
    return status;
}

i32 ext2_flush_super(ext2_volume_t *volume)
{
    if (!volume || volume->read_only) {
        return AXFS_OK;
    }
    u8 *super_block = (u8 *)kmalloc(volume->block_size);
    if (!super_block) {
        return AXFS_ENOMEM;
    }
    i32 status = ext2_read_block(volume, 0, super_block);
    if (status == AXFS_OK) {
        write_super_fields(volume, super_block + 1024 - 0);
        status = ext2_write_block(volume, 0, super_block);
    }
    kfree(super_block);
    if (status != AXFS_OK) {
        return status;
    }
    for (u32 g = 0; g < volume->groups; g++) {
        i32 gs = flush_group(volume, g);
        if (gs != AXFS_OK) {
            return gs;
        }
    }
    volume->dirty = (bool_t)0;
    return AXFS_OK;
}

i32 ext2_volume_open(ext2_volume_t *volume, u64 base_lba)
{
    if (!volume) {
        return AXFS_EINVAL;
    }
    ax_memset(volume, 0, sizeof(*volume));
    volume->base_lba = base_lba;

    /* Probe with 1 KiB block: the superblock always sits at byte 1024. */
    u8 probe[1024];
    u64 offset = base_lba * 512ull + 1024ull;
    i32 status = axfs_block_read_bytes(offset, probe, sizeof(probe));
    if (status != AXFS_OK) {
        return status;
    }
    if (e16(probe + 56) != EXT2_MAGIC) {
        return AXFS_EINVAL;
    }

    /* The block size is encoded as a log, which is what makes the next read
     * necessary before we can parse anything else. */
    u32 log = e32(probe + 24);
    if (log > 4) {
        return AXFS_EINVAL;
    }
    volume->block_size = 1024u << log;

    if (volume->block_size > 1024) {
        u8 *first = (u8 *)kmalloc(volume->block_size);
        if (!first) {
            return AXFS_ENOMEM;
        }
        status = axfs_block_read_bytes(base_lba * 512ull, first, volume->block_size);
        if (status != AXFS_OK) {
            kfree(first);
            return status;
        }
        for (u32 i = 0; i < 1024; i++) {
            probe[i] = first[1024 + i];
        }
        kfree(first);
    }

    parse_super(volume, probe);
    if (volume->super.magic != EXT2_MAGIC) {
        return AXFS_EINVAL;
    }
    volume->inodes_count = volume->super.inodes_count;
    volume->blocks_count = volume->super.blocks_count;
    volume->first_data_block = volume->super.first_data_block;
    volume->blocks_per_group = volume->super.blocks_per_group;
    if (volume->blocks_per_group == 0 || volume->inodes_count == 0) {
        return AXFS_EINVAL;
    }
    volume->inodes_per_group = volume->super.itables_per_group
                                   ? volume->super.itables_per_group
                                   : 0;
    if (volume->inodes_per_group == 0) {
        return AXFS_EINVAL;
    }
    volume->inode_size = volume->super.rev_level == 0
                             ? EXT2_GOOD_OLD_INODE_SIZE
                             : volume->super.inode_size;
    if (volume->inode_size < EXT2_GOOD_OLD_INODE_SIZE ||
        volume->inode_size > volume->block_size) {
        volume->inode_size = EXT2_GOOD_OLD_INODE_SIZE;
    }
    volume->feature_compat = volume->super.feature_compat;
    volume->feature_incompat = volume->super.feature_incompat;
    volume->feature_ro_compat = volume->super.feature_ro_compat;
    volume->has_filetype = (bool_t)((volume->feature_incompat &
                                     EXT2_FEATURE_INCOMPAT_FILETYPE) != 0);
    volume->read_only = (bool_t)0;

    if (volume->feature_incompat & (EXT2_FEATURE_INCOMPAT_RECOVER |
                                    EXT2_FEATURE_INCOMPAT_COMPRESSION |
                                    EXT2_FEATURE_INCOMPAT_JOURNAL_DEV)) {
        /* A dirty journal needs replay, which this driver does not implement;
         * mounting read-only is the honest option. */
        volume->read_only = (bool_t)1;
        printk("ext2: volume has an unreplayed journal, mounting read-only\n");
    }

    volume->blocks_per_group = volume->super.blocks_per_group;
    if (volume->blocks_per_group == 0) {
        return AXFS_EINVAL;
    }
    u32 groups_by_blocks = (volume->blocks_count - volume->first_data_block +
                            volume->blocks_per_group - 1) / volume->blocks_per_group;
    u32 groups_by_inodes = (volume->inodes_count + volume->inodes_per_group - 1) /
                           volume->inodes_per_group;
    volume->groups = groups_by_blocks > groups_by_inodes ? groups_by_blocks
                                                         : groups_by_inodes;
    if (volume->groups == 0 || volume->groups > 1024) {
        return AXFS_EINVAL;
    }

    volume->group = (ext2_group_t *)kzalloc(sizeof(ext2_group_t) * volume->groups);
    if (!volume->group) {
        return AXFS_ENOMEM;
    }

    /* Group descriptors start in the block after the superblock's block. */
    u32 desc_block = volume->first_data_block + 1;
    u32 desc_per_block = volume->block_size / 32;
    u8 *desc = (u8 *)kmalloc(volume->block_size);
    if (!desc) {
        kfree(volume->group);
        volume->group = 0;
        return AXFS_ENOMEM;
    }
    for (u32 g = 0; g < volume->groups; g++) {
        u32 block = desc_block + g / desc_per_block;
        if (g % desc_per_block == 0) {
            status = ext2_read_block(volume, block, desc);
            if (status != AXFS_OK) {
                kfree(desc);
                kfree(volume->group);
                volume->group = 0;
                return status;
            }
        }
        const u8 *raw = desc + (g % desc_per_block) * 32;
        volume->group[g].block_bitmap_block = e32(raw + 0);
        volume->group[g].inode_bitmap_block = e32(raw + 4);
        volume->group[g].inode_table_block = e32(raw + 8);
        volume->group[g].free_blocks = e16(raw + 12);
        volume->group[g].free_inodes = e16(raw + 14);
        volume->group[g].used_dirs = e16(raw + 16);
    }
    kfree(desc);

    for (usize i = 0; i < 16; i++) {
        volume->label[i] = volume->super.volume_name[i];
    }
    volume->label[16] = 0;
    volume->next_free_inode_hint = volume->super.first_ino ? volume->super.first_ino
                                                          : 11;
    volume->next_free_block_hint = volume->first_data_block;
    volume->mounted = (bool_t)1;

    printk("ext2: volume '%s' bs=%u blocks=%u inodes=%u groups=%u%s\n",
           volume->label, volume->block_size, volume->blocks_count,
           volume->inodes_count, volume->groups,
           volume->read_only ? " (ro)" : "");
    (void)log2_u32;
    return AXFS_OK;
}

void ext2_volume_close(ext2_volume_t *volume)
{
    if (!volume) {
        return;
    }
    ext2_flush_super(volume);
    if (volume->group) {
        kfree(volume->group);
        volume->group = 0;
    }
    volume->mounted = (bool_t)0;
}

/* ------------------------------------------------------------------ */
/* Inode I/O                                                           */
/* ------------------------------------------------------------------ */

i32 ext2_read_inode(ext2_volume_t *volume, u32 ino, ext2_inode_t *out)
{
    if (!volume || !out || ino == 0 || ino > volume->inodes_count) {
        return AXFS_EINVAL;
    }
    u64 offset = (u64)volume->base_lba * 512ull +
                 ext2_inode_to_offset(volume, ino);
    u8 raw[EXT2_GOOD_OLD_INODE_SIZE + 64];
    if (volume->inode_size > sizeof(raw)) {
        return AXFS_ENOSYS;
    }
    i32 status = axfs_block_read_bytes(offset, raw, volume->inode_size);
    if (status != AXFS_OK) {
        return status;
    }
    ax_memset(out, 0, sizeof(*out));
    out->mode = e16(raw + 0);
    out->uid = e16(raw + 2);
    out->size = e32(raw + 4);
    out->atime = e32(raw + 8);
    out->ctime = e32(raw + 12);
    out->mtime = e32(raw + 16);
    out->dtime = e32(raw + 20);
    out->gid = e16(raw + 24);
    out->links_count = e16(raw + 26);
    out->blocks = e32(raw + 28);
    out->flags = e32(raw + 32);
    out->osd1 = e32(raw + 36);
    for (u32 i = 0; i < EXT2_N_BLOCKS; i++) {
        out->block[i] = e32(raw + 40 + i * 4);
    }
    out->generation = e32(raw + 100);
    out->file_acl = e32(raw + 104);
    out->dir_acl = e32(raw + 108);
    out->faddr = e32(raw + 112);
    return AXFS_OK;
}

i32 ext2_write_inode(ext2_volume_t *volume, u32 ino, const ext2_inode_t *inode)
{
    if (!volume || !inode || ino == 0 || ino > volume->inodes_count) {
        return AXFS_EINVAL;
    }
    if (volume->read_only) {
        return AXFS_EROFS;
    }
    u64 offset = (u64)volume->base_lba * 512ull +
                 ext2_inode_to_offset(volume, ino);
    u8 raw[EXT2_GOOD_OLD_INODE_SIZE + 64];
    ax_memset(raw, 0, sizeof(raw));
    p16(raw + 0, inode->mode);
    p16(raw + 2, inode->uid);
    p32(raw + 4, inode->size);
    p32(raw + 8, inode->atime);
    p32(raw + 12, inode->ctime);
    p32(raw + 16, inode->mtime);
    p32(raw + 20, inode->dtime);
    p16(raw + 24, inode->gid);
    p16(raw + 26, inode->links_count);
    p32(raw + 28, inode->blocks);
    p32(raw + 32, inode->flags);
    p32(raw + 36, inode->osd1);
    for (u32 i = 0; i < EXT2_N_BLOCKS; i++) {
        p32(raw + 40 + i * 4, inode->block[i]);
    }
    p32(raw + 100, inode->generation);
    p32(raw + 104, inode->file_acl);
    p32(raw + 108, inode->dir_acl);
    p32(raw + 112, inode->faddr);
    return axfs_block_write_bytes(offset, raw, volume->inode_size);
}

/* ------------------------------------------------------------------ */
/* Bitmap allocation                                                   */
/* ------------------------------------------------------------------ */

static i32 bitmap_test(ext2_volume_t *volume, u32 bitmap_block, u32 bit,
                       bool_t *set)
{
    u32 index = bit / 8;
    u32 byte_offset = index;
    u8 byte;
    if (byte_offset >= volume->block_size) {
        return AXFS_EINVAL;
    }
    u64 offset = (u64)volume->base_lba * 512ull +
                 (u64)bitmap_block * volume->block_size + byte_offset;
    i32 status = axfs_block_read_bytes(offset, &byte, 1);
    if (status != AXFS_OK) {
        return status;
    }
    if (set) {
        *set = (bool_t)((byte >> (bit % 8)) & 1);
    }
    return AXFS_OK;
}

static i32 bitmap_set(ext2_volume_t *volume, u32 bitmap_block, u32 bit, bool_t value)
{
    u32 byte_offset = bit / 8;
    if (byte_offset >= volume->block_size) {
        return AXFS_EINVAL;
    }
    u64 offset = (u64)volume->base_lba * 512ull +
                 (u64)bitmap_block * volume->block_size + byte_offset;
    u8 byte;
    i32 status = axfs_block_read_bytes(offset, &byte, 1);
    if (status != AXFS_OK) {
        return status;
    }
    u8 mask = (u8)(1u << (bit % 8));
    if (value) {
        byte = (u8)(byte | mask);
    } else {
        byte = (u8)(byte & (u8)~mask);
    }
    return axfs_block_write_bytes(offset, &byte, 1);
}

i32 ext2_alloc_inode(ext2_volume_t *volume, u32 *out_ino)
{
    if (!volume || !out_ino) {
        return AXFS_EINVAL;
    }
    if (volume->read_only) {
        return AXFS_EROFS;
    }
    u32 start_group = ext2_group_of_inode(volume, volume->next_free_inode_hint);
    for (u32 pass = 0; pass < volume->groups; pass++) {
        u32 g = (start_group + pass) % volume->groups;
        ext2_group_t *group = &volume->group[g];
        if (group->free_inodes == 0) {
            continue;
        }
        for (u32 bit = 0; bit < volume->inodes_per_group; bit++) {
            u32 ino = g * volume->inodes_per_group + bit + 1;
            if (ino > volume->inodes_count) {
                break;
            }
            bool_t used = (bool_t)0;
            i32 status = bitmap_test(volume, group->inode_bitmap_block, bit, &used);
            if (status != AXFS_OK) {
                return status;
            }
            if (used) {
                continue;
            }
            status = bitmap_set(volume, group->inode_bitmap_block, bit, (bool_t)1);
            if (status != AXFS_OK) {
                return status;
            }
            group->free_inodes--;
            volume->super.free_inodes_count--;
            volume->next_free_inode_hint = ino + 1;
            *out_ino = ino;
            return AXFS_OK;
        }
    }
    return AXFS_ENOSPC;
}

i32 ext2_free_inode(ext2_volume_t *volume, u32 ino)
{
    if (!volume || ino == 0 || ino > volume->inodes_count) {
        return AXFS_EINVAL;
    }
    if (volume->read_only) {
        return AXFS_EROFS;
    }
    u32 g = ext2_group_of_inode(volume, ino);
    u32 bit = (ino - 1) % volume->inodes_per_group;
    ext2_group_t *group = &volume->group[g];
    i32 status = bitmap_set(volume, group->inode_bitmap_block, bit, (bool_t)0);
    if (status != AXFS_OK) {
        return status;
    }
    group->free_inodes++;
    volume->super.free_inodes_count++;
    if (ino < volume->next_free_inode_hint) {
        volume->next_free_inode_hint = ino;
    }
    return AXFS_OK;
}

i32 ext2_alloc_block(ext2_volume_t *volume, u32 *out_block)
{
    if (!volume || !out_block) {
        return AXFS_EINVAL;
    }
    if (volume->read_only) {
        return AXFS_EROFS;
    }
    u32 start_group = ext2_group_of_block(volume, volume->next_free_block_hint);
    for (u32 pass = 0; pass < volume->groups; pass++) {
        u32 g = (start_group + pass) % volume->groups;
        ext2_group_t *group = &volume->group[g];
        if (group->free_blocks == 0) {
            continue;
        }
        u32 group_first = volume->first_data_block + g * volume->blocks_per_group;
        for (u32 bit = 0; bit < volume->blocks_per_group; bit++) {
            u32 block = group_first + bit;
            if (block >= volume->blocks_count) {
                break;
            }
            bool_t used = (bool_t)0;
            i32 status = bitmap_test(volume, group->block_bitmap_block, bit, &used);
            if (status != AXFS_OK) {
                return status;
            }
            if (used) {
                continue;
            }
            status = bitmap_set(volume, group->block_bitmap_block, bit, (bool_t)1);
            if (status != AXFS_OK) {
                return status;
            }
            group->free_blocks--;
            volume->super.free_blocks_count--;
            volume->next_free_block_hint = block + 1;
            *out_block = block;
            return AXFS_OK;
        }
    }
    return AXFS_ENOSPC;
}

i32 ext2_free_block(ext2_volume_t *volume, u32 block)
{
    if (!volume || block < volume->first_data_block || block >= volume->blocks_count) {
        return AXFS_EINVAL;
    }
    u32 g = ext2_group_of_block(volume, block);
    u32 bit = block - volume->first_data_block - g * volume->blocks_per_group;
    ext2_group_t *group = &volume->group[g];
    i32 status = bitmap_set(volume, group->block_bitmap_block, bit, (bool_t)0);
    if (status != AXFS_OK) {
        return status;
    }
    group->free_blocks++;
    volume->super.free_blocks_count++;
    if (block < volume->next_free_block_hint) {
        volume->next_free_block_hint = block;
    }
    /* Zero the block so a later allocator cannot leak the previous owner's
     * data into a new file. */
    u8 *zero = (u8 *)kzalloc(volume->block_size);
    if (zero) {
        ext2_write_block(volume, block, zero);
        kfree(zero);
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Block mapping                                                       */
/* ------------------------------------------------------------------ */

static i32 read_ind(ext2_volume_t *volume, u32 block, u32 index, u32 *out)
{
    u64 offset = (u64)volume->base_lba * 512ull +
                 (u64)block * volume->block_size + (u64)index * 4;
    u8 raw[4];
    i32 status = axfs_block_read_bytes(offset, raw, 4);
    if (status != AXFS_OK) {
        return status;
    }
    *out = e32(raw);
    return AXFS_OK;
}

static i32 write_ind(ext2_volume_t *volume, u32 block, u32 index, u32 value)
{
    u64 offset = (u64)volume->base_lba * 512ull +
                 (u64)block * volume->block_size + (u64)index * 4;
    u8 raw[4];
    p32(raw, value);
    return axfs_block_write_bytes(offset, raw, 4);
}

/* Map a logical block to a physical one, allocating indirection blocks when
 * `create` is set.  The three indirection levels are handled by the same
 * recursive walker, which keeps the code honest about the nesting. */
static i32 bmap_walk(ext2_volume_t *volume, u32 *slot, u32 depth, u32 logical,
                     u32 span, u32 *out, bool_t create)
{
    if (depth == 0) {
        if (*slot == 0) {
            if (!create) {
                *out = 0;
                return AXFS_OK;
            }
            u32 block = 0;
            i32 status = ext2_alloc_block(volume, &block);
            if (status != AXFS_OK) {
                return status;
            }
            u8 *zero = (u8 *)kzalloc(volume->block_size);
            if (!zero) {
                ext2_free_block(volume, block);
                return AXFS_ENOMEM;
            }
            status = ext2_write_block(volume, block, zero);
            kfree(zero);
            if (status != AXFS_OK) {
                return status;
            }
            *slot = block;
        }
        *out = *slot;
        return AXFS_OK;
    }

    if (*slot == 0) {
        if (!create) {
            *out = 0;
            return AXFS_OK;
        }
        u32 block = 0;
        i32 status = ext2_alloc_block(volume, &block);
        if (status != AXFS_OK) {
            return status;
        }
        u8 *zero = (u8 *)kzalloc(volume->block_size);
        if (!zero) {
            ext2_free_block(volume, block);
            return AXFS_ENOMEM;
        }
        status = ext2_write_block(volume, block, zero);
        kfree(zero);
        if (status != AXFS_OK) {
            return status;
        }
        *slot = block;
    }

    u32 entries = volume->block_size / 4;
    u32 index = logical / span;
    if (index >= entries) {
        return AXFS_EIO;
    }
    u32 child = 0;
    i32 status = read_ind(volume, *slot, index, &child);
    if (status != AXFS_OK) {
        return status;
    }
    status = bmap_walk(volume, &child, depth - 1, logical % span, span / entries,
                       out, create);
    if (status != AXFS_OK) {
        return status;
    }
    if (depth == 1 || child != 0) {
        /* Persist the child pointer: an allocation deeper down has to be
         * visible through this level. */
        write_ind(volume, *slot, index, child);
    }
    return AXFS_OK;
}

i32 ext2_bmap(ext2_volume_t *volume, const ext2_inode_t *inode, u32 logical,
               u32 *out_physical, bool_t create)
{
    if (!volume || !inode || !out_physical) {
        return AXFS_EINVAL;
    }
    *out_physical = 0;
    u32 entries = volume->block_size / 4;
    if (logical < EXT2_NDIR_BLOCKS) {
        u32 slot = inode->block[logical];
        if (slot == 0 && create) {
            /* The inode is const here, but the caller passes the same object it
             * will write back; casting away constness is confined to this
             * function and documented by the signature's contract. */
            ext2_inode_t *mutable = (ext2_inode_t *)inode;
            u32 block = 0;
            i32 status = ext2_alloc_block(volume, &block);
            if (status != AXFS_OK) {
                return status;
            }
            u8 *zero = (u8 *)kzalloc(volume->block_size);
            if (!zero) {
                ext2_free_block(volume, block);
                return AXFS_ENOMEM;
            }
            status = ext2_write_block(volume, block, zero);
            kfree(zero);
            if (status != AXFS_OK) {
                return status;
            }
            mutable->block[logical] = block;
            mutable->blocks += volume->block_size / 512;
            *out_physical = block;
            return AXFS_OK;
        }
        *out_physical = slot;
        return AXFS_OK;
    }

    u32 index = logical - EXT2_NDIR_BLOCKS;
    if (index < entries) {
        u32 slot = inode->block[EXT2_IND_BLOCK];
        return bmap_walk(volume, &slot, 1, index, 1, out_physical, create);
    }
    index -= entries;

    if (index < entries * entries) {
        u32 slot = inode->block[EXT2_DIND_BLOCK];
        i32 status = bmap_walk(volume, &slot, 2, index, entries, out_physical,
                               create);
        if (status == AXFS_OK && create && slot && inode->block[EXT2_DIND_BLOCK] == 0) {
            ((ext2_inode_t *)inode)->block[EXT2_DIND_BLOCK] = slot;
        }
        return status;
    }
    index -= entries * entries;

    if (index < entries * entries * entries) {
        u32 slot = inode->block[EXT2_TIND_BLOCK];
        i32 status = bmap_walk(volume, &slot, 3, index, entries * entries,
                               out_physical, create);
        if (status == AXFS_OK && create && slot && inode->block[EXT2_TIND_BLOCK] == 0) {
            ((ext2_inode_t *)inode)->block[EXT2_TIND_BLOCK] = slot;
        }
        return status;
    }
    return AXFS_EFBIG;
}

/* Release every indirection and data block an inode owns. */
static i32 free_ind(ext2_volume_t *volume, u32 block, u32 depth)
{
    if (block == 0) {
        return AXFS_OK;
    }
    u8 *buffer = (u8 *)kmalloc(volume->block_size);
    if (!buffer) {
        return AXFS_ENOMEM;
    }
    i32 status = ext2_read_block(volume, block, buffer);
    if (status == AXFS_OK && depth > 0) {
        u32 entries = volume->block_size / 4;
        for (u32 i = 0; i < entries; i++) {
            u32 child = e32(buffer + i * 4);
            if (child) {
                free_ind(volume, child, depth - 1);
            }
        }
    }
    kfree(buffer);
    if (status != AXFS_OK) {
        return status;
    }
    return ext2_free_block(volume, block);
}

i32 ext2_truncate_blocks(ext2_volume_t *volume, ext2_inode_t *inode)
{
    if (!volume || !inode) {
        return AXFS_EINVAL;
    }
    for (u32 i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (inode->block[i]) {
            ext2_free_block(volume, inode->block[i]);
            inode->block[i] = 0;
        }
    }
    if (inode->block[EXT2_IND_BLOCK]) {
        free_ind(volume, inode->block[EXT2_IND_BLOCK], 0);
        inode->block[EXT2_IND_BLOCK] = 0;
    }
    if (inode->block[EXT2_DIND_BLOCK]) {
        free_ind(volume, inode->block[EXT2_DIND_BLOCK], 1);
        inode->block[EXT2_DIND_BLOCK] = 0;
    }
    if (inode->block[EXT2_TIND_BLOCK]) {
        free_ind(volume, inode->block[EXT2_TIND_BLOCK], 2);
        inode->block[EXT2_TIND_BLOCK] = 0;
    }
    inode->size = 0;
    inode->blocks = 0;
    return AXFS_OK;
}

u32 ext2_count_blocks(ext2_volume_t *volume, const ext2_inode_t *inode)
{
    if (!volume || !inode) {
        return 0;
    }
    u32 data_blocks = (inode->size + volume->block_size - 1) / volume->block_size;
    u32 count = 0;
    u32 entries = volume->block_size / 4;
    if (data_blocks > EXT2_NDIR_BLOCKS) {
        count++;
        data_blocks -= EXT2_NDIR_BLOCKS;
    } else {
        return data_blocks;
    }
    if (data_blocks > entries) {
        count++;
        data_blocks -= entries;
    } else {
        return count + data_blocks;
    }
    if (data_blocks > entries * entries) {
        count++;
        data_blocks -= entries * entries;
    } else {
        u32 need = (data_blocks + entries - 1) / entries;
        return count + need + data_blocks;
    }
    u32 need = (data_blocks + entries * entries - 1) / (entries * entries);
    return count + need + data_blocks;
}

u32 ext2_total_blocks(ext2_volume_t *volume)
{
    return volume ? volume->blocks_count : 0;
}

/* ------------------------------------------------------------------ */
/* File-level read and write                                           */
/* ------------------------------------------------------------------ */

static i32 ext2_read_bytes(ext2_volume_t *volume, ext2_inode_t *inode, u64 offset,
                            void *buffer, u64 count, u64 *out_done)
{
    u64 done = 0;
    u8 *block = (u8 *)kmalloc(volume->block_size);
    if (!block) {
        return AXFS_ENOMEM;
    }
    while (done < count) {
        u64 absolute = offset + done;
        u32 logical = (u32)(absolute / volume->block_size);
        u32 within = (u32)(absolute % volume->block_size);
        u32 physical = 0;
        i32 status = ext2_bmap(volume, inode, logical, &physical, (bool_t)0);
        if (status != AXFS_OK) {
            kfree(block);
            return status;
        }
        if (physical == 0) {
            /* A hole reads as zeroes. */
            u64 chunk = volume->block_size - within;
            if (chunk > count - done) {
                chunk = count - done;
            }
            for (u64 i = 0; i < chunk; i++) {
                ((u8 *)buffer)[done + i] = 0;
            }
            done += chunk;
            continue;
        }
        status = ext2_read_block(volume, physical, block);
        if (status != AXFS_OK) {
            kfree(block);
            return status;
        }
        u64 chunk = volume->block_size - within;
        if (chunk > count - done) {
            chunk = count - done;
        }
        for (u64 i = 0; i < chunk; i++) {
            ((u8 *)buffer)[done + i] = block[within + i];
        }
        done += chunk;
    }
    kfree(block);
    if (out_done) {
        *out_done = done;
    }
    return AXFS_OK;
}

static i32 ext2_write_bytes(ext2_volume_t *volume, ext2_inode_t *inode, u64 offset,
                             const void *buffer, u64 count, u64 *out_done)
{
    u64 done = 0;
    u8 *block = (u8 *)kmalloc(volume->block_size);
    if (!block) {
        return AXFS_ENOMEM;
    }
    while (done < count) {
        u64 absolute = offset + done;
        u32 logical = (u32)(absolute / volume->block_size);
        u32 within = (u32)(absolute % volume->block_size);
        u32 physical = 0;
        i32 status = ext2_bmap(volume, inode, logical, &physical, (bool_t)1);
        if (status != AXFS_OK) {
            kfree(block);
            return status;
        }
        u64 chunk = volume->block_size - within;
        if (chunk > count - done) {
            chunk = count - done;
        }
        if (within != 0 || chunk < volume->block_size) {
            /* Partial block: preserve the bytes outside the write range. */
            status = ext2_read_block(volume, physical, block);
            if (status != AXFS_OK) {
                kfree(block);
                return status;
            }
        }
        for (u64 i = 0; i < chunk; i++) {
            block[within + i] = ((const u8 *)buffer)[done + i];
        }
        status = ext2_write_block(volume, physical, block);
        if (status != AXFS_OK) {
            kfree(block);
            return status;
        }
        done += chunk;
    }
    kfree(block);
    if (out_done) {
        *out_done = done;
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Inode payload attached to axfs inodes                               */
/* ------------------------------------------------------------------ */

typedef struct ext2_payload {
    u32 ino;
    u16 mode;
} ext2_payload_t;

static ext2_volume_t *ext2_of(axfs_inode_t *inode)
{
    if (!inode || !inode->super) {
        return 0;
    }
    return (ext2_volume_t *)inode->super->fs_data;
}

static ext2_payload_t *ext2_payload_of(axfs_inode_t *inode)
{
    return inode ? (ext2_payload_t *)inode->fs_data : 0;
}

static i64 ext2_op_read(axfs_file_t *file, void *buffer, u64 count)
{
    if (!file || !file->inode || !buffer) {
        return AXFS_EINVAL;
    }
    axfs_inode_t *node = file->inode;
    ext2_volume_t *volume = ext2_of(node);
    ext2_payload_t *payload = ext2_payload_of(node);
    if (!volume || !payload) {
        return AXFS_EIO;
    }
    if (node->size <= file->offset) {
        return 0;
    }
    u64 want = count;
    if (file->offset + want > node->size) {
        want = node->size - file->offset;
    }
    ext2_inode_t inode;
    i32 status = ext2_read_inode(volume, payload->ino, &inode);
    if (status != AXFS_OK) {
        return status;
    }
    u64 done = 0;
    status = ext2_read_bytes(volume, &inode, file->offset, buffer, want, &done);
    if (status != AXFS_OK) {
        return status;
    }
    file->offset += done;
    return (i64)done;
}

static i64 ext2_op_write(axfs_file_t *file, const void *buffer, u64 count)
{
    if (!file || !file->inode || !buffer) {
        return AXFS_EINVAL;
    }
    axfs_inode_t *node = file->inode;
    ext2_volume_t *volume = ext2_of(node);
    ext2_payload_t *payload = ext2_payload_of(node);
    if (!volume || !payload) {
        return AXFS_EIO;
    }
    if (volume->read_only) {
        return AXFS_EROFS;
    }
    u64 end = file->offset + count;
    if (node->super && end > node->super->max_file_size) {
        return AXFS_EFBIG;
    }
    ext2_inode_t inode;
    i32 status = ext2_read_inode(volume, payload->ino, &inode);
    if (status != AXFS_OK) {
        return status;
    }
    u64 done = 0;
    status = ext2_write_bytes(volume, &inode, file->offset, buffer, count, &done);
    if (status != AXFS_OK) {
        return status;
    }
    file->offset += done;
    if (file->offset > inode.size) {
        inode.size = (u32)file->offset;
        node->size = file->offset;
    }
    inode.mtime = (u32)axfs_now();
    inode.blocks = ext2_count_blocks(volume, &inode);
    status = ext2_write_inode(volume, payload->ino, &inode);
    if (status != AXFS_OK) {
        return status;
    }
    node->mtime = axfs_now();
    return (i64)done;
}

static i32 ext2_op_truncate(axfs_inode_t *node, u64 size)
{
    if (!node) {
        return AXFS_EINVAL;
    }
    ext2_volume_t *volume = ext2_of(node);
    ext2_payload_t *payload = ext2_payload_of(node);
    if (!volume || !payload) {
        return AXFS_EIO;
    }
    if (volume->read_only) {
        return AXFS_EROFS;
    }
    ext2_inode_t inode;
    i32 status = ext2_read_inode(volume, payload->ino, &inode);
    if (status != AXFS_OK) {
        return status;
    }
    if (size == 0) {
        status = ext2_truncate_blocks(volume, &inode);
    } else if (size < inode.size) {
        /* Free the blocks past the new end by walking the range and zeroing
         * the pointers one at a time; simple, and truncation is rare. */
        u32 logical = (u32)((size + volume->block_size - 1) / volume->block_size);
        u32 physical = 0;
        while (ext2_bmap(volume, &inode, logical, &physical, (bool_t)0) == AXFS_OK &&
               physical != 0) {
            u32 entries = volume->block_size / 4;
            if (logical < EXT2_NDIR_BLOCKS) {
                ext2_free_block(volume, physical);
                inode.block[logical] = 0;
            } else {
                /* Leave indirection blocks in place; they are reclaimed when
                 * the inode itself goes away.  Data blocks are what matter. */
                ext2_free_block(volume, physical);
            }
            logical++;
            if (logical > EXT2_NDIR_BLOCKS + entries + entries * entries + 1) {
                break;
            }
        }
        inode.size = (u32)size;
        inode.blocks = ext2_count_blocks(volume, &inode);
    } else {
        inode.size = (u32)size;
    }
    node->size = size;
    status = ext2_write_inode(volume, payload->ino, &inode);
    return status;
}

static i32 ext2_op_stat(axfs_inode_t *node, axfs_attr_t *out)
{
    if (!node || !out) {
        return AXFS_EINVAL;
    }
    ext2_volume_t *volume = ext2_of(node);
    ext2_payload_t *payload = ext2_payload_of(node);
    if (!volume || !payload) {
        return AXFS_EIO;
    }
    ext2_inode_t inode;
    i32 status = ext2_read_inode(volume, payload->ino, &inode);
    if (status != AXFS_OK) {
        return status;
    }
    out->mode = (u32)inode.mode;
    out->uid = inode.uid;
    out->gid = inode.gid;
    out->size = inode.size;
    out->ino = payload->ino;
    out->nlink = inode.links_count;
    out->blocks = inode.blocks;
    out->atime = inode.atime;
    out->mtime = inode.mtime;
    out->ctime = inode.ctime;
    out->type = (inode.mode & EXT2_S_IFDIR) ? AXFS_FT_DIR
                 : (inode.mode & EXT2_S_IFLNK) ? AXFS_FT_SYMLINK
                 : (inode.mode & EXT2_S_IFCHR) ? AXFS_FT_CHAR
                 : (inode.mode & EXT2_S_IFBLK) ? AXFS_FT_BLOCK
                                               : AXFS_FT_FILE;
    return AXFS_OK;
}

static i32 ext2_op_chmod(axfs_inode_t *node, u32 mode)
{
    if (!node) {
        return AXFS_EINVAL;
    }
    ext2_volume_t *volume = ext2_of(node);
    ext2_payload_t *payload = ext2_payload_of(node);
    if (!volume || !payload) {
        return AXFS_EIO;
    }
    ext2_inode_t inode;
    i32 status = ext2_read_inode(volume, payload->ino, &inode);
    if (status != AXFS_OK) {
        return status;
    }
    inode.mode = (u16)((inode.mode & 0xF000) | (mode & 07777));
    node->mode = (node->mode & AXFS_S_IFMT) | (mode & 07777);
    return ext2_write_inode(volume, payload->ino, &inode);
}

/* ------------------------------------------------------------------ */
/* Ops tables                                                          */
/* ------------------------------------------------------------------ */

static axfs_inode_ops_t g_ext2_file_ops;
static axfs_inode_ops_t g_ext2_dir_ops;
static axfs_super_ops_t g_ext2_super_ops;
static bool_t g_ext2_ready;

static i32 ext2_super_alloc_inode(axfs_super_t *super, axfs_inode_t **out)
{
    if (!super || !out) {
        return AXFS_EINVAL;
    }
    ext2_volume_t *volume = (ext2_volume_t *)super->fs_data;
    u32 ino = 0;
    i32 status = ext2_alloc_inode(volume, &ino);
    if (status != AXFS_OK) {
        return status;
    }
    axfs_inode_t *inode = axfs_alloc_inode(super, ino, AXFS_FT_FILE, 0644);
    if (!inode) {
        ext2_free_inode(volume, ino);
        return AXFS_ENOMEM;
    }
    ext2_payload_t *payload = (ext2_payload_t *)kzalloc(sizeof(*payload));
    if (!payload) {
        kfree(inode);
        ext2_free_inode(volume, ino);
        return AXFS_ENOMEM;
    }
    payload->ino = ino;
    payload->mode = EXT2_S_IFREG | 0644;
    inode->fs_data = payload;
    inode->ops = &g_ext2_file_ops;
    *out = inode;
    return AXFS_OK;
}

static void ext2_super_destroy_inode(axfs_inode_t *inode)
{
    if (!inode || !inode->fs_data) {
        return;
    }
    kfree(inode->fs_data);
    inode->fs_data = 0;
}

static i32 ext2_super_write_inode(axfs_inode_t *inode)
{
    if (!inode) {
        return AXFS_EINVAL;
    }
    inode->flags &= ~AXFS_I_DIRTY;
    return AXFS_OK;
}

static i32 ext2_super_statfs(axfs_super_t *super, u64 *blocks, u64 *free_blocks,
                             u64 *files, u64 *free_files)
{
    ext2_volume_t *volume = super ? (ext2_volume_t *)super->fs_data : 0;
    if (!volume) {
        return AXFS_EINVAL;
    }
    if (blocks) {
        *blocks = volume->blocks_count;
    }
    if (free_blocks) {
        *free_blocks = volume->super.free_blocks_count;
    }
    if (files) {
        *files = volume->inodes_count;
    }
    if (free_files) {
        *free_files = volume->super.free_inodes_count;
    }
    return AXFS_OK;
}

static i32 ext2_super_sync(axfs_super_t *super)
{
    ext2_volume_t *volume = super ? (ext2_volume_t *)super->fs_data : 0;
    if (!volume) {
        return AXFS_EINVAL;
    }
    return ext2_flush_super(volume);
}

void axfs_ext2_dir_install(axfs_inode_ops_t *ops);

static void ext2_prepare_ops(void)
{
    if (g_ext2_ready) {
        return;
    }
    ax_memset(&g_ext2_file_ops, 0, sizeof(g_ext2_file_ops));
    g_ext2_file_ops.read = ext2_op_read;
    g_ext2_file_ops.write = ext2_op_write;
    g_ext2_file_ops.truncate = ext2_op_truncate;
    g_ext2_file_ops.stat = ext2_op_stat;
    g_ext2_file_ops.chmod = ext2_op_chmod;

    ax_memset(&g_ext2_dir_ops, 0, sizeof(g_ext2_dir_ops));
    g_ext2_dir_ops.stat = ext2_op_stat;
    g_ext2_dir_ops.chmod = ext2_op_chmod;
    axfs_ext2_dir_install(&g_ext2_dir_ops);

    ax_memset(&g_ext2_super_ops, 0, sizeof(g_ext2_super_ops));
    g_ext2_super_ops.alloc_inode = ext2_super_alloc_inode;
    g_ext2_super_ops.destroy_inode = ext2_super_destroy_inode;
    g_ext2_super_ops.write_inode = ext2_super_write_inode;
    g_ext2_super_ops.statfs = ext2_super_statfs;
    g_ext2_super_ops.sync = ext2_super_sync;
    g_ext2_ready = (bool_t)1;
}

axfs_inode_ops_t *axfs_ext2_file_ops(void)
{
    ext2_prepare_ops();
    return &g_ext2_file_ops;
}

axfs_inode_ops_t *axfs_ext2_dir_ops(void)
{
    ext2_prepare_ops();
    return &g_ext2_dir_ops;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

axfs_super_t *axfs_ext2_mount_at(const char *path, u64 partition_lba, u32 flags)
{
    ext2_prepare_ops();

    ext2_volume_t *volume = (ext2_volume_t *)kzalloc(sizeof(*volume));
    if (!volume) {
        return 0;
    }
    i32 status = ext2_volume_open(volume, partition_lba);
    if (status != AXFS_OK) {
        printk("ext2: probe failed at lba %llu (%d)\n", partition_lba, status);
        kfree(volume);
        return 0;
    }

    axfs_super_t *super = (axfs_super_t *)kzalloc(sizeof(*super));
    if (!super) {
        ext2_volume_close(volume);
        kfree(volume);
        return 0;
    }
    super->name = "ext2";
    super->magic = 0x45585432;      /* "EXT2" */
    super->block_size = volume->block_size;
    super->total_blocks = volume->blocks_count;
    super->free_blocks = volume->super.free_blocks_count;
    super->total_inodes = volume->inodes_count;
    super->free_inodes = volume->super.free_inodes_count;
    super->max_name = EXT2_DIRENT_NAME_MAX;
    super->max_file_size = 0xFFFFFFFFull;
    super->ops = &g_ext2_super_ops;
    super->root_ops = &g_ext2_dir_ops;
    super->fs_data = volume;
    super->serial = axfs_next_super_serial();
    super->read_only = volume->read_only;
    volume->axfs = super;

    /* The root inode is number 2 by convention. */
    ext2_inode_t root_inode;
    status = ext2_read_inode(volume, EXT2_ROOT_INO, &root_inode);
    if (status != AXFS_OK || !(root_inode.mode & EXT2_S_IFDIR)) {
        printk("ext2: root inode invalid\n");
        kfree(super);
        ext2_volume_close(volume);
        kfree(volume);
        return 0;
    }
    super->root = axfs_alloc_inode(super, EXT2_ROOT_INO, AXFS_FT_DIR, 0755);
    if (!super->root) {
        kfree(super);
        ext2_volume_close(volume);
        kfree(volume);
        return 0;
    }
    ext2_payload_t *payload = (ext2_payload_t *)kzalloc(sizeof(*payload));
    if (!payload) {
        kfree(super->root);
        kfree(super);
        ext2_volume_close(volume);
        kfree(volume);
        return 0;
    }
    payload->ino = EXT2_ROOT_INO;
    payload->mode = root_inode.mode;
    super->root->fs_data = payload;
    super->root->ops = &g_ext2_dir_ops;
    super->root->flags |= AXFS_I_ROOT;
    super->root->size = root_inode.size;
    super->root->mode = AXFS_S_IFDIR | (root_inode.mode & 07777);

    axfs_mount_t *mount = axfs_mount_add(path, super, flags, 0);
    if (!mount) {
        kfree(payload);
        kfree(super->root);
        kfree(super);
        ext2_volume_close(volume);
        kfree(volume);
        return 0;
    }
    super->root->mount = mount;
    printk("ext2: mounted at %s (%u blocks, %u free inodes)\n", path,
           volume->blocks_count, volume->super.free_inodes_count);
    return super;
}

i32 axfs_ext2_mount(const char *path, u64 partition_lba, u32 flags)
{
    axfs_super_t *super = axfs_ext2_mount_at(path, partition_lba, flags);
    return super ? AXFS_OK : AXFS_EIO;
}

i32 axfs_ext2_probe(u64 partition_lba)
{
    ext2_volume_t volume;
    i32 status = ext2_volume_open(&volume, partition_lba);
    if (status != AXFS_OK) {
        return status;
    }
    printk("ext2: probe ok, label=%s, %u free blocks\n", volume.label,
           volume.super.free_blocks_count);
    ext2_volume_close(&volume);
    return AXFS_OK;
}

/* Build an axfs inode from an ext2 inode number.  Used by the namespace
 * layer and by the VFS when it needs the `..` target. */
axfs_inode_t *axfs_ext2_make_inode(axfs_super_t *super, u32 ino, u16 mode, u32 size,
                                    const char *name)
{
    if (!super) {
        return 0;
    }
    u8 type = (mode & EXT2_S_IFDIR) ? AXFS_FT_DIR
                : (mode & EXT2_S_IFLNK) ? AXFS_FT_SYMLINK
                : (mode & EXT2_S_IFCHR) ? AXFS_FT_CHAR
                : (mode & EXT2_S_IFBLK) ? AXFS_FT_BLOCK
                                        : AXFS_FT_FILE;
    axfs_inode_t *inode = axfs_alloc_inode(super, ino, type, 0644);
    if (!inode) {
        return 0;
    }
    ext2_payload_t *payload = (ext2_payload_t *)kzalloc(sizeof(*payload));
    if (!payload) {
        kfree(inode);
        return 0;
    }
    payload->ino = ino;
    payload->mode = mode;
    inode->fs_data = payload;
    inode->ops = (type == AXFS_FT_DIR) ? &g_ext2_dir_ops : &g_ext2_file_ops;
    inode->size = size;
    u32 permission = (u32)(mode & 07777);
    u32 posix = (type == AXFS_FT_DIR) ? AXFS_S_IFDIR : AXFS_S_IFREG;
    inode->mode = posix | permission;
    inode->nlink = 1;
    (void)name;
    return inode;
}

void axfs_ext2_dump(void)
{
    axfs_mount_t *root = axfs_mount_root();
    if (!root) {
        printk("ext2: no mounts\n");
        return;
    }
    for (i32 i = 0; i < axfs_mount_count(); i++) {
        axfs_mount_t *mount = axfs_mount_at(i);
        if (!mount || !mount->super || mount->super->magic != 0x45585432) {
            continue;
        }
        ext2_volume_t *volume = (ext2_volume_t *)mount->super->fs_data;
        printk("ext2: %s label=%s bs=%u blocks=%u free=%u groups=%u ro=%u\n",
               mount->path, volume->label, volume->block_size,
               volume->blocks_count, volume->super.free_blocks_count,
               volume->groups, volume->read_only);
    }
}
