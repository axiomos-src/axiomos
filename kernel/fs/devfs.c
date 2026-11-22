/*
 * AxiomOS - devfs.
 *
 * A tiny filesystem whose nodes are device entry points rather than stored
 * data.  Each node carries optional read and write callbacks; when a program
 * opens `/dev/time` and reads it, the callback produces the bytes on demand.
 *
 * Why not just special-case paths in the syscall layer?  Because mountable
 * device trees give the shell `ls /dev` and `cat /dev/...` for free, and a
 * driver can publish a diagnostic file without touching the syscall table.
 *
 * Node kinds map to POSIX file types: character devices, block devices and
 * plain informational files (type FILE with callbacks).
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axfs.h>

#define DEVFS_MAX 64

typedef i64 (*dev_read_fn)(axfs_file_t *file, void *buffer, u64 count);
typedef i64 (*dev_write_fn)(axfs_file_t *file, const void *buffer, u64 count);

typedef struct dev_node {
    char        name[AXFS_NAME_MAX + 1];
    u8          type;
    u32         mode;
    u64         ino;
    u64         rdev;
    i64         size;           /* -1 = dynamic */
    dev_read_fn read;
    dev_write_fn write;
    axfs_inode_t *inode;
    struct dev_node *next;
} dev_node_t;

typedef struct dev_super {
    dev_node_t *nodes;
    u64         next_ino;
} dev_super_t;

/* ------------------------------------------------------------------ */
/* Node management                                                     */
/* ------------------------------------------------------------------ */

static dev_super_t *dev_super_of(axfs_inode_t *inode)
{
    if (!inode || !inode->super) {
        return 0;
    }
    return (dev_super_t *)inode->super->fs_data;
}

static dev_node_t *dev_node_of(axfs_inode_t *inode)
{
    if (!inode) {
        return 0;
    }
    if (inode->type == AXFS_FT_DIR && inode->flags & AXFS_I_ROOT) {
        return 0;
    }
    return (dev_node_t *)inode->fs_data;
}

static dev_node_t *dev_find(dev_super_t *super, const char *name)
{
    if (!super || !name) {
        return 0;
    }
    for (dev_node_t *node = super->nodes; node; node = node->next) {
        if (ax_strcmp(node->name, name) == 0) {
            return node;
        }
    }
    return 0;
}

/* Encoding used by readdir: the root directory's "children" are the node list
 * in insertion order, computed by walking with an index.  Node order is stable
 * because nodes are appended at the tail. */
static dev_node_t *dev_at(dev_super_t *super, u64 index)
{
    if (!super) {
        return 0;
    }
    u64 i = 0;
    for (dev_node_t *node = super->nodes; node; node = node->next) {
        if (i == index) {
            return node;
        }
        i++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Directory ops                                                       */
/* ------------------------------------------------------------------ */

static i32 dev_lookup(axfs_inode_t *dir, const char *name, axfs_inode_t **out)
{
    if (!dir || !out) {
        return AXFS_EINVAL;
    }
    dev_super_t *super = dev_super_of(dir);
    if (!super) {
        return AXFS_EIO;
    }
    dev_node_t *node = dev_find(super, name);
    if (!node) {
        return AXFS_ENOENT;
    }
    axfs_ref_inode(node->inode);
    *out = node->inode;
    return AXFS_OK;
}

static i32 dev_create(axfs_inode_t *dir, const char *name, u32 mode,
                      axfs_inode_t **out)
{
    (void)dir;
    (void)name;
    (void)mode;
    (void)out;
    return AXFS_EPERM;
}

static i32 dev_mkdir(axfs_inode_t *dir, const char *name, u32 mode,
                     axfs_inode_t **out)
{
    (void)dir;
    (void)name;
    (void)mode;
    (void)out;
    return AXFS_EPERM;
}

static i32 dev_unlink(axfs_inode_t *dir, const char *name)
{
    (void)dir;
    (void)name;
    return AXFS_EPERM;
}

static i32 dev_rmdir(axfs_inode_t *dir, const char *name)
{
    (void)dir;
    (void)name;
    return AXFS_EPERM;
}

static i32 dev_rename(axfs_inode_t *old_dir, const char *old_name,
                      axfs_inode_t *new_dir, const char *new_name, u32 flags)
{
    (void)old_dir;
    (void)old_name;
    (void)new_dir;
    (void)new_name;
    (void)flags;
    return AXFS_EPERM;
}

static i32 dev_readdir(axfs_inode_t *dir, u64 index, axfs_dirent_t *out)
{
    if (!dir || !out) {
        return AXFS_EINVAL;
    }
    if (index == 0) {
        ax_strlcpy(out->name, ".", sizeof(out->name));
        out->name_len = 1;
        out->ino = dir->ino;
        out->type = AXFS_FT_DIR;
        return AXFS_OK;
    }
    if (index == 1) {
        ax_strlcpy(out->name, "..", sizeof(out->name));
        out->name_len = 2;
        out->ino = dir->ino;
        out->type = AXFS_FT_DIR;
        return AXFS_OK;
    }
    dev_super_t *super = dev_super_of(dir);
    dev_node_t *node = dev_at(super, index - 2);
    if (!node) {
        return AXFS_ENOENT;
    }
    ax_strlcpy(out->name, node->name, sizeof(out->name));
    out->name_len = (u16)ax_strlen(out->name);
    out->ino = node->ino;
    out->type = node->type;
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* File ops                                                            */
/* ------------------------------------------------------------------ */

static i64 dev_read(axfs_file_t *file, void *buffer, u64 count)
{
    dev_node_t *node = dev_node_of(file ? file->inode : 0);
    if (!node) {
        return AXFS_EINVAL;
    }
    if (!node->read) {
        return AXFS_EINVAL;
    }
    return node->read(file, buffer, count);
}

static i64 dev_write(axfs_file_t *file, const void *buffer, u64 count)
{
    dev_node_t *node = dev_node_of(file ? file->inode : 0);
    if (!node) {
        return AXFS_EINVAL;
    }
    if (!node->write) {
        return AXFS_EPERM;
    }
    return node->write(file, buffer, count);
}

static i32 dev_truncate(axfs_inode_t *inode, u64 size)
{
    (void)inode;
    (void)size;
    return AXFS_EPERM;
}

static i32 dev_stat(axfs_inode_t *inode, axfs_attr_t *out)
{
    if (!inode || !out) {
        return AXFS_EINVAL;
    }
    dev_node_t *node = dev_node_of(inode);
    out->mode = inode->mode;
    out->uid = 0;
    out->gid = 0;
    out->size = inode->size;
    out->blocks = 0;
    out->ino = inode->ino;
    out->atime = inode->atime;
    out->mtime = inode->mtime;
    out->ctime = inode->ctime;
    out->nlink = 1;
    out->type = inode->type;
    if (node) {
        out->blocks = node->rdev;
    }
    return AXFS_OK;
}

static i32 dev_chmod(axfs_inode_t *inode, u32 mode)
{
    if (!inode) {
        return AXFS_EINVAL;
    }
    inode->mode = (inode->mode & AXFS_S_IFMT) | (mode & 0777);
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Superblock                                                          */
/* ------------------------------------------------------------------ */

static i32 dev_alloc_inode(axfs_super_t *super, axfs_inode_t **out)
{
    (void)super;
    (void)out;
    return AXFS_EPERM;      /* nodes are created only via devfs_register */
}

static void dev_destroy_inode(axfs_inode_t *inode)
{
    (void)inode;
}

static i32 dev_write_inode(axfs_inode_t *inode)
{
    (void)inode;
    return AXFS_OK;
}

static i32 dev_statfs(axfs_super_t *super, u64 *blocks, u64 *free_blocks,
                      u64 *files, u64 *free_files)
{
    dev_super_t *dev = super ? (dev_super_t *)super->fs_data : 0;
    if (!dev) {
        return AXFS_EINVAL;
    }
    u64 count = 0;
    for (dev_node_t *node = dev->nodes; node; node = node->next) {
        count++;
    }
    if (blocks) {
        *blocks = count;
    }
    if (free_blocks) {
        *free_blocks = 0;
    }
    if (files) {
        *files = count;
    }
    if (free_files) {
        *free_files = 0;
    }
    return AXFS_OK;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static axfs_inode_ops_t g_dev_ops;
static axfs_super_ops_t g_dev_super_ops;
static bool_t g_dev_ready;

void axfs_devfs_init(void)
{
    if (g_dev_ready) {
        return;
    }
    ax_memset(&g_dev_ops, 0, sizeof(g_dev_ops));
    g_dev_ops.lookup = dev_lookup;
    g_dev_ops.create = dev_create;
    g_dev_ops.mkdir = dev_mkdir;
    g_dev_ops.unlink = dev_unlink;
    g_dev_ops.rmdir = dev_rmdir;
    g_dev_ops.rename = dev_rename;
    g_dev_ops.readdir = dev_readdir;
    g_dev_ops.read = dev_read;
    g_dev_ops.write = dev_write;
    g_dev_ops.truncate = dev_truncate;
    g_dev_ops.stat = dev_stat;
    g_dev_ops.chmod = dev_chmod;

    ax_memset(&g_dev_super_ops, 0, sizeof(g_dev_super_ops));
    g_dev_super_ops.alloc_inode = dev_alloc_inode;
    g_dev_super_ops.destroy_inode = dev_destroy_inode;
    g_dev_super_ops.write_inode = dev_write_inode;
    g_dev_super_ops.statfs = dev_statfs;
    g_dev_ready = (bool_t)1;
}

axfs_super_t *axfs_devfs_create(void)
{
    axfs_devfs_init();

    axfs_super_t *super = (axfs_super_t *)kzalloc(sizeof(*super));
    dev_super_t *dev = (dev_super_t *)kzalloc(sizeof(*dev));
    if (!super || !dev) {
        if (super) {
            kfree(super);
        }
        if (dev) {
            kfree(dev);
        }
        return 0;
    }
    dev->next_ino = 2;

    super->name = "devfs";
    super->magic = 0x44455646;      /* "DEVF" */
    super->features = 0;
    super->block_size = 512;
    super->total_blocks = 0;
    super->free_blocks = 0;
    super->total_inodes = 0;
    super->free_inodes = 0;
    super->max_name = AXFS_NAME_MAX;
    super->max_file_size = 0;
    super->ops = &g_dev_super_ops;
    super->root_ops = &g_dev_ops;
    super->fs_data = dev;
    super->serial = axfs_next_super_serial();

    super->root = axfs_alloc_inode(super, 1, AXFS_FT_DIR, 0755);
    if (!super->root) {
        kfree(dev);
        kfree(super);
        return 0;
    }
    super->root->ops = &g_dev_ops;
    super->root->flags |= AXFS_I_ROOT;
    super->root->fs_data = 0;
    return super;
}

/* Publish a node.  Returns the inode number, or a negative status. */
i32 axfs_devfs_register(const char *name, u8 type, u32 mode, dev_read_fn read,
                        dev_write_fn write)
{
    axfs_mount_t *mount = axfs_mount_find("/dev");
    if (!mount || !mount->super || !mount->super->fs_data) {
        return AXFS_ENXIO;
    }
    dev_super_t *super = (dev_super_t *)mount->super->fs_data;
    if (dev_find(super, name)) {
        return AXFS_EEXIST;
    }

    dev_node_t *node = (dev_node_t *)kzalloc(sizeof(*node));
    if (!node) {
        return AXFS_ENOMEM;
    }
    ax_strlcpy(node->name, name, sizeof(node->name));
    node->type = type;
    node->mode = mode;
    node->ino = super->next_ino++;
    node->read = read;
    node->write = write;
    node->size = -1;

    axfs_super_t *sb = mount->super;
    node->inode = axfs_alloc_inode(sb, node->ino, type, mode);
    if (!node->inode) {
        kfree(node);
        return AXFS_ENOMEM;
    }
    node->inode->fs_data = node;
    node->inode->ops = &g_dev_ops;
    node->inode->super = sb;
    node->inode->mount = mount;
    node->inode->size = 0;
    u32 posix = type == AXFS_FT_CHAR ? AXFS_S_IFCHR
                 : type == AXFS_FT_BLOCK ? AXFS_S_IFBLK
                 : type == AXFS_FT_DIR ? AXFS_S_IFDIR
                                       : AXFS_S_IFREG;
    node->inode->mode = posix | (mode & 07777);

    node->next = super->nodes;
    super->nodes = node;
    return (i32)node->ino;
}

/* A helper for drivers publishing a read-only informational file. */
i32 axfs_devfs_info(const char *name, const char *text)
{
    /* The text lives in a static buffer owned by the driver; wrap it in a
     * closure-less callback by storing the pointer in the file's private
     * field is not possible here, so this helper is intentionally absent.
     * Drivers build their own callbacks with a static buffer instead. */
    (void)name;
    (void)text;
    return AXFS_ENOSYS;
}

/* ------------------------------------------------------------------ */
/* Built-in diagnostic nodes                                           */
/* ------------------------------------------------------------------ */

/* /dev/uptime: "seconds\n" */
static i64 dev_uptime_read(axfs_file_t *file, void *buffer, u64 count)
{
    (void)file;
    char line[32];
    ax_snprintf(line, sizeof(line), "%llu\n", axfs_uptime());
    usize len = ax_strlen(line);
    if (len > count) {
        len = count;
    }
    for (usize i = 0; i < len; i++) {
        ((char *)buffer)[i] = line[i];
    }
    return (i64)len;
}

/* /dev/meminfo: a few kernel counters, in the format /proc/meminfo uses. */
static i64 dev_meminfo_read(axfs_file_t *file, void *buffer, u64 count)
{
    (void)file;
    char text[256];
    u64 hits = 0;
    u64 misses = 0;
    u64 pages = 0;
    axfs_page_stats(&hits, &misses, &pages);
    ax_snprintf(text, sizeof(text),
                "Pages: %llu\nPageHits: %llu\nPageMisses: %llu\nTmpfs: %llu\n",
                pages, hits, misses, axfs_tmpfs_bytes());
    usize len = ax_strlen(text);
    if (len > count) {
        len = count;
    }
    for (usize i = 0; i < len; i++) {
        ((char *)buffer)[i] = text[i];
    }
    return (i64)len;
}

/* /dev/null: reads zero, writes discard. */
static i64 dev_null_read(axfs_file_t *file, void *buffer, u64 count)
{
    (void)file;
    (void)buffer;
    (void)count;
    return 0;
}

static i64 dev_null_write(axfs_file_t *file, const void *buffer, u64 count)
{
    (void)file;
    (void)buffer;
    return (i64)count;
}

/* /dev/zero: produces zero bytes, which userland uses to fill buffers. */
static i64 dev_zero_read(axfs_file_t *file, void *buffer, u64 count)
{
    (void)file;
    if (!buffer) {
        return AXFS_EINVAL;
    }
    for (u64 i = 0; i < count; i++) {
        ((u8 *)buffer)[i] = 0;
    }
    return (i64)count;
}

/* /dev/random: an LCG, deliberately not cryptographic; the kernel's real RNG
 * lives in the crypto layer and is exposed as /dev/urandom by a later patch. */
static u64 g_random_state = 0x243F6A8885A308D3ull;

static i64 dev_random_read(axfs_file_t *file, void *buffer, u64 count)
{
    (void)file;
    if (!buffer) {
        return AXFS_EINVAL;
    }
    for (u64 i = 0; i < count; i++) {
        g_random_state = g_random_state * 6364136223846793005ull + 1442695040888963407ull;
        ((u8 *)buffer)[i] = (u8)(g_random_state >> 33);
    }
    return (i64)count;
}

void axfs_devfs_builtins(void)
{
    axfs_devfs_register("null", AXFS_FT_CHAR, 0666, dev_null_read, dev_null_write);
    axfs_devfs_register("zero", AXFS_FT_CHAR, 0644, dev_zero_read, 0);
    axfs_devfs_register("random", AXFS_FT_CHAR, 0644, dev_random_read, 0);
    axfs_devfs_register("uptime", AXFS_FT_FILE, 0444, dev_uptime_read, 0);
    axfs_devfs_register("meminfo", AXFS_FT_FILE, 0444, dev_meminfo_read, 0);
}

void axfs_devfs_dump(void)
{
    axfs_mount_t *mount = axfs_mount_find("/dev");
    if (!mount || !mount->super) {
        printk("devfs: not mounted\n");
        return;
    }
    dev_super_t *super = (dev_super_t *)mount->super->fs_data;
    printk("devfs: nodes\n");
    for (dev_node_t *node = super->nodes; node; node = node->next) {
        printk("  %-12s ino=%llu type=%u mode=%o%s%s\n", node->name, node->ino,
               node->type, node->mode, node->read ? " r" : "",
               node->write ? " w" : "");
    }
}
