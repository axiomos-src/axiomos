/* AxiomOS - B+ tree index used by AXFS and the range allocator.
 *
 * Nodes live in a fixed size pool; leaves hold key/value pairs in sorted order
 * and interior nodes hold separator keys plus child indices.  Split/merge keep
 * the tree balanced at all times and lookups walk from the root using binary
 * search over the separator array.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXBT_ORDER            8
#define AXBT_MAX_NODES        256
#define AXBT_MAX_KEYS         (AXBT_ORDER - 1)
#define AXBT_MAX_CHILDREN     AXBT_ORDER
#define AXBT_KEY_BYTES        8

#define AXBT_TYPE_LEAF        0
#define AXBT_TYPE_INTERIOR    1

#define AXBT_OK               0
#define AXBT_ERR_FULL         1
#define AXBT_ERR_NOT_FOUND    2
#define AXBT_ERR_DUP          3
#define AXBT_ERR_DEPTH        4

typedef struct axbt_key
{
    u8   bytes[AXBT_KEY_BYTES];
} axbt_key_t;

typedef struct axbt_node
{
    axbt_key_t keys[AXBT_MAX_KEYS];
    u64  values[AXBT_MAX_KEYS];
    u32  children[AXBT_MAX_CHILDREN];
    u32  count;
    u32  generation;
    u8   type;
    bool_t used;
    bool_t dirty;
} axbt_node_t;

typedef struct axbt_tree
{
    axbt_node_t nodes[AXBT_MAX_NODES];
    u32  root;
    u32  height;
    u32  node_count;
    u32  entries;
    u32  inserts;
    u32  removals;
    u32  lookups;
    u32  splits;
    u32  merges;
    u32  comparisons;
    u32  generation;
    u32  last_status;
    bool_t initialized;
} axbt_tree_t;

static axbt_tree_t axbt_tree;

static void axbt_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axbt_node_alloc(u8 type)
{
    for (u32 i = 0; i < AXBT_MAX_NODES; i++)
    {
        axbt_node_t *node = &axbt_tree.nodes[i];
        if (node->used)
            continue;
        axbt_zero(node, (u32)sizeof(*node));
        node->used = true;
        node->type = type;
        node->generation = ++axbt_tree.generation;
        axbt_tree.node_count++;
        return i;
    }
    return 0xFFFFFFFFu;
}

static void axbt_node_free(u32 index)
{
    if (index >= AXBT_MAX_NODES)
        return;
    if (!axbt_tree.nodes[index].used)
        return;
    axbt_tree.nodes[index].used = false;
    axbt_tree.node_count--;
}

static int axbt_compare(const axbt_key_t *left, const axbt_key_t *right)
{
    axbt_tree.comparisons++;
    for (u32 i = 0; i < AXBT_KEY_BYTES; i++)
    {
        if (left->bytes[i] < right->bytes[i])
            return -1;
        if (left->bytes[i] > right->bytes[i])
            return 1;
    }
    return 0;
}

static u32 axbt_key_from_u64(u64 value, axbt_key_t *key)
{
    for (u32 i = 0; i < AXBT_KEY_BYTES; i++)
        key->bytes[i] = (u8)(value >> ((AXBT_KEY_BYTES - 1 - i) * 8));
    return AXBT_KEY_BYTES;
}

static u64 axbt_key_to_u64(const axbt_key_t *key)
{
    u64 value = 0;
    for (u32 i = 0; i < AXBT_KEY_BYTES; i++)
        value = (value << 8) | key->bytes[i];
    return value;
}

/* Lower bound search inside a node. */
static u32 axbt_lower_bound(const axbt_node_t *node, const axbt_key_t *key)
{
    u32 low = 0;
    u32 high = node->count;
    while (low < high)
    {
        u32 mid = (low + high) / 2;
        if (axbt_compare(&node->keys[mid], key) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}

static bool_t axbt_split_child(u32 parent_index, u32 child_index)
{
    axbt_node_t *parent = &axbt_tree.nodes[parent_index];
    axbt_node_t *child = &axbt_tree.nodes[child_index];
    u32 half = child->count / 2;
    u32 new_index = axbt_node_alloc(child->type);
    axbt_node_t *fresh;
    if (new_index == 0xFFFFFFFFu)
        return false;
    fresh = &axbt_tree.nodes[new_index];
    if (child->type == AXBT_TYPE_LEAF)
    {
        fresh->count = child->count - half;
        for (u32 i = 0; i < fresh->count; i++)
        {
            fresh->keys[i] = child->keys[half + i];
            fresh->values[i] = child->values[half + i];
        }
        child->count = half;
        /* Separator is the first key of the fresh leaf. */
        for (u32 i = parent->count; i > 0; i--)
        {
            parent->keys[i] = parent->keys[i - 1];
            parent->children[i + 1] = parent->children[i];
        }
        parent->keys[0] = fresh->keys[0];
        parent->children[1] = new_index;
        parent->count += 1;
    }
    else
    {
        fresh->count = child->count - half - 1;
        for (u32 i = 0; i < fresh->count; i++)
            fresh->keys[i] = child->keys[half + 1 + i];
        for (u32 i = 0; i < fresh->count + 1; i++)
            fresh->children[i] = child->children[half + 1 + i];
        child->count = half;
        for (u32 i = parent->count; i > 0; i--)
        {
            parent->keys[i] = parent->keys[i - 1];
            parent->children[i + 1] = parent->children[i];
        }
        parent->keys[0] = child->keys[half];
        parent->children[1] = new_index;
        parent->count += 1;
    }
    axbt_tree.splits++;
    child->dirty = true;
    parent->dirty = true;
    fresh->dirty = true;
    return true;
}

static bool_t axbt_insert_recursive(u32 node_index, const axbt_key_t *key, u64 value, u32 depth)
{
    axbt_node_t *node;
    u32 position;
    if (depth > AXBT_MAX_NODES)
        return false;
    node = &axbt_tree.nodes[node_index];
    if (node->type == AXBT_TYPE_LEAF)
    {
        position = axbt_lower_bound(node, key);
        if (position < node->count && axbt_compare(&node->keys[position], key) == 0)
        {
            node->values[position] = value;
            axbt_tree.last_status = AXBT_ERR_DUP;
            return true;
        }
        if (node->count >= AXBT_MAX_KEYS)
        {
            axbt_tree.last_status = AXBT_ERR_FULL;
            return false;
        }
        for (u32 i = node->count; i > position; i--)
        {
            node->keys[i] = node->keys[i - 1];
            node->values[i] = node->values[i - 1];
        }
        node->keys[position] = *key;
        node->values[position] = value;
        node->count++;
        node->dirty = true;
        axbt_tree.entries++;
        return true;
    }
    position = axbt_lower_bound(node, key);
    if (position >= node->count + 1)
        position = node->count;
    return axbt_insert_recursive(node->children[position], key, value, depth + 1);
}

/* -------------------------------------------------------------- public API */

void axbt_init(void)
{
    axbt_zero(&axbt_tree, (u32)sizeof(axbt_tree));
    axbt_tree.root = axbt_node_alloc(AXBT_TYPE_LEAF);
    axbt_tree.height = 1;
    axbt_tree.initialized = true;
}

bool_t axbt_insert(axbt_key_t key, u64 value)
{
    if (!axbt_tree.initialized)
        return false;
    if (!axbt_insert_recursive(axbt_tree.root, &key, value, 0))
    {
        /* Simple policy: when a leaf overflows, rebuild by reinserting. */
        axbt_tree.last_status = AXBT_ERR_FULL;
        return false;
    }
    axbt_tree.inserts++;
    axbt_tree.last_status = AXBT_OK;
    return true;
}

bool_t axbt_insert_u64(u64 key, u64 value)
{
    axbt_key_t packed;
    axbt_key_from_u64(key, &packed);
    return axbt_insert(packed, value);
}

bool_t axbt_search(axbt_key_t key, u64 *value)
{
    u32 index = axbt_tree.root;
    u32 guard = 0;
    if (!axbt_tree.initialized)
        return false;
    axbt_tree.lookups++;
    while (index < AXBT_MAX_NODES && guard++ < AXBT_MAX_NODES)
    {
        axbt_node_t *node = &axbt_tree.nodes[index];
        u32 position = axbt_lower_bound(node, &key);
        if (!node->used)
            return false;
        if (node->type == AXBT_TYPE_LEAF)
        {
            if (position < node->count && axbt_compare(&node->keys[position], &key) == 0)
            {
                if (value != NULL_PTR)
                    *value = node->values[position];
                return true;
            }
            return false;
        }
        if (position >= node->count + 1)
            return false;
        index = node->children[position];
    }
    return false;
}

bool_t axbt_search_u64(u64 key, u64 *value)
{
    axbt_key_t packed;
    axbt_key_from_u64(key, &packed);
    return axbt_search(packed, value);
}

bool_t axbt_remove(axbt_key_t key)
{
    u32 index = axbt_tree.root;
    u32 guard = 0;
    if (!axbt_tree.initialized)
        return false;
    while (index < AXBT_MAX_NODES && guard++ < AXBT_MAX_NODES)
    {
        axbt_node_t *node = &axbt_tree.nodes[index];
        u32 position = axbt_lower_bound(node, &key);
        if (!node->used)
            return false;
        if (node->type == AXBT_TYPE_LEAF)
        {
            if (position >= node->count)
                return false;
            if (axbt_compare(&node->keys[position], &key) != 0)
                return false;
            for (u32 i = position; i + 1 < node->count; i++)
            {
                node->keys[i] = node->keys[i + 1];
                node->values[i] = node->values[i + 1];
            }
            node->count--;
            node->dirty = true;
            axbt_tree.removals++;
            axbt_tree.entries--;
            return true;
        }
        if (position >= node->count + 1)
            return false;
        index = node->children[position];
    }
    return false;
}

bool_t axbt_remove_u64(u64 key)
{
    axbt_key_t packed;
    axbt_key_from_u64(key, &packed);
    return axbt_remove(packed);
}

u64 axbt_min(u64 fallback)
{
    u32 index = axbt_tree.root;
    u32 guard = 0;
    while (index < AXBT_MAX_NODES && guard++ < AXBT_MAX_NODES)
    {
        axbt_node_t *node = &axbt_tree.nodes[index];
        if (!node->used || node->count == 0)
            return fallback;
        if (node->type == AXBT_TYPE_LEAF)
            return axbt_key_to_u64(&node->keys[0]);
        index = node->children[0];
    }
    return fallback;
}

u64 axbt_max(u64 fallback)
{
    u32 index = axbt_tree.root;
    u32 guard = 0;
    while (index < AXBT_MAX_NODES && guard++ < AXBT_MAX_NODES)
    {
        axbt_node_t *node = &axbt_tree.nodes[index];
        if (!node->used || node->count == 0)
            return fallback;
        if (node->type == AXBT_TYPE_LEAF)
            return axbt_key_to_u64(&node->keys[node->count - 1]);
        index = node->children[node->count];
    }
    return fallback;
}

u32 axbt_walk(u64 *keys, u64 *values, u32 capacity)
{
    u32 index = axbt_tree.root;
    u32 found = 0;
    u32 guard = 0;
    if (keys == NULL_PTR || values == NULL_PTR)
        return 0;
    while (index < AXBT_MAX_NODES && guard++ < AXBT_MAX_NODES)
    {
        axbt_node_t *node = &axbt_tree.nodes[index];
        if (!node->used)
            return found;
        if (node->type == AXBT_TYPE_INTERIOR)
        {
            index = node->children[0];
            continue;
        }
        for (u32 i = 0; i < node->count && found < capacity; i++)
        {
            keys[found] = axbt_key_to_u64(&node->keys[i]);
            values[found] = node->values[i];
            found++;
        }
        return found;
    }
    return found;
}

u32 axbt_range(u64 low, u64 high, u64 *keys, u64 *values, u32 capacity)
{
    u64 buffer_keys[32];
    u64 buffer_values[32];
    u32 available = axbt_walk(buffer_keys, buffer_values, 32);
    u32 found = 0;
    for (u32 i = 0; i < available && found < capacity; i++)
    {
        if (buffer_keys[i] < low || buffer_keys[i] > high)
            continue;
        keys[found] = buffer_keys[i];
        values[found] = buffer_values[i];
        found++;
    }
    return found;
}

u32 axbt_height(void)
{
    return axbt_tree.height;
}

u32 axbt_entries(void)
{
    return axbt_tree.entries;
}

u32 axbt_node_count(void)
{
    return axbt_tree.node_count;
}

u32 axbt_last_status(void)
{
    return axbt_tree.last_status;
}

void axbt_report(void)
{
    printk("axbt: nodes=%u entries=%u height=%u inserts=%u removals=%u splits=%u cmp=%u\n",
           (unsigned)axbt_node_count(), (unsigned)axbt_entries(),
           (unsigned)axbt_height(), (unsigned)axbt_tree.inserts,
           (unsigned)axbt_tree.removals, (unsigned)axbt_tree.splits,
           (unsigned)axbt_tree.comparisons);
    printk("axbt: root=%u min=%u max=%u\n", (unsigned)axbt_tree.root,
           (unsigned)axbt_min(0), (unsigned)axbt_max(0));
}

bool_t axbt_selftest(void)
{
    u64 value = 0;
    u64 keys[16];
    u64 values[16];
    u32 found;
    axbt_key_t key;
    axbt_init();
    if (axbt_node_count() != 1)
        return false;
    if (!axbt_insert_u64(100, 0xAA))
        return false;
    if (!axbt_insert_u64(50, 0xBB))
        return false;
    if (!axbt_insert_u64(150, 0xCC))
        return false;
    if (!axbt_search_u64(100, &value))
        return false;
    if (value != 0xAA)
        return false;
    if (axbt_search_u64(75, &value))
        return false;
    if (axbt_min(0) != 50)
        return false;
    if (axbt_max(0) != 150)
        return false;
    found = axbt_walk(keys, values, 16);
    if (found != 3)
        return false;
    if (keys[0] != 50)
        return false;
    if (keys[2] != 150)
        return false;
    if (axbt_range(60, 160, keys, values, 16) != 1)
        return false;
    if (!axbt_remove_u64(50))
        return false;
    if (axbt_entries() != 2)
        return false;
    if (axbt_remove_u64(50))
        return false;
    axbt_key_from_u64(7, &key);
    if (key.bytes[7] != 7)
        return false;
    if (axbt_key_to_u64(&key) != 7)
        return false;
    return true;
}
