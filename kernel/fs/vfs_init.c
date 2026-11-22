/*
 * AxiomOS - VFS bring-up and shutdown.
 *
 * Boot order matters: the block layer registers a device, the filesystem
 * drivers probe it, and the mount table ends up with `/`, `/dev`, `/tmp` and
 * `/mnt` populated.  The kernel calls axfs_init() once from kmain; everything
 * below is idempotent so a second call after a soft reboot is harmless.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axfs.h>

static bool_t g_vfs_up;
static u64    g_boot_time;

/* The block device registered by AHCI or ATA. */
static axfs_blockdev_t *g_blockdev;

/* ------------------------------------------------------------------ */
/* Block layer shim                                                    */
/* ------------------------------------------------------------------ */

void axfs_blockdev_register(axfs_blockdev_t *device)
{
    g_blockdev = device;
    if (device) {
        printk("vfs: block device %llu sectors of %llu bytes\n", device->sector_count,
               device->sector_size);
    }
}

axfs_blockdev_t *axfs_blockdev(void)
{
    return g_blockdev;
}

/* Read a byte range that may not be sector aligned, by reading the covering
 * sectors into a bounce buffer.  Filesystems call this for metadata. */
i32 axfs_block_read_bytes(u64 offset, void *buffer, u64 count)
{
    if (!g_blockdev || !buffer) {
        return AXFS_ENXIO;
    }
    u64 sector = g_blockdev->sector_size ? g_blockdev->sector_size : 512;
    u64 first = offset / sector;
    u64 last = (offset + count - 1) / sector;
    u64 span = (last - first + 1) * sector;
    if (span > 4096 * 16) {
        return AXFS_EINVAL;
    }
    u8 bounce[4096 * 16];
    if (!g_blockdev->read(first, (u16)((span + 511) / 512), bounce)) {
        return AXFS_EIO;
    }
    u64 inner = offset - first * sector;
    for (u64 i = 0; i < count; i++) {
        ((u8 *)buffer)[i] = bounce[inner + i];
    }
    return AXFS_OK;
}

i32 axfs_block_write_bytes(u64 offset, const void *buffer, u64 count)
{
    if (!g_blockdev || !buffer || !g_blockdev->write) {
        return AXFS_EIO;
    }
    u64 sector = g_blockdev->sector_size ? g_blockdev->sector_size : 512;
    u64 first = offset / sector;
    u64 last = (offset + count - 1) / sector;
    u64 span = (last - first + 1) * sector;
    if (span > 4096 * 16) {
        return AXFS_EINVAL;
    }
    u8 bounce[4096 * 16];
    if (!g_blockdev->read(first, (u16)((span + 511) / 512), bounce)) {
        return AXFS_EIO;
    }
    u64 inner = offset - first * sector;
    for (u64 i = 0; i < count; i++) {
        bounce[inner + i] = ((const u8 *)buffer)[i];
    }
    if (!g_blockdev->write(first, (u16)((span + 511) / 512), bounce)) {
        return AXFS_EIO;
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Dirty tracking                                                      */
/* ------------------------------------------------------------------ */

/* The name is kept for the generic write path: it means "this inode changed,
 * schedule a flush".  Pseudo filesystems write synchronously and ignore it. */
void axfs_dirty(axfs_inode_t *inode)
{
    if (!inode) {
        return;
    }
    inode->flags |= AXFS_I_DIRTY;
    inode->version++;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void axfs_init(void)
{
    if (g_vfs_up) {
        return;
    }
    axfs_path_selftest();
    axfs_tmpfs_init();
    axfs_devfs_init();

    /* The root is a tmpfs so the shell always has somewhere to write even
     * when no disk filesystem is available. */
    axfs_super_t *root_super = axfs_tmpfs_create("rootfs", 0);
    if (root_super) {
        root_super->root->flags |= AXFS_I_ROOT;
        axfs_mount_t *root = axfs_mount_add("/", root_super, 0, 0);
        if (root) {
            root->id = 0;
            root->path[0] = '/';
            root->path[1] = 0;
        }
    }

    /* /dev is a device filesystem; /tmp a second tmpfs; /mnt a directory for
     * later mounts. */
    axfs_super_t *dev_super = axfs_devfs_create();
    if (dev_super) {
        axfs_inode_t *covered = 0;
        axfs_lookup(axfs_mount_root(), "/dev", &covered, 0, 0);
        axfs_mount_add("/dev", dev_super, AXFS_MNT_NODEV, covered);
        if (covered) {
            axfs_unref_inode(covered);
        }
    }

    axfs_super_t *tmp_super = axfs_tmpfs_create("tmpfs", 64ull * 1024 * 1024);
    if (tmp_super) {
        axfs_mkdir("/tmp", 0755);
        axfs_inode_t *covered = 0;
        axfs_lookup(axfs_mount_root(), "/tmp", &covered, 0, 0);
        axfs_mount_add("/tmp", tmp_super, 0, covered);
        if (covered) {
            axfs_unref_inode(covered);
        }
    }
    axfs_mkdir("/mnt", 0755);
    axfs_mkdir("/etc", 0755);
    axfs_mkdir("/home", 0755);
    axfs_mkdir("/bin", 0755);

    g_boot_time = axfs_now();
    g_vfs_up = (bool_t)1;
    printk("vfs: up, rootfs=tmpfs mounts=%d\n", axfs_mount_count());
}

void axfs_shutdown(void)
{
    if (!g_vfs_up) {
        return;
    }
    axfs_page_flush_all();
    axfs_mount_sync_all();
    /* Unmount everything below the root, then leave the root in place so a
     * warm reboot can keep using it. */
    for (i32 i = axfs_mount_count() - 1; i >= 0; i--) {
        axfs_mount_t *mount = axfs_mount_at(i);
        if (mount && mount->path[0] == '/' && ax_strcmp(mount->path, "/") != 0) {
            /* Direct detach; children were already removed. */
            mount->parent = 0;
        }
    }
    axfs_cache_release((bool_t)0);
    g_vfs_up = (bool_t)0;
    printk("vfs: down\n");
}

bool_t axfs_ready(void)
{
    return g_vfs_up;
}

/* Uptime in seconds since the VFS came up; used in stat times and in the
 * `info` command. */
u64 axfs_uptime(void)
{
    return axfs_now() - g_boot_time;
}

/* ------------------------------------------------------------------ */
/* Kernel-side helpers used by init and the shell                       */
/* ------------------------------------------------------------------ */

i32 axfs_mkdir(const char *path, u32 mode)
{
    char parent_path[AXFS_PATH_MAX];
    char name[AXFS_NAME_MAX + 1];
    if (axfs_path_dirname(path, parent_path, sizeof(parent_path)) != AXFS_OK) {
        return AXFS_EINVAL;
    }
    if (axfs_path_basename(path, name, sizeof(name)) != AXFS_OK) {
        return AXFS_EINVAL;
    }
    axfs_inode_t *parent = 0;
    i32 rc = axfs_lookup(axfs_mount_root(), parent_path, &parent, 0, 0);
    if (rc != AXFS_OK || !parent) {
        return rc;
    }
    if (!parent->ops || !parent->ops->mkdir) {
        axfs_unref_inode(parent);
        return AXFS_ENOSYS;
    }
    axfs_inode_t *child = 0;
    rc = parent->ops->mkdir(parent, name, mode, &child);
    if (child) {
        axfs_unref_inode(child);
    }
    axfs_unref_inode(parent);
    return rc;
}

i32 axfs_unlink(const char *path)
{
    char parent_path[AXFS_PATH_MAX];
    char name[AXFS_NAME_MAX + 1];
    if (axfs_path_dirname(path, parent_path, sizeof(parent_path)) != AXFS_OK ||
        axfs_path_basename(path, name, sizeof(name)) != AXFS_OK) {
        return AXFS_EINVAL;
    }
    axfs_inode_t *parent = 0;
    i32 rc = axfs_lookup(axfs_mount_root(), parent_path, &parent, 0, 0);
    if (rc != AXFS_OK || !parent) {
        return rc;
    }
    if (!parent->ops || !parent->ops->unlink) {
        axfs_unref_inode(parent);
        return AXFS_ENOSYS;
    }
    rc = parent->ops->unlink(parent, name);
    axfs_unref_inode(parent);
    return rc;
}

i32 axfs_rmdir(const char *path)
{
    char parent_path[AXFS_PATH_MAX];
    char name[AXFS_NAME_MAX + 1];
    if (axfs_path_dirname(path, parent_path, sizeof(parent_path)) != AXFS_OK ||
        axfs_path_basename(path, name, sizeof(name)) != AXFS_OK) {
        return AXFS_EINVAL;
    }
    axfs_inode_t *parent = 0;
    i32 rc = axfs_lookup(axfs_mount_root(), parent_path, &parent, 0, 0);
    if (rc != AXFS_OK || !parent) {
        return rc;
    }
    if (!parent->ops || !parent->ops->rmdir) {
        axfs_unref_inode(parent);
        return AXFS_ENOSYS;
    }
    rc = parent->ops->rmdir(parent, name);
    axfs_unref_inode(parent);
    return rc;
}

i32 axfs_rename(const char *from, const char *to)
{
    char old_dir[AXFS_PATH_MAX];
    char old_name[AXFS_NAME_MAX + 1];
    char new_dir[AXFS_PATH_MAX];
    char new_name[AXFS_NAME_MAX + 1];
    if (axfs_path_dirname(from, old_dir, sizeof(old_dir)) != AXFS_OK ||
        axfs_path_basename(from, old_name, sizeof(old_name)) != AXFS_OK ||
        axfs_path_dirname(to, new_dir, sizeof(new_dir)) != AXFS_OK ||
        axfs_path_basename(to, new_name, sizeof(new_name)) != AXFS_OK) {
        return AXFS_EINVAL;
    }
    axfs_inode_t *src_dir = 0;
    axfs_inode_t *dst_dir = 0;
    i32 rc = axfs_lookup(axfs_mount_root(), old_dir, &src_dir, 0, 0);
    if (rc != AXFS_OK || !src_dir) {
        return rc;
    }
    rc = axfs_lookup(axfs_mount_root(), new_dir, &dst_dir, 0, 0);
    if (rc != AXFS_OK || !dst_dir) {
        axfs_unref_inode(src_dir);
        return rc;
    }
    if (src_dir->super != dst_dir->super) {
        axfs_unref_inode(src_dir);
        axfs_unref_inode(dst_dir);
        return AXFS_EXDEV;
    }
    if (!src_dir->ops || !src_dir->ops->rename) {
        axfs_unref_inode(src_dir);
        axfs_unref_inode(dst_dir);
        return AXFS_ENOSYS;
    }
    rc = src_dir->ops->rename(src_dir, old_name, dst_dir, new_name, 0);
    axfs_unref_inode(src_dir);
    axfs_unref_inode(dst_dir);
    return rc;
}

i32 axfs_sync(void)
{
    i32 rc = axfs_page_flush_all();
    i32 mount_failures = axfs_mount_sync_all();
    return rc != AXFS_OK ? rc : (mount_failures ? AXFS_EIO : AXFS_OK);
}

/* ------------------------------------------------------------------ */
/* Boot-time report                                                    */
/* ------------------------------------------------------------------ */

void axfs_report(void)
{
    axfs_mount_dump();
    axfs_page_dump();
    axfs_cache_dump();

    axfs_statfs_t stats;
    if (axfs_statfs("/", &stats) == AXFS_OK) {
        printk("vfs: root blocks=%llu free=%llu block=%llu name_max=%llu\n",
               stats.blocks, stats.free_blocks, stats.block_size, stats.name_max);
    }

    /* A small write/read/verify cycle, so a broken root filesystem shows up in
     * the boot log rather than on the first user command. */
    u32 marker = 0xA17F5EED;
    i32 fd = axfs_open("/vfs-selftest", AXFS_O_RDWR | AXFS_O_CREAT | AXFS_O_TRUNC,
                       0644);
    if (fd >= 0) {
        axfs_write(fd, &marker, sizeof(marker));
        axfs_close(fd);
        u32 readback = 0;
        fd = axfs_open("/vfs-selftest", AXFS_O_RDONLY, 0);
        if (fd >= 0) {
            axfs_read(fd, &readback, sizeof(readback));
            axfs_close(fd);
        }
        axfs_unlink("/vfs-selftest");
        printk("vfs: selftest %s (0x%x)\n",
               readback == marker ? "ok" : "mismatch", readback);
    } else {
        printk("vfs: selftest skipped, open failed\n");
    }
}
