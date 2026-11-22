/* AxiomOS - RAM file system.
 *
 * An in-memory tree used for /tmp, the boot log and the pseudo files exposed
 * to the GUI (for example /proc/version).  Node contents live in the kernel
 * heap.  The tree is intentionally simple: no locks yet (single CPU, and all
 * mutations happen from the main thread), directories are flat lists.
 */

#include <axiom/kernel.h>

#define RAMFS_MAX_NAME 48

struct ramfs_node {
    char  name[RAMFS_MAX_NAME];
    u8    is_dir;
    u64   size;
    u8   *data;
    struct ramfs_node *parent;
    struct ramfs_node *children;
    struct ramfs_node *next;
};

static struct ramfs_node *g_root;
static u64 g_ramfs_bytes;
static u32 g_ramfs_nodes;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static bool_t name_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

static struct ramfs_node *ramfs_new_node(const char *name, u8 is_dir,
                                         struct ramfs_node *parent)
{
    struct ramfs_node *node = (struct ramfs_node *)kzalloc(sizeof(*node));
    if (!node)
        return NULL_PTR;
    u32 n = 0;
    while (name[n] && n < RAMFS_MAX_NAME - 1) {
        node->name[n] = name[n];
        n++;
    }
    node->name[n] = 0;
    node->is_dir = is_dir;
    node->parent = parent;
    return node;
}

static void ramfs_insert(struct ramfs_node *dir, struct ramfs_node *node)
{
    node->next = dir->children;
    dir->children = node;
    g_ramfs_nodes++;
}

static struct ramfs_node *ramfs_find_child(struct ramfs_node *dir, const char *name)
{
    for (struct ramfs_node *c = dir->children; c; c = c->next) {
        if (name_equal(c->name, name))
            return c;
    }
    return NULL_PTR;
}

/* Resolve a path to a node.  With create set, missing components are created
 * as directories (used for intermediate components only). */
static struct ramfs_node *ramfs_lookup(const char *path, bool_t create_dirs)
{
    struct ramfs_node *dir = g_root;
    const char *cursor = path;

    if (!g_root)
        return NULL_PTR;
    while (*cursor == '/')
        cursor++;

    while (*cursor) {
        const char *slash = cursor;
        while (*slash && *slash != '/')
            slash++;
        u32 length = (u32)(slash - cursor);
        if (!length || length >= RAMFS_MAX_NAME)
            return NULL_PTR;

        char component[RAMFS_MAX_NAME];
        memcpy(component, cursor, length);
        component[length] = 0;

        struct ramfs_node *child = ramfs_find_child(dir, component);
        if (!child) {
            if (!create_dirs)
                return NULL_PTR;
            child = ramfs_new_node(component, 1, dir);
            if (!child)
                return NULL_PTR;
            ramfs_insert(dir, child);
        }
        if (*slash == '/' && !child->is_dir)
            return NULL_PTR;
        dir = child;
        cursor += length;
        while (*cursor == '/')
            cursor++;
    }
    return dir;
}

static const char *path_leaf(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            last = p + 1;
    }
    return last;
}

static const char *path_parent_prefix(const char *path)
{
    const char *last = path;
    const char *p = path;
    for (; *p; p++) {
        if (*p == '/')
            last = p;
    }
    return last == path ? NULL_PTR : last;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void ramfs_init(void)
{
    g_root = ramfs_new_node("/", 1, NULL_PTR);
    g_ramfs_bytes = 0;
    g_ramfs_nodes = 0;
    if (!g_root) {
        printk("ramfs: cannot allocate the root node\n");
        return;
    }
    g_ramfs_nodes = 1;
    printk("ramfs: root directory created\n");
}

bool_t ramfs_create(const char *path, u64 size)
{
    struct ramfs_node *dir;
    const char *leaf = path_leaf(path);

    if (!g_root || !*leaf)
        return false;
    dir = ramfs_lookup(path, true);
    if (!dir)
        return false;
    /* ramfs_lookup(path) resolves to the full path; for a file we need its
     * parent, so resolve the prefix explicitly. */
    {
        char prefix[128];
        const char *parent_end = path_parent_prefix(path);
        u32 length = parent_end ? (u32)(parent_end - path) : 0;
        if (parent_end) {
            if (length >= sizeof(prefix))
                return false;
            memcpy(prefix, path, length);
            prefix[length] = 0;
            dir = ramfs_lookup(prefix, true);
        } else {
            dir = g_root;
        }
    }
    if (!dir)
        return false;
    if (ramfs_find_child(dir, leaf))
        return false;

    struct ramfs_node *file = ramfs_new_node(leaf, 0, dir);
    if (!file)
        return false;
    if (size) {
        file->data = (u8 *)kzalloc(size);
        if (!file->data) {
            kfree(file);
            return false;
        }
        file->size = size;
        g_ramfs_bytes += size;
    }
    ramfs_insert(dir, file);
    return true;
}

bool_t ramfs_write(const char *path, const void *data, u64 size)
{
    struct ramfs_node *node = ramfs_lookup(path, false);
    if (!node) {
        if (!ramfs_create(path, size))
            return false;
        node = ramfs_lookup(path, false);
        if (!node)
            return false;
    } else if (node->is_dir) {
        return false;
    }

    if (node->size < size) {
        u8 *fresh = (u8 *)kzalloc(size);
        if (!fresh)
            return false;
        if (node->data)
            memcpy(fresh, node->data, node->size);
        if (node->data) {
            g_ramfs_bytes -= node->size;
            kfree(node->data);
        }
        node->data = fresh;
        g_ramfs_bytes += size;
        node->size = size;
    }
    if (size)
        memcpy(node->data, data, size);
    return true;
}

bool_t ramfs_read(const char *path, void **data, u64 *size)
{
    struct ramfs_node *node = ramfs_lookup(path, false);
    if (!node || node->is_dir)
        return false;
    *data = node->data;
    *size = node->size;
    return true;
}

bool_t ramfs_stat(const char *path, u64 *size, u8 *is_dir)
{
    struct ramfs_node *node = ramfs_lookup(path, false);
    if (!node)
        return false;
    if (size)
        *size = node->size;
    if (is_dir)
        *is_dir = node->is_dir;
    return true;
}

bool_t ramfs_list(const char *path)
{
    struct ramfs_node *dir = ramfs_lookup(path, false);
    if (!dir || !dir->is_dir)
        return false;
    printk("ramfs: %s\n", path);
    for (struct ramfs_node *c = dir->children; c; c = c->next)
        printk("  %s%s %u bytes\n", c->name, c->is_dir ? "/" : "", c->size);
    return true;
}

u64 ramfs_bytes(void)
{
    return g_ramfs_bytes;
}

u32 ramfs_node_count(void)
{
    return g_ramfs_nodes;
}

void ramfs_dump(void)
{
    printk("ramfs: %u nodes, %u bytes\n", g_ramfs_nodes, g_ramfs_bytes);
}
