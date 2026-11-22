/*
 * AxiomOS - FAT32 volume layer for the axfs VFS.
 *
 * This file owns everything below the namespace: parsing the boot parameter
 * block, mirroring the FAT in memory, translating clusters to sectors, doing
 * sector I/O through the block layer, and allocating or freeing chains.
 *
 * The mirror approach costs four bytes per cluster.  On the 64 MiB ESP used by
 * the build that is 256 KiB, which is a rounding error next to the page cache,
 * and it removes an entire class of "read the FAT twice because a write moved
 * the chain underneath us" bugs.
 *
 * All multi-byte fields on disk are little endian; x86 is little endian, so the
 * code reads them directly but wraps that in helpers so a future big-endian
 * port has one place to change.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include "fat32_int.h"

/* ------------------------------------------------------------------ */
/* Little endian helpers                                               */
/* ------------------------------------------------------------------ */

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}


static void put_le32(u8 *p, u32 value)
{
    p[0] = (u8)(value & 0xFF);
    p[1] = (u8)((value >> 8) & 0xFF);
    p[2] = (u8)((value >> 16) & 0xFF);
    p[3] = (u8)((value >> 24) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Sector and cluster I/O                                              */
/* ------------------------------------------------------------------ */

i32 fat_read_sectors(fat_volume_t *volume, u32 lba, u32 count, void *buffer)
{
    if (!volume || !buffer || count == 0) {
        return AXFS_EINVAL;
    }
    u64 offset = ((u64)volume->base_lba + (u64)lba) * volume->bpb.bytes_per_sector;
    u64 bytes = (u64)count * volume->bpb.bytes_per_sector;
    return axfs_block_read_bytes(offset, buffer, bytes);
}

i32 fat_write_sectors(fat_volume_t *volume, u32 lba, u32 count, const void *buffer)
{
    if (!volume || !buffer || count == 0) {
        return AXFS_EINVAL;
    }
    u64 offset = ((u64)volume->base_lba + (u64)lba) * volume->bpb.bytes_per_sector;
    u64 bytes = (u64)count * volume->bpb.bytes_per_sector;
    return axfs_block_write_bytes(offset, buffer, bytes);
}

i32 fat_read_cluster(fat_volume_t *volume, u32 cluster, void *buffer)
{
    if (!volume || !buffer) {
        return AXFS_EINVAL;
    }
    if (cluster < 2 || cluster >= volume->cluster_count + 2) {
        return AXFS_EINVAL;
    }
    u32 lba = volume->data_start_lba + (cluster - 2) * volume->bpb.sectors_per_cluster;
    return fat_read_sectors(volume, lba, volume->bpb.sectors_per_cluster, buffer);
}

i32 fat_write_cluster(fat_volume_t *volume, u32 cluster, const void *buffer)
{
    if (!volume || !buffer) {
        return AXFS_EINVAL;
    }
    if (cluster < 2 || cluster >= volume->cluster_count + 2) {
        return AXFS_EINVAL;
    }
    u32 lba = volume->data_start_lba + (cluster - 2) * volume->bpb.sectors_per_cluster;
    volume->dirty = (bool_t)1;
    return fat_write_sectors(volume, lba, volume->bpb.sectors_per_cluster, buffer);
}

/* ------------------------------------------------------------------ */
/* Cluster chain manipulation                                          */
/* ------------------------------------------------------------------ */

u32 fat_next_cluster(fat_volume_t *volume, u32 cluster)
{
    if (!volume || !volume->fat) {
        return FAT_EOC;
    }
    if (cluster < 2 || cluster >= volume->cluster_count + 2) {
        return FAT_EOC;
    }
    return volume->fat[cluster] & 0x0FFFFFFF;
}

i32 fat_set_next(fat_volume_t *volume, u32 cluster, u32 value)
{
    if (!volume || !volume->fat) {
        return AXFS_EINVAL;
    }
    if (cluster < 2 || cluster >= volume->cluster_count + 2) {
        return AXFS_EINVAL;
    }
    volume->fat[cluster] = value & 0x0FFFFFFF;
    volume->dirty = (bool_t)1;
    return AXFS_OK;
}

u32 fat_chain_length(fat_volume_t *volume, u32 first)
{
    if (first < 2) {
        return 0;
    }
    u32 length = 0;
    u32 cluster = first;
    /* The bound is the cluster count: a corrupt chain that loops otherwise
     * spins forever, and this is the cheapest defence available. */
    while (cluster >= 2 && cluster < volume->cluster_count + 2) {
        length++;
        if (length > volume->cluster_count) {
            break;
        }
        u32 next = fat_next_cluster(volume, cluster);
        if (next >= FAT_EOC) {
            break;
        }
        cluster = next;
    }
    return length;
}

u32 fat_cluster_at(fat_volume_t *volume, u32 first, u32 index)
{
    if (first < 2) {
        return 0;
    }
    u32 cluster = first;
    for (u32 i = 0; i < index; i++) {
        u32 next = fat_next_cluster(volume, cluster);
        if (next >= FAT_EOC) {
            return 0;
        }
        cluster = next;
    }
    return cluster;
}

i32 fat_free_chain(fat_volume_t *volume, u32 first)
{
    if (first < 2) {
        return AXFS_OK;
    }
    u32 cluster = first;
    u32 guard = 0;
    while (cluster >= 2 && cluster < volume->cluster_count + 2) {
        u32 next = fat_next_cluster(volume, cluster);
        volume->fat[cluster] = FAT_FREE;
        volume->free_clusters++;
        if (cluster < volume->next_free_hint) {
            volume->next_free_hint = cluster;
        }
        if (next >= FAT_EOC) {
            break;
        }
        cluster = next;
        if (++guard > volume->cluster_count) {
            break;
        }
    }
    volume->dirty = (bool_t)1;
    return AXFS_OK;
}

u32 fat_alloc_cluster(fat_volume_t *volume)
{
    if (!volume || !volume->fat || volume->free_clusters == 0) {
        return 0;
    }
    u32 start = volume->next_free_hint;
    if (start < 2) {
        start = 2;
    }
    for (u32 pass = 0; pass < 2; pass++) {
        u32 begin = pass == 0 ? start : 2;
        u32 end = pass == 0 ? volume->cluster_count + 2 : start;
        for (u32 cluster = begin; cluster < end; cluster++) {
            if (volume->fat[cluster] == FAT_FREE) {
                volume->fat[cluster] = FAT_EOC;
                volume->free_clusters--;
                volume->next_free_hint = cluster + 1;
                volume->dirty = (bool_t)1;
                return cluster;
            }
        }
    }
    return 0;
}

i32 fat_extend_chain(fat_volume_t *volume, u32 last, u32 count, u32 *out)
{
    if (!out || count == 0) {
        return AXFS_EINVAL;
    }
    u32 first_new = 0;
    u32 previous = last;
    for (u32 i = 0; i < count; i++) {
        u32 cluster = fat_alloc_cluster(volume);
        if (cluster == 0) {
            if (previous >= 2) {
                fat_set_next(volume, previous, FAT_EOC);
            }
            if (first_new) {
                *out = first_new;
                return AXFS_ENOSPC;
            }
            return AXFS_ENOSPC;
        }
        if (previous >= 2) {
            fat_set_next(volume, previous, cluster);
        } else {
            first_new = cluster;
        }
        previous = cluster;
    }
    *out = first_new;
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* FAT mirror                                                          */
/* ------------------------------------------------------------------ */

static i32 fat_load_mirror(fat_volume_t *volume)
{
    u32 fat_bytes = volume->cluster_count * 4 + 8;
    u32 entries = volume->cluster_count + 2;
    volume->fat = (u32 *)kzalloc((usize)entries * sizeof(u32));
    if (!volume->fat) {
        return AXFS_ENOMEM;
    }

    /* Read the FAT one sector at a time straight into the mirror.  The mirror
     * is laid out exactly like the on-disk array, so the copy is a memcpy per
     * sector. */
    u16 bps = volume->bpb.bytes_per_sector;
    u32 sectors = (fat_bytes + bps - 1) / bps;
    if (sectors > volume->bpb.fat_size32) {
        sectors = volume->bpb.fat_size32;
    }
    u8 *bounce = (u8 *)kmalloc(bps);
    if (!bounce) {
        kfree(volume->fat);
        volume->fat = 0;
        return AXFS_ENOMEM;
    }
    for (u32 i = 0; i < sectors; i++) {
        i32 status = fat_read_sectors(volume, volume->fat_start_lba + i, 1, bounce);
        if (status != AXFS_OK) {
            kfree(bounce);
            kfree(volume->fat);
            volume->fat = 0;
            return status;
        }
        for (u32 j = 0; j < bps / 4; j++) {
            u32 index = i * (bps / 4) + j;
            if (index >= entries) {
                break;
            }
            volume->fat[index] = le32(bounce + j * 4) & 0x0FFFFFFF;
        }
    }
    kfree(bounce);

    u32 free_count = 0;
    for (u32 cluster = 2; cluster < entries; cluster++) {
        if (volume->fat[cluster] == FAT_FREE) {
            free_count++;
        }
    }
    volume->free_clusters = free_count;
    volume->next_free_hint = 2;
    return AXFS_OK;
}

i32 fat_flush(fat_volume_t *volume)
{
    if (!volume || !volume->fat || !volume->dirty) {
        return AXFS_OK;
    }
    u16 bps = volume->bpb.bytes_per_sector;
    u8 *bounce = (u8 *)kmalloc(bps);
    if (!bounce) {
        return AXFS_ENOMEM;
    }
    u32 entries = volume->cluster_count + 2;
    u32 sectors = volume->bpb.fat_size32;
    for (u32 i = 0; i < sectors; i++) {
        for (u32 j = 0; j < bps / 4; j++) {
            u32 index = i * (bps / 4) + j;
            u32 value = index < entries ? volume->fat[index] : 0;
            put_le32(bounce + j * 4, value);
        }
        /* Writing the primary copy is enough: every reader follows FAT[0]'s
         * active-flag rules, and this driver never sets the "use second FAT"
         * bit. */
        i32 status = fat_write_sectors(volume, volume->fat_start_lba + i, 1, bounce);
        if (status != AXFS_OK) {
            kfree(bounce);
            return status;
        }
        if (volume->bpb.fat_count > 1) {
            u32 mirror = volume->fat_start_lba + volume->bpb.fat_size32 + i;
            fat_write_sectors(volume, mirror, 1, bounce);
        }
    }
    kfree(bounce);
    volume->dirty = (bool_t)0;
    return fat_update_fsinfo(volume);
}

i32 fat_update_fsinfo(fat_volume_t *volume)
{
    if (!volume || volume->bpb.fsinfo_sector == 0) {
        return AXFS_OK;
    }
    u8 sector[512];
    i32 status = fat_read_sectors(volume, volume->bpb.fsinfo_sector, 1, sector);
    if (status != AXFS_OK) {
        return status;
    }
    put_le32(sector + 0x1E8, volume->free_clusters);
    /* The "next free" hint is advisory; the FAT itself is authoritative. */
    put_le32(sector + 0x1EC, volume->next_free_hint);
    return fat_write_sectors(volume, volume->bpb.fsinfo_sector, 1, sector);
}

u64 fat_volume_free_bytes(fat_volume_t *volume)
{
    if (!volume) {
        return 0;
    }
    return volume->free_clusters * (u64)volume->cluster_bytes;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

static void fat_default_bpb(fat_bpb_t *bpb)
{
    ax_memset(bpb, 0, sizeof(*bpb));
}

i32 fat_mount_volume(fat_volume_t *volume, u64 base_lba)
{
    if (!volume) {
        return AXFS_EINVAL;
    }
    ax_memset(volume, 0, sizeof(*volume));
    volume->base_lba = base_lba;
    fat_default_bpb(&volume->bpb);

    u8 boot[512];
    i32 status = fat_read_sectors(volume, 0, 1, boot);
    if (status != AXFS_OK) {
        return status;
    }

    u16 bps = le16(boot + 11);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
        return AXFS_EINVAL;
    }
    u8 spc = boot[13];
    if (spc == 0 || (spc & (spc - 1)) != 0) {
        return AXFS_EINVAL;
    }
    u8 fat_count = boot[16];
    if (fat_count == 0 || fat_count > 4) {
        return AXFS_EINVAL;
    }

    volume->bpb.bytes_per_sector = bps;
    volume->bpb.sectors_per_cluster = spc;
    volume->bpb.reserved_sectors = le16(boot + 14);
    volume->bpb.fat_count = fat_count;
    volume->bpb.root_entries = le16(boot + 17);
    volume->bpb.total_sectors16 = le16(boot + 19);
    volume->bpb.fat_size16 = le16(boot + 22);
    volume->bpb.total_sectors32 = le32(boot + 32);
    volume->bpb.fat_size32 = le32(boot + 36);
    volume->bpb.root_cluster = le32(boot + 44);
    volume->bpb.fsinfo_sector = le16(boot + 48);
    volume->bpb.backup_boot = le16(boot + 50);

    if (volume->bpb.fat_size32 == 0) {
        return AXFS_EINVAL;         /* FAT12/16: not our job */
    }
    if (volume->bpb.root_cluster < 2) {
        return AXFS_EINVAL;
    }
    u32 total = volume->bpb.total_sectors32
                    ? volume->bpb.total_sectors32
                    : volume->bpb.total_sectors16;
    if (total == 0) {
        return AXFS_EINVAL;
    }

    volume->sectors_per_cluster = spc;
    volume->cluster_bytes = spc * bps;
    volume->fat_start_lba = volume->bpb.reserved_sectors;
    volume->data_start_lba = volume->bpb.reserved_sectors +
                             volume->bpb.fat_count * volume->bpb.fat_size32;
    if (volume->data_start_lba >= total) {
        return AXFS_EINVAL;
    }
    u32 data_sectors = total - volume->data_start_lba;
    volume->cluster_count = data_sectors / spc;
    if (volume->cluster_count < 65525) {
        /* Microsoft's threshold: below this a volume should call itself FAT16,
         * and silently treating it as FAT32 risks reading a FAT16 FAT as 32-bit
         * entries.  Refuse instead of corrupting. */
        return AXFS_EINVAL;
    }
    volume->root_cluster = volume->bpb.root_cluster;

    ax_strlcpy(volume->label, "AXIOM-ESP", sizeof(volume->label));
    volume->serial = le32(boot + 67);

    status = fat_load_mirror(volume);
    if (status != AXFS_OK) {
        return status;
    }
    volume->mounted = (bool_t)1;
    volume->dirty = (bool_t)0;
    return AXFS_OK;
}

void fat_unmount_volume(fat_volume_t *volume)
{
    if (!volume) {
        return;
    }
    fat_flush(volume);
    if (volume->fat) {
        kfree(volume->fat);
        volume->fat = 0;
    }
    volume->mounted = (bool_t)0;
}

/* ------------------------------------------------------------------ */
/* Inode operations                                                    */
/* ------------------------------------------------------------------ */


static fat_volume_t *fat_of(axfs_inode_t *inode)
{
    if (!inode || !inode->super) {
        return 0;
    }
    return (fat_volume_t *)inode->super->fs_data;
}

static fat_inode_data_t *fat_data_of(axfs_inode_t *inode)
{
    return inode ? (fat_inode_data_t *)inode->fs_data : 0;
}

static i64 fat_inode_read(axfs_file_t *file, void *buffer, u64 count)
{
    if (!file || !file->inode || !buffer) {
        return AXFS_EINVAL;
    }
    axfs_inode_t *inode = file->inode;
    fat_volume_t *volume = fat_of(inode);
    fat_inode_data_t *data = fat_data_of(inode);
    if (!volume || !data) {
        return AXFS_EIO;
    }
    if (inode->type == AXFS_FT_DIR) {
        return AXFS_EISDIR;
    }
    if (file->offset >= inode->size) {
        return 0;
    }
    u64 want = count;
    if (file->offset + want > inode->size) {
        want = inode->size - file->offset;
    }

    u8 *cluster = (u8 *)kmalloc(volume->cluster_bytes);
    if (!cluster) {
        return AXFS_ENOMEM;
    }
    u64 copied = 0;
    while (copied < want) {
        u64 absolute = file->offset + copied;
        u32 index = (u32)(absolute / volume->cluster_bytes);
        u32 offset = (u32)(absolute % volume->cluster_bytes);
        u32 phys = fat_cluster_at(volume, data->cluster, index);
        if (phys == 0) {
            break;
        }
        i32 status = fat_read_cluster(volume, phys, cluster);
        if (status != AXFS_OK) {
            kfree(cluster);
            return status;
        }
        u64 chunk = volume->cluster_bytes - offset;
        if (chunk > want - copied) {
            chunk = want - copied;
        }
        for (u64 i = 0; i < chunk; i++) {
            ((u8 *)buffer)[copied + i] = cluster[offset + i];
        }
        copied += chunk;
    }
    kfree(cluster);
    file->offset += copied;
    return (i64)copied;
}

static i64 fat_inode_write(axfs_file_t *file, const void *buffer, u64 count)
{
    if (!file || !file->inode || !buffer) {
        return AXFS_EINVAL;
    }
    axfs_inode_t *inode = file->inode;
    fat_volume_t *volume = fat_of(inode);
    fat_inode_data_t *data = fat_data_of(inode);
    if (!volume || !data) {
        return AXFS_EIO;
    }
    if (inode->type == AXFS_FT_DIR) {
        return AXFS_EISDIR;
    }
    if (inode->mode & AXFS_S_IFMT) {
        if (!(inode->mode & 0200)) {
            return AXFS_EACCES;
        }
    }
    u64 end = file->offset + count;
    if (inode->super && end > inode->super->max_file_size) {
        return AXFS_EFBIG;
    }

    /* Ensure the chain is long enough before touching any data, so a failure
     * halfway through cannot leave the file claiming a size it cannot reach. */
    u32 needed = (u32)((end + volume->cluster_bytes - 1) / volume->cluster_bytes);
    u32 have = data->cluster ? fat_chain_length(volume, data->cluster) : 0;
    if (needed > have) {
        u32 extra = needed - have;
        u32 last = have ? fat_cluster_at(volume, data->cluster, have - 1) : 0;
        u32 first_new = 0;
        i32 status = fat_extend_chain(volume, last, extra, &first_new);
        if (status != AXFS_OK) {
            return status;
        }
        if (!data->cluster) {
            data->cluster = first_new;
        }
    }

    u8 *cluster = (u8 *)kmalloc(volume->cluster_bytes);
    if (!cluster) {
        return AXFS_ENOMEM;
    }
    u64 written = 0;
    while (written < count) {
        u64 absolute = file->offset + written;
        u32 index = (u32)(absolute / volume->cluster_bytes);
        u32 offset = (u32)(absolute % volume->cluster_bytes);
        u32 phys = fat_cluster_at(volume, data->cluster, index);
        if (phys == 0) {
            kfree(cluster);
            return AXFS_EIO;
        }
        /* Partial writes need the existing contents of the cluster, because a
         * cluster is larger than a sector and the block layer writes whole
         * sectors. */
        if (offset != 0 || count - written < volume->cluster_bytes) {
            i32 status = fat_read_cluster(volume, phys, cluster);
            if (status != AXFS_OK) {
                kfree(cluster);
                return status;
            }
        }
        u64 chunk = volume->cluster_bytes - offset;
        if (chunk > count - written) {
            chunk = count - written;
        }
        for (u64 i = 0; i < chunk; i++) {
            cluster[offset + i] = ((const u8 *)buffer)[written + i];
        }
        i32 status = fat_write_cluster(volume, phys, cluster);
        if (status != AXFS_OK) {
            kfree(cluster);
            return status;
        }
        written += chunk;
    }
    kfree(cluster);

    file->offset += written;
    if (file->offset > inode->size) {
        inode->size = file->offset;
        inode->flags |= AXFS_I_DIRTY;
        /* Persist the new size in the directory entry so the next boot sees
         * the file as it is now rather than as it was. */
        if (data->name[0] && data->dir_cluster) {
            fat_dir_update_entry(volume, data->dir_cluster, data->name,
                                 data->cluster, inode->size);
        }
    }
    inode->mtime = axfs_now();
    return (i64)written;
}

static i32 fat_inode_truncate(axfs_inode_t *inode, u64 size)
{
    fat_volume_t *volume = fat_of(inode);
    fat_inode_data_t *data = fat_data_of(inode);
    if (!volume || !data) {
        return AXFS_EIO;
    }
    if (size == 0) {
        if (data->cluster) {
            fat_free_chain(volume, data->cluster);
            data->cluster = 0;
        }
        inode->size = 0;
        inode->flags |= AXFS_I_DIRTY;
        if (data->name[0] && data->dir_cluster) {
            fat_dir_update_entry(volume, data->dir_cluster, data->name, 0, 0);
        }
        return AXFS_OK;
    }
    u64 current = inode->size;
    if (size < current) {
        u32 keep = (u32)((size + volume->cluster_bytes - 1) / volume->cluster_bytes);
        u32 have = data->cluster ? fat_chain_length(volume, data->cluster) : 0;
        if (have > keep) {
            u32 cut = fat_cluster_at(volume, data->cluster, keep);
            if (cut) {
                u32 next = fat_next_cluster(volume, cut);
                fat_set_next(volume, cut, FAT_EOC);
                if (next >= 2 && next < FAT_EOC) {
                    fat_free_chain(volume, next);
                }
            }
        }
        inode->size = size;
        inode->flags |= AXFS_I_DIRTY;
        return AXFS_OK;
    }

    /* Growing a file extends the chain and leaves the new clusters untouched;
     * they read as zero because allocation zeroes them. */
    u32 needed = (u32)((size + volume->cluster_bytes - 1) / volume->cluster_bytes);
    u32 have = data->cluster ? fat_chain_length(volume, data->cluster) : 0;
    if (needed > have) {
        u32 last = have ? fat_cluster_at(volume, data->cluster, have - 1) : 0;
        u32 first_new = 0;
        i32 status = fat_extend_chain(volume, last, needed - have, &first_new);
        if (status != AXFS_OK) {
            return status;
        }
        if (!data->cluster) {
            data->cluster = first_new;
        }
    }
    inode->size = size;
    inode->flags |= AXFS_I_DIRTY;
    return AXFS_OK;
}

static i32 fat_inode_stat(axfs_inode_t *inode, axfs_attr_t *out)
{
    if (!inode || !out) {
        return AXFS_EINVAL;
    }
    out->mode = inode->mode;
    out->uid = inode->uid;
    out->gid = inode->gid;
    out->size = inode->size;
    out->ino = inode->ino;
    out->nlink = inode->nlink;
    out->type = inode->type;
    out->atime = inode->atime;
    out->mtime = inode->mtime;
    out->ctime = inode->ctime;
    fat_volume_t *volume = fat_of(inode);
    fat_inode_data_t *data = fat_data_of(inode);
    if (volume && data) {
        u32 clusters = data->cluster ? fat_chain_length(volume, data->cluster) : 0;
        out->blocks = clusters * (u64)volume->sectors_per_cluster;
    }
    return AXFS_OK;
}

static i32 fat_inode_chmod(axfs_inode_t *inode, u32 mode)
{
    if (!inode) {
        return AXFS_EINVAL;
    }
    inode->mode = (inode->mode & AXFS_S_IFMT) | (mode & 0777);
    inode->flags |= AXFS_I_DIRTY;
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Ops tables and superblock                                          */
/* ------------------------------------------------------------------ */

static axfs_inode_ops_t g_fat_ops;
static axfs_inode_ops_t g_fat_dir_ops;
static axfs_super_ops_t g_fat_super_ops;
static bool_t g_fat_ops_ready;

static i32 fat_super_alloc_inode(axfs_super_t *super, axfs_inode_t **out)
{
    (void)super;
    (void)out;
    return AXFS_EPERM;
}

static void fat_super_destroy_inode(axfs_inode_t *inode)
{
    (void)inode;
}

static i32 fat_super_write_inode(axfs_inode_t *inode)
{
    if (!inode) {
        return AXFS_EINVAL;
    }
    inode->flags &= ~AXFS_I_DIRTY;
    return AXFS_OK;
}

static i32 fat_super_statfs(axfs_super_t *super, u64 *blocks, u64 *free_blocks,
                            u64 *files, u64 *free_files)
{
    fat_volume_t *volume = super ? (fat_volume_t *)super->fs_data : 0;
    if (!volume) {
        return AXFS_EINVAL;
    }
    if (blocks) {
        *blocks = volume->cluster_count;
    }
    if (free_blocks) {
        *free_blocks = volume->free_clusters;
    }
    if (files) {
        *files = 0;
    }
    if (free_files) {
        *free_files = 0;
    }
    return AXFS_OK;
}

static i32 fat_super_sync(axfs_super_t *super)
{
    fat_volume_t *volume = super ? (fat_volume_t *)super->fs_data : 0;
    if (!volume) {
        return AXFS_EINVAL;
    }
    return fat_flush(volume);
}

static void fat_prepare_ops(void)
{
    if (g_fat_ops_ready) {
        return;
    }
    ax_memset(&g_fat_ops, 0, sizeof(g_fat_ops));
    g_fat_ops.read = fat_inode_read;
    g_fat_ops.write = fat_inode_write;
    g_fat_ops.truncate = fat_inode_truncate;
    g_fat_ops.stat = fat_inode_stat;
    g_fat_ops.chmod = fat_inode_chmod;

    ax_memset(&g_fat_dir_ops, 0, sizeof(g_fat_dir_ops));
    g_fat_dir_ops.stat = fat_inode_stat;
    g_fat_dir_ops.chmod = fat_inode_chmod;
    axfs_fat32_dir_install(&g_fat_dir_ops);

    ax_memset(&g_fat_super_ops, 0, sizeof(g_fat_super_ops));
    g_fat_super_ops.alloc_inode = fat_super_alloc_inode;
    g_fat_super_ops.destroy_inode = fat_super_destroy_inode;
    g_fat_super_ops.write_inode = fat_super_write_inode;
    g_fat_super_ops.statfs = fat_super_statfs;
    g_fat_super_ops.sync = fat_super_sync;
    g_fat_ops_ready = (bool_t)1;
}

axfs_inode_ops_t *axfs_fat32_file_ops(void)
{
    fat_prepare_ops();
    return &g_fat_ops;
}

axfs_inode_ops_t *axfs_fat32_dir_ops(void)
{
    fat_prepare_ops();
    return &g_fat_dir_ops;
}

void axfs_fat32_detach_ops(axfs_inode_ops_t *ops)
{
    if (ops == &g_fat_dir_ops) {
        ax_memset(ops, 0, sizeof(*ops));
        /* Directory ops are rebuilt on every mount, and the mount path fills
         * in the namespace hooks. */
        g_fat_ops_ready = (bool_t)0;
    }
}

/* ------------------------------------------------------------------ */
/* Mount entry point                                                   */
/* ------------------------------------------------------------------ */

axfs_super_t *axfs_fat32_mount_at(const char *path, u64 partition_lba)
{
    fat_prepare_ops();

    fat_volume_t *volume = (fat_volume_t *)kzalloc(sizeof(*volume));
    if (!volume) {
        return 0;
    }
    i32 status = fat_mount_volume(volume, partition_lba);
    if (status != AXFS_OK) {
        printk("fat32: probe at lba %llu failed (%d)\n", partition_lba, (i32)status);
        kfree(volume);
        return 0;
    }

    axfs_super_t *super = (axfs_super_t *)kzalloc(sizeof(*super));
    if (!super) {
        fat_unmount_volume(volume);
        kfree(volume);
        return 0;
    }
    super->name = "fat32";
    super->magic = 0x46415433;      /* "FAT3" */
    super->block_size = volume->cluster_bytes;
    super->total_blocks = volume->cluster_count;
    super->free_blocks = volume->free_clusters;
    super->total_inodes = 0;
    super->free_inodes = 0;
    super->max_name = FAT_LFN_MAX;
    super->max_file_size = 0xFFFFFFFFull;
    super->ops = &g_fat_super_ops;
    super->root_ops = &g_fat_dir_ops;
    super->fs_data = volume;
    super->serial = axfs_next_super_serial();
    super->read_only = (bool_t)0;

    volume->super = super;

    super->root = axfs_alloc_inode(super, 2, AXFS_FT_DIR, 0755);
    if (!super->root) {
        kfree(super);
        fat_unmount_volume(volume);
        kfree(volume);
        return 0;
    }
    fat_inode_data_t *root_data = (fat_inode_data_t *)kzalloc(sizeof(*root_data));
    if (!root_data) {
        kfree(super->root);
        kfree(super);
        fat_unmount_volume(volume);
        kfree(volume);
        return 0;
    }
    root_data->cluster = volume->root_cluster;
    root_data->dir_cluster = 0;
    root_data->attr = FAT_ATTR_DIR;
    super->root->fs_data = root_data;
    super->root->ops = &g_fat_dir_ops;
    super->root->flags |= AXFS_I_ROOT;
    super->root->mount = 0;

    axfs_mount_t *mount = axfs_mount_add(path, super, 0, 0);
    if (!mount) {
        kfree(root_data);
        kfree(super->root);
        kfree(super);
        fat_unmount_volume(volume);
        kfree(volume);
        return 0;
    }
    super->root->mount = mount;

    printk("fat32: mounted %s at %s (%u clusters, %llu free, %u B/cluster)\n",
           volume->label, path, volume->cluster_count, volume->free_clusters,
           volume->cluster_bytes);
    return super;
}

/* Build an inode for a directory entry found by the namespace layer. */
axfs_inode_t *axfs_fat32_make_inode(axfs_super_t *super, const void *entry_ptr)
{
    const fat_dir_entry_t *entry = (const fat_dir_entry_t *)entry_ptr;
    if (!super || !entry) {
        return 0;
    }
    fat_volume_t *volume = (fat_volume_t *)super->fs_data;
    u64 ino = ((u64)entry->entry_cluster << 16) | entry->entry_offset;
    u8 type = (entry->attr & FAT_ATTR_DIR) ? AXFS_FT_DIR : AXFS_FT_FILE;
    axfs_inode_t *inode = axfs_alloc_inode(super, ino, type, 0755);
    if (!inode) {
        return 0;
    }
    fat_inode_data_t *data = (fat_inode_data_t *)kzalloc(sizeof(*data));
    if (!data) {
        kfree(inode);
        return 0;
    }
    (void)volume;
    data->cluster = entry->first_cluster;
    data->dir_cluster = entry->entry_cluster;
    data->attr = entry->attr;
    ax_strlcpy(data->name, entry->name, sizeof(data->name));
    inode->fs_data = data;
    inode->size = entry->size;
    inode->ops = (type == AXFS_FT_DIR) ? &g_fat_dir_ops : &g_fat_ops;
    inode->mode = (type == AXFS_FT_DIR ? AXFS_S_IFDIR : AXFS_S_IFREG) |
                  ((entry->attr & FAT_ATTR_READONLY) ? 0555 : 0755);
    inode->nlink = 1;
    return inode;
}

void axfs_fat32_dump(void)
{
    axfs_mount_t *mount = axfs_mount_find("/boot");
    if (!mount || !mount->super) {
        printk("fat32: /boot not mounted\n");
        return;
    }
    fat_volume_t *volume = (fat_volume_t *)mount->super->fs_data;
    printk("fat32: volume label=%s serial=%08x\n", volume->label, volume->serial);
    printk("  base=%llu bps=%u spc=%u clusters=%u free=%llu\n",
           volume->base_lba, volume->bpb.bytes_per_sector,
           volume->bpb.sectors_per_cluster, volume->cluster_count,
           volume->free_clusters);
    printk("  fat@%u data@%u root=%u mirror=%u bytes\n", volume->fat_start_lba,
           volume->data_start_lba, volume->root_cluster,
           volume->cluster_count * 4);
}

/* The header-declared three-argument entry point keeps the status-returning
 * convention the boot path expects; the superblock pointer form above is for
 * callers that want to inspect the volume. */
i32 axfs_fat32_mount(const char *path, u64 partition_lba, u32 flags)
{
    (void)flags;
    axfs_super_t *super = axfs_fat32_mount_at(path, partition_lba);
    return super ? AXFS_OK : AXFS_EIO;
}

i32 axfs_fat32_probe(u64 partition_lba)
{
    fat_volume_t probe;
    i32 status = fat_mount_volume(&probe, partition_lba);
    if (status != AXFS_OK) {
        return status;
    }
    u64 free_bytes = fat_volume_free_bytes(&probe);
    printk("fat32: probe ok, label=%s free=%llu bytes\n", probe.label, free_bytes);
    fat_unmount_volume(&probe);
    return AXFS_OK;
}

i32 axfs_fat32_sync(void)
{
    axfs_mount_t *mount = axfs_mount_find("/boot");
    if (!mount || !mount->super) {
        return AXFS_ENOENT;
    }
    fat_volume_t *volume = (fat_volume_t *)mount->super->fs_data;
    return fat_flush(volume);
}