/*
 * AxiomOS - ext2 on-disk structures.
 *
 * ext2 is the second filesystem the kernel can mount.  It exists here for two
 * reasons: it is a genuinely different allocation model from FAT32 (bitmaps,
 * inode tables, indirect blocks), and it gives the VFS a second real
 * implementation to exercise generic paths against.
 *
 * The definitions follow the published ext2 layout, revision 1 with
 * `s_inode_size = 128`.  Fields that only matter for ext3/ext4 journals are
 * deliberately omitted rather than mislabelled.
 */

#ifndef AXIOM_EXT2_INT_H
#define AXIOM_EXT2_INT_H

#include <axiom/types.h>
#include <axiom/axfs.h>

#define EXT2_MAGIC              0xEF53
#define EXT2_SUPER_OFFSET       1024
#define EXT2_ROOT_INO           2
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_DIRENT_NAME_MAX    255
#define EXT2_EXTENT_MAGIC       0xF30A

#define EXT2_S_IFREG            0100000
#define EXT2_S_IFDIR            0040000
#define EXT2_S_IFLNK            0120000
#define EXT2_S_IFCHR            0020000
#define EXT2_S_IFBLK            0060000
#define EXT2_S_IFIFO            0010000
#define EXT2_S_IFSOCK           0140000

#define EXT2_FT_UNKNOWN         0
#define EXT2_FT_REG_FILE        1
#define EXT2_FT_DIR             2
#define EXT2_FT_CHRDEV          3
#define EXT2_FT_BLKDEV          4
#define EXT2_FT_FIFO            5
#define EXT2_FT_SOCK            6
#define EXT2_FT_SYMLINK         7

#define EXT2_FEATURE_COMPAT_DIR_PREALLOC  0x0001
#define EXT2_FEATURE_COMPAT_IMAGIC_INODES 0x0002
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL   0x0004
#define EXT2_FEATURE_COMPAT_EXT_ATTR      0x0008
#define EXT2_FEATURE_COMPAT_RESIZE_INO    0x0010
#define EXT2_FEATURE_COMPAT_DIR_INDEX     0x0020
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR    0x0004

#define EXT2_FEATURE_INCOMPAT_COMPRESSION   0x0001
#define EXT2_FEATURE_INCOMPAT_FILETYPE      0x0002
#define EXT2_FEATURE_INCOMPAT_RECOVER       0x0004
#define EXT2_FEATURE_INCOMPAT_JOURNAL_DEV   0x0008

#define EXT2_DIR_PAD            4
#define EXT2_DIR_ROUND          3
#define EXT2_NDIR_BLOCKS        12
#define EXT2_IND_BLOCK          EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK         (EXT2_IND_BLOCK + 1)
#define EXT2_TIND_BLOCK         (EXT2_DIND_BLOCK + 1)
#define EXT2_N_BLOCKS           15

typedef struct ext2_super_block {
    u32 inodes_count;
    u32 blocks_count;
    u32 r_blocks_count;
    u32 free_blocks_count;
    u32 free_inodes_count;
    u32 first_data_block;
    u32 log_block_size;
    u32 log_cluster_size;
    u32 blocks_per_group;
    u32 frags_per_group;
    u32 itables_per_group;
    u32 mtime;
    u32 wtime;
    u16 mnt_count;
    u16 max_mnt_count;
    u16 magic;
    u16 state;
    u16 errors;
    u16 minor_rev_level;
    u32 lastcheck;
    u32 checkinterval;
    u32 creator_os;
    u32 rev_level;
    u16 def_resuid;
    u16 def_resgid;
    u32 first_ino;
    u16 inode_size;
    u16 block_group_nr;
    u32 feature_compat;
    u32 feature_incompat;
    u32 feature_ro_compat;
    u8  uuid[16];
    char volume_name[16];
    u8  last_mounted[64];
    u32 algorithm_usage_bitmap;
    u8  prealloc_blocks;
    u8  prealloc_dir_blocks;
    u16 padding1;
    u8  journal_uuid[16];
    u32 journal_inum;
    u32 journal_dev;
    u32 last_orphan;
    u8  reserved[788 - 4 * 3];
} ext2_super_block_t;

typedef struct ext2_group_desc {
    u32 block_bitmap;
    u32 inode_bitmap;
    u32 inode_table;
    u16 free_blocks_count;
    u16 free_inodes_count;
    u16 used_dirs_count;
    u16 padding;
    u32 reserved[3];
} ext2_group_desc_t;

typedef struct ext2_inode {
    u16 mode;
    u16 uid;
    u32 size;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 dtime;
    u16 gid;
    u16 links_count;
    u32 blocks;
    u32 flags;
    u32 osd1;
    u32 block[EXT2_N_BLOCKS];
    u32 generation;
    u32 file_acl;
    u32 dir_acl;
    u32 faddr;
    u8  osd2[12];
} ext2_inode_t;

typedef struct ext2_dir_entry {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    u8  file_type;
    char name[EXT2_DIRENT_NAME_MAX];
} ext2_dir_entry_t;

typedef struct ext2_group {
    u32 block_bitmap_block;
    u32 inode_bitmap_block;
    u32 inode_table_block;
    u32 free_blocks;
    u32 free_inodes;
    u32 used_dirs;
} ext2_group_t;

typedef struct ext2_volume {
    u64 base_lba;
    u32 block_size;
    u32 blocks_count;
    u32 inodes_count;
    u32 first_data_block;
    u32 blocks_per_group;
    u32 inodes_per_group;
    u32 inode_size;
    u32 groups;
    u32 feature_compat;
    u32 feature_incompat;
    u32 feature_ro_compat;
    u32 next_free_inode_hint;
    u32 next_free_block_hint;
    ext2_super_block_t super;
    ext2_group_t *group;
    axfs_super_t *axfs;
    char label[17];
    bool_t has_filetype;
    bool_t mounted;
    bool_t read_only;
    bool_t dirty;
} ext2_volume_t;

typedef struct ext2_handle {
    u32 ino;
    ext2_inode_t inode;
    ext2_volume_t *volume;
    bool_t dirty;
} ext2_handle_t;

/* Superblock level. */
i32  ext2_volume_open(ext2_volume_t *volume, u64 base_lba);
void ext2_volume_close(ext2_volume_t *volume);
i32  ext2_read_block(ext2_volume_t *volume, u32 block, void *buffer);
i32  ext2_write_block(ext2_volume_t *volume, u32 block, const void *buffer);
i32  ext2_read_inode(ext2_volume_t *volume, u32 ino, ext2_inode_t *out);
i32  ext2_write_inode(ext2_volume_t *volume, u32 ino, const ext2_inode_t *inode);
i32  ext2_alloc_inode(ext2_volume_t *volume, u32 *out_ino);
i32  ext2_free_inode(ext2_volume_t *volume, u32 ino);
i32  ext2_alloc_block(ext2_volume_t *volume, u32 *out_block);
i32  ext2_free_block(ext2_volume_t *volume, u32 block);
u32  ext2_total_blocks(ext2_volume_t *volume);
i32  ext2_flush_super(ext2_volume_t *volume);
u32  ext2_group_of_inode(ext2_volume_t *volume, u32 ino);
u32  ext2_group_of_block(ext2_volume_t *volume, u32 block);
u32  ext2_inode_to_offset(ext2_volume_t *volume, u32 ino);

/* Block mapping. */
i32  ext2_bmap(ext2_volume_t *volume, const ext2_inode_t *inode, u32 logical,
               u32 *out_physical, bool_t create);
i32  ext2_truncate_blocks(ext2_volume_t *volume, ext2_inode_t *inode);
u32  ext2_count_blocks(ext2_volume_t *volume, const ext2_inode_t *inode);

/* Directory layer. */
i32  ext2_dir_lookup(ext2_volume_t *volume, ext2_handle_t *dir, const char *name,
                     u32 *out_ino, u8 *out_type);
i32  ext2_dir_list(ext2_volume_t *volume, ext2_handle_t *dir, u32 index,
                   char *name, u32 name_cap, u32 *out_ino, u8 *out_type);
i32  ext2_dir_add(ext2_volume_t *volume, ext2_handle_t *dir, const char *name,
                  u32 ino, u8 type);
i32  ext2_dir_remove(ext2_volume_t *volume, ext2_handle_t *dir, const char *name);
i32  ext2_dir_init(ext2_volume_t *volume, u32 ino, u32 parent_ino);
i32  ext2_dir_merge(ext2_volume_t *volume, ext2_handle_t *dir);
i32  ext2_name_valid(const char *name);
u8   ext2_mode_to_filetype(u16 mode);

#endif /* AXIOM_EXT2_INT_H */
