/*
 * AxiomOS - FAT32 internal interface.
 *
 * Two translation units share this: `fat32.c` owns the volume (BPB parsing,
 * the FAT itself, cluster allocation and sector I/O) and `fat32_dir.c` owns
 * the namespace (directory entries, long names, create/rename/unlink).
 *
 * FAT32 is the filesystem the bootloader understands, so keeping write
 * support here means the kernel can update files the firmware will read on the
 * next boot.  The implementation is deliberately conservative: a fully
 * in-memory FAT mirror, one cluster at a time writes, and no support for
 * files larger than the filesystem's 4 GiB - 1 ceiling.
 */

#ifndef AXIOM_FAT32_INT_H
#define AXIOM_FAT32_INT_H

#include <axiom/types.h>
#include <axiom/axfs.h>

#define FAT_EOC          0x0FFFFFF8u
#define FAT_BAD          0x0FFFFFF7u
#define FAT_FREE         0x00000000u
#define FAT_RESERVED     0x0FFFFFF0u
#define FAT_MAX_CLUSTER  0x0FFFFFF5u

#define FAT_ATTR_READONLY 0x01
#define FAT_ATTR_HIDDEN   0x02
#define FAT_ATTR_SYSTEM   0x04
#define FAT_ATTR_VOLUME   0x08
#define FAT_ATTR_DIR      0x10
#define FAT_ATTR_ARCHIVE  0x20
#define FAT_ATTR_LFN      0x0F

#define FAT_NAME_MAX      11
#define FAT_LFN_MAX       255
#define FAT_ENTRY_SIZE    32
#define FAT_LFN_ENTRIES   20

typedef struct fat_bpb {
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  fat_count;
    u16 root_entries;
    u16 total_sectors16;
    u8  media;
    u16 fat_size16;
    u16 sectors_per_track;
    u16 heads;
    u32 hidden_sectors;
    u32 total_sectors32;
    u32 fat_size32;
    u16 ext_flags;
    u16 fs_version;
    u32 root_cluster;
    u16 fsinfo_sector;
    u16 backup_boot;
} fat_bpb_t;

typedef struct fat_dir_entry {
    char name[FAT_NAME_MAX + 1];     /* 8.3 as stored, plus NUL */
    char short_name[FAT_NAME_MAX + 1];
    char long_name[FAT_LFN_MAX + 1];
    u8   attr;
    u32  first_cluster;
    u64  size;
    u32  entry_index;                /* index within the directory */
    u16  entry_cluster;              /* cluster containing the entry */
    u16  entry_offset;               /* byte offset within that cluster */
} fat_dir_entry_t;

/* Private payload hung off each FAT inode.  The layout is shared by the
 * volume and namespace translation units. */
typedef struct fat_inode_data {
    u32  cluster;                    /* first cluster of the file or directory */
    u32  dir_cluster;                /* parent directory's first cluster */
    u8   attr;
    char name[FAT_LFN_MAX + 1];      /* long name, for entry updates */
} fat_inode_data_t;

typedef struct fat_volume {
    u64          base_lba;
    fat_bpb_t    bpb;
    u32          sectors_per_cluster;
    u32          cluster_bytes;
    u32          cluster_count;
    u32          fat_start_lba;
    u32          data_start_lba;
    u32          root_cluster;
    u32         *fat;                /* in-memory mirror, cluster_count entries */
    u64          free_clusters;
    u32          next_free_hint;
    bool_t       dirty;
    bool_t       mounted;
    axfs_super_t *super;
    char         label[12];
    u32          serial;
} fat_volume_t;

/* Volume level API. */
i32  fat_mount_volume(fat_volume_t *volume, u64 base_lba);
void fat_unmount_volume(fat_volume_t *volume);
i32  fat_read_sectors(fat_volume_t *volume, u32 lba, u32 count, void *buffer);
i32  fat_write_sectors(fat_volume_t *volume, u32 lba, u32 count, const void *buffer);
i32  fat_read_cluster(fat_volume_t *volume, u32 cluster, void *buffer);
i32  fat_write_cluster(fat_volume_t *volume, u32 cluster, const void *buffer);
u32  fat_next_cluster(fat_volume_t *volume, u32 cluster);
i32  fat_set_next(fat_volume_t *volume, u32 cluster, u32 value);
u32  fat_alloc_cluster(fat_volume_t *volume);
i32  fat_free_chain(fat_volume_t *volume, u32 first);
i32  fat_extend_chain(fat_volume_t *volume, u32 last, u32 count, u32 *out);
u32  fat_chain_length(fat_volume_t *volume, u32 first);
u32  fat_cluster_at(fat_volume_t *volume, u32 first, u32 index);
i32  fat_flush(fat_volume_t *volume);
i32  fat_update_fsinfo(fat_volume_t *volume);
u64  fat_volume_free_bytes(fat_volume_t *volume);

/* Directory API, implemented in fat32_dir.c. */
i32 fat_dir_lookup(fat_volume_t *volume, u32 dir_cluster, const char *name,
                   fat_dir_entry_t *out);
i32 fat_dir_list(fat_volume_t *volume, u32 dir_cluster, u32 index,
                 fat_dir_entry_t *out);
i32 fat_dir_add_entry(fat_volume_t *volume, u32 dir_cluster, const char *name,
                      u8 attr, u32 first_cluster, u64 size, u32 *out_cluster,
                      u16 *out_offset);
i32 fat_dir_remove_entry(fat_volume_t *volume, u32 dir_cluster, const char *name);
i32 fat_dir_create(fat_volume_t *volume, u32 dir_cluster, const char *name,
                   u8 attr, u32 *out_cluster);
i32 fat_dir_update_entry(fat_volume_t *volume, u32 dir_cluster, const char *name,
                         u32 first_cluster, u64 size);
i32 fat_dir_rename(fat_volume_t *volume, u32 dir_cluster, const char *old_name,
                   const char *new_name);
i32 fat_dir_is_empty(fat_volume_t *volume, u32 cluster);
i32 fat_make_short_name(const char *long_name, char out[FAT_NAME_MAX + 1],
                        u32 *out_hash, bool_t *out_needs_lfn);
void fat_format_short(const char raw[FAT_NAME_MAX + 1], char *out, usize cap);
u32 fat_short_name_hash(const char raw[FAT_NAME_MAX + 1]);
void fat_timestamp(u64 seconds, u16 *date, u16 *time);
u64  fat_parse_timestamp(u16 date, u16 time);
void axfs_fat32_dir_install(axfs_inode_ops_t *ops);
void fat_encode_lfn_entry(u8 *raw, const char *name, usize chars_before,
                          u32 checksum, u8 sequence, bool_t last);
i32  fat_write_lfn_entries(fat_volume_t *volume, u32 dir_cluster, const char *name,
                           u32 *out_cluster, u16 *out_offset, i32 entries);

#endif /* AXIOM_FAT32_INT_H */
