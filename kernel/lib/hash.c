/* AxiomOS - hash map for the kernel.
 *
 * Open addressing with linear probing over a power of two bucket array, FNV-1a
 * string hashing, tombstones for deletion, and a grow path that rehashes into
 * a caller supplied spare array so that the allocator is never called from
 * interrupt context.
 */

#include <axiom/kernel.h>

#define AXHS_MIN_BUCKETS   16
#define AXHS_MAX_BUCKETS   4096
#define AXHS_MAX_KEY       48

enum axhs_slot_state
{
    AXHS_SLOT_EMPTY = 0,
    AXHS_SLOT_FULL,
    AXHS_SLOT_TOMB
};

typedef struct axhs_slot
{
    u64  hash;
    u64  value;
    u8   state;
    u8   key_length;
    char key[AXHS_MAX_KEY];
} axhs_slot_t;

typedef struct axhs_map
{
    axhs_slot_t *slots;
    u32          capacity;
    u32          count;
    u32          tombstones;
    u32          probes;
    u32          resize_events;
    bool_t       frozen;
} axhs_map_t;

static void axhs_zero(void *ptr, u32 size);
static u32  axhs_strlen(const char *text);
static u64  axhs_hash(const char *key, u32 length);
static u32  axhs_next_power(u32 value);
static u32  axhs_find_slot(const axhs_map_t *map, u64 hash, const char *key,
                           u32 length, bool_t *found);
static bool_t axhs_put(axhs_map_t *map, const char *key, u32 length, u64 value);
static bool_t axhs_get(const axhs_map_t *map, const char *key, u32 length,
                       u64 *value);
static bool_t axhs_remove(axhs_map_t *map, const char *key, u32 length);
static void axhs_clear(axhs_map_t *map);
static bool_t axhs_grow(axhs_map_t *map, axhs_slot_t *spare, u32 spare_capacity);
static u32  axhs_load_percent(const axhs_map_t *map);

static u32 axhs_strlen(const char *text)
{
    u32 i = 0;
    while (text[i] != 0)
        i++;
    return i;
}

static void axhs_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u64 axhs_hash(const char *key, u32 length)
{
    u64 hash = 0xCBF29CE484222325ULL;
    u32 i;
    for (i = 0; i < length; i++)
    {
        hash ^= (u64)(u8)key[i];
        hash *= 0x100000001B3ULL;
    }
    hash ^= hash >> 33;
    hash *= 0xFF51AFD7ED558CCDULL;
    hash ^= hash >> 33;
    return hash;
}

static u32 axhs_next_power(u32 value)
{
    u32 result = AXHS_MIN_BUCKETS;
    while (result < value && result < AXHS_MAX_BUCKETS)
        result <<= 1;
    return result;
}

static u32 axhs_find_slot(const axhs_map_t *map, u64 hash, const char *key,
                          u32 length, bool_t *found)
{
    u32 mask;
    u32 index;
    u32 probes = 0;
    u32 tombstone = 0xFFFFFFFFu;
    if (map->capacity == 0)
    {
        *found = false;
        return 0xFFFFFFFFu;
    }
    mask = map->capacity - 1;
    index = (u32)hash & mask;
    while (probes < map->capacity)
    {
        const axhs_slot_t *slot = &map->slots[index];
        if (slot->state == AXHS_SLOT_EMPTY)
        {
            *found = false;
            return tombstone != 0xFFFFFFFFu ? tombstone : index;
        }
        if (slot->state == AXHS_SLOT_TOMB)
        {
            if (tombstone == 0xFFFFFFFFu)
                tombstone = index;
        }
        else if (slot->hash == hash && slot->key_length == length)
        {
            u32 i;
            bool_t equal = true;
            for (i = 0; i < length; i++)
            {
                if (slot->key[i] != key[i])
                {
                    equal = false;
                    break;
                }
            }
            if (equal)
            {
                *found = true;
                return index;
            }
        }
        index = (index + 1) & mask;
        probes++;
    }
    *found = false;
    return tombstone;
}

static bool_t axhs_put(axhs_map_t *map, const char *key, u32 length, u64 value)
{
    bool_t found = false;
    u32 index;
    axhs_slot_t *slot;
    u64 hash;
    if (map->frozen || map->slots == NULL_PTR || length >= AXHS_MAX_KEY)
        return false;
    hash = axhs_hash(key, length);
    index = axhs_find_slot(map, hash, key, length, &found);
    if (index == 0xFFFFFFFFu)
        return false;
    slot = &map->slots[index];
    map->probes++;
    if (!found)
    {
        if (map->count + map->tombstones + 1 >= map->capacity / 4 * 3)
            return false;
        if (slot->state == AXHS_SLOT_TOMB)
            map->tombstones--;
        map->count++;
        slot->key_length = (u8)length;
        {
            u32 i;
            for (i = 0; i < length; i++)
                slot->key[i] = key[i];
            if (length < AXHS_MAX_KEY)
                slot->key[length] = 0;
        }
    }
    slot->hash = hash;
    slot->state = AXHS_SLOT_FULL;
    slot->value = value;
    return true;
}

static bool_t axhs_get(const axhs_map_t *map, const char *key, u32 length,
                       u64 *value)
{
    bool_t found = false;
    u32 index;
    u64 hash;
    if (map->slots == NULL_PTR || length >= AXHS_MAX_KEY)
        return false;
    hash = axhs_hash(key, length);
    index = axhs_find_slot(map, hash, key, length, &found);
    if (!found || index == 0xFFFFFFFFu)
        return false;
    *value = map->slots[index].value;
    return true;
}

static bool_t axhs_remove(axhs_map_t *map, const char *key, u32 length)
{
    bool_t found = false;
    u32 index;
    u64 hash;
    if (map->frozen || map->slots == NULL_PTR)
        return false;
    hash = axhs_hash(key, length);
    index = axhs_find_slot(map, hash, key, length, &found);
    if (!found || index == 0xFFFFFFFFu)
        return false;
    map->slots[index].state = AXHS_SLOT_TOMB;
    map->slots[index].value = 0;
    map->count--;
    map->tombstones++;
    return true;
}

static void axhs_clear(axhs_map_t *map)
{
    u32 i;
    if (map->slots == NULL_PTR)
        return;
    for (i = 0; i < map->capacity; i++)
        map->slots[i].state = AXHS_SLOT_EMPTY;
    map->count = 0;
    map->tombstones = 0;
}

static bool_t axhs_grow(axhs_map_t *map, axhs_slot_t *spare, u32 spare_capacity)
{
    u32 capacity = axhs_next_power(map->capacity * 2);
    u32 i;
    if (spare == NULL_PTR || spare_capacity < capacity)
        return false;
    axhs_zero(spare, capacity * (u32)sizeof(axhs_slot_t));
    {
        axhs_map_t target;
        target.slots = spare;
        target.capacity = capacity;
        target.count = 0;
        target.tombstones = 0;
        target.probes = 0;
        target.resize_events = 0;
        target.frozen = false;
        for (i = 0; i < map->capacity; i++)
        {
            const axhs_slot_t *slot = &map->slots[i];
            if (slot->state != AXHS_SLOT_FULL)
                continue;
            axhs_put(&target, slot->key, slot->key_length, slot->value);
        }
        map->slots = spare;
        map->capacity = capacity;
        map->count = target.count;
        map->tombstones = 0;
        map->resize_events++;
    }
    return true;
}

static u32 axhs_load_percent(const axhs_map_t *map)
{
    if (map->capacity == 0)
        return 0;
    return (map->count * 100) / map->capacity;
}

/* -------------------------------------------------------------- public API */

bool_t axhs_init(axhs_map_t *map, axhs_slot_t *storage, u32 capacity)
{
    if (storage == NULL_PTR)
        return false;
    axhs_zero(map, (u32)sizeof(*map));
    map->capacity = axhs_next_power(capacity < AXHS_MIN_BUCKETS ? AXHS_MIN_BUCKETS
                                                                : capacity);
    map->slots = storage;
    axhs_clear(map);
    return true;
}

bool_t axhs_set(axhs_map_t *map, const char *key, u64 value)
{
    u32 length = (u32)axhs_strlen(key);
    if (length >= AXHS_MAX_KEY)
        return false;
    if (map->count + map->tombstones + 1 >= map->capacity / 4 * 3)
    {
        if (map->count * 4 >= map->capacity)
            return false;
        {
            /* in place compaction: clear tombstones only */
            u32 i;
            u32 writes = 0;
            for (i = 0; i < map->capacity && writes < map->tombstones; i++)
            {
                if (map->slots[i].state == AXHS_SLOT_TOMB)
                {
                    map->slots[i].state = AXHS_SLOT_EMPTY;
                    writes++;
                }
            }
            map->tombstones = 0;
        }
    }
    return axhs_put(map, key, length, value);
}

bool_t axhs_lookup(const axhs_map_t *map, const char *key, u64 *value)
{
    return axhs_get(map, key, (u32)axhs_strlen(key), value);
}

bool_t axhs_delete(axhs_map_t *map, const char *key)
{
    return axhs_remove(map, key, (u32)axhs_strlen(key));
}

u32 axhs_size(const axhs_map_t *map)
{
    return map->count;
}

u32 axhs_capacity(const axhs_map_t *map)
{
    return map->capacity;
}

u32 axhs_load(const axhs_map_t *map)
{
    return axhs_load_percent(map);
}

bool_t axhs_resize(axhs_map_t *map, axhs_slot_t *spare, u32 spare_capacity)
{
    return axhs_grow(map, spare, spare_capacity);
}

void axhs_freeze(axhs_map_t *map)
{
    map->frozen = true;
}

void axhs_thaw(axhs_map_t *map)
{
    map->frozen = false;
}

u32 axhs_walk(const axhs_map_t *map, bool_t (*visit)(const char *key, u64 value,
                                                     void *context),
              void *context)
{
    u32 i;
    u32 visited = 0;
    if (visit == NULL_PTR)
        return 0;
    for (i = 0; i < map->capacity; i++)
    {
        const axhs_slot_t *slot = &map->slots[i];
        if (slot->state != AXHS_SLOT_FULL)
            continue;
        visit(slot->key, slot->value, context);
        visited++;
    }
    return visited;
}

void axhs_dump(const axhs_map_t *map)
{
    printk("axhs: count=%u capacity=%u tomatoes=%u probes=%u load=%u%%\n",
           (unsigned)map->count, (unsigned)map->capacity,
           (unsigned)map->tombstones, (unsigned)map->probes,
           (unsigned)axhs_load_percent(map));
}

bool_t axhs_selftest(void)
{
    static axhs_slot_t storage[AXHS_MAX_BUCKETS];
    static axhs_map_t map;
    u64 value = 0;
    if (!axhs_init(&map, storage, 64))
        return false;
    if (!axhs_set(&map, "alpha", 1))
        return false;
    if (!axhs_set(&map, "beta", 2))
        return false;
    if (!axhs_set(&map, "gamma", 3))
        return false;
    if (!axhs_lookup(&map, "beta", &value) || value != 2)
        return false;
    if (!axhs_set(&map, "beta", 22))
        return false;
    if (!axhs_lookup(&map, "beta", &value) || value != 22)
        return false;
    if (axhs_size(&map) != 3)
        return false;
    if (!axhs_delete(&map, "alpha"))
        return false;
    if (axhs_lookup(&map, "alpha", &value))
        return false;
    if (axhs_size(&map) != 2)
        return false;
    if (!axhs_lookup(&map, "gamma", &value) || value != 3)
        return false;
    return true;
}

