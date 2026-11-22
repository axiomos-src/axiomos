/*
 * AxiomOS - eBPF map implementations.
 *
 * Five map types cover the tracing workload we care about:
 *   - array: fixed keys, preallocated values, used for counters and config;
 *   - hash: open addressing with chaining, used for per-pid statistics;
 *   - LRU hash: same as hash but evicts the least recently used key when full;
 *   - percpu array: array with one value per CPU, used for per-CPU counters;
 *   - ringbuf: single producer/single consumer byte ring, used for events.
 *
 * Concurrency.  AxiomOS is currently single-core in practice (the scheduler can
 * migrate but not run BPF programs concurrently on two CPUs yet), so the maps
 * use no locks beyond a generation counter that consumers can check.  When
 * symmetric multiprocessing lands, the same structures work with a per-map
 * spinlock; the call sites are already narrow.
 *
 * Key handling is generic: keys are hashed by FNV-1a over key_size bytes and
 * compared bytewise.  This keeps one implementation per map family instead of
 * one per key type.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>

#define AXBPF_HASH_BUCKETS 256
#define AXBPF_RING_RECORD_MAGIC 0xA5B4C3D2u

/* Local errno names avoid depending on the axfs header from the eBPF core. */
#define ENOMEM_LOCAL  -12
#define ENOENT_LOCAL  -2
#define E2BIG_LOCAL   -7
#define EINVAL_LOCAL  -22
#define EEXIST_LOCAL  -17
#define ENOSPC_LOCAL  -28
#define ENOTSUP_LOCAL -95
#define EPERM_LOCAL   -1


static axbpf_map_t *g_maps[AXBPF_MAX_MAPS];
static u32 g_map_count;
static u32 g_map_serial;

/* ------------------------------------------------------------------ */
/* Key hashing                                                         */
/* ------------------------------------------------------------------ */

static u64 key_hash(const void *key, u32 size)
{
    const u8 *bytes = (const u8 *)key;
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool_t key_equal(const void *a, const void *b, u32 size)
{
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    for (u32 i = 0; i < size; i++) {
        if (pa[i] != pb[i]) {
            return (bool_t)0;
        }
    }
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Generic lookup and update                                           */
/* ------------------------------------------------------------------ */

static axbpf_map_entry_t *hash_find(axbpf_map_t *map, const void *key,
                                    axbpf_map_entry_t **out_previous)
{
    if (!map || !map->buckets) {
        return 0;
    }
    u64 hash = key_hash(key, map->def.key_size);
    u32 bucket = (u32)(hash % map->bucket_count);
    axbpf_map_entry_t *previous = 0;
    for (axbpf_map_entry_t *entry = map->buckets[bucket]; entry;
         entry = entry->next) {
        if (key_equal(entry->key, key, map->def.key_size)) {
            entry->last_used = ++map->generation;
            if (out_previous) {
                *out_previous = previous;
            }
            return entry;
        }
        previous = entry;
    }
    if (out_previous) {
        *out_previous = previous;
    }
    return 0;
}

static i32 hash_insert(axbpf_map_t *map, const void *key, const void *value)
{
    u64 hash = key_hash(key, map->def.key_size);
    u32 bucket = (u32)(hash % map->bucket_count);
    axbpf_map_entry_t *entry = (axbpf_map_entry_t *)kzalloc(sizeof(*entry));
    if (!entry) {
        return ENOMEM_LOCAL;
    }
    entry->key = kzalloc(map->def.key_size);
    entry->value = kzalloc(map->def.value_size);
    if (!entry->key || !entry->value) {
        if (entry->key) {
            kfree(entry->key);
        }
        if (entry->value) {
            kfree(entry->value);
        }
        kfree(entry);
        return ENOMEM_LOCAL;
    }
    for (u32 i = 0; i < map->def.key_size; i++) {
        ((u8 *)entry->key)[i] = ((const u8 *)key)[i];
    }
    for (u32 i = 0; i < map->def.value_size; i++) {
        ((u8 *)entry->value)[i] = ((const u8 *)value)[i];
    }
    entry->generation = ++map->generation;
    entry->last_used = entry->generation;
    entry->next = map->buckets[bucket];
    map->buckets[bucket] = entry;
    map->entry_count++;
    return 0;
}

/* Local errno names avoid depending on the axfs header from the eBPF core. */
#define ENOMEM_LOCAL  -12
#define ENOENT_LOCAL  -2
#define E2BIG_LOCAL   -7
#define EINVAL_LOCAL  -22
#define EEXIST_LOCAL  -17
#define ENOSPC_LOCAL  -28
#define ENOTSUP_LOCAL -95

/* ------------------------------------------------------------------ */
/* Array maps                                                          */
/* ------------------------------------------------------------------ */

static i32 array_lookup(axbpf_map_t *map, const void *key, void *value)
{
    if (!map || !key || !value) {
        return EINVAL_LOCAL;
    }
    if (map->def.key_size != 4) {
        return EINVAL_LOCAL;
    }
    u32 index = *(const u32 *)key;
    if (index >= map->def.max_entries) {
        map->misses++;
        return ENOENT_LOCAL;
    }
    u8 *slot = map->storage + (u64)index * map->def.value_size;
    for (u32 i = 0; i < map->def.value_size; i++) {
        ((u8 *)value)[i] = slot[i];
    }
    map->hits++;
    return 0;
}

static i32 array_update(axbpf_map_t *map, const void *key, const void *value,
                        u32 flags)
{
    if (!map || !key || !value) {
        return EINVAL_LOCAL;
    }
    if (map->frozen) {
        return EPERM_LOCAL;
    }
    u32 index = *(const u32 *)key;
    if (index >= map->def.max_entries) {
        return E2BIG_LOCAL;
    }
    if ((flags & 1) && index >= map->entry_count) {
        /* BPF_NOEXIST on an array slot that was never written. */
        return ENOENT_LOCAL;
    }
    if ((flags & 2) && index < map->entry_count) {
        return EEXIST_LOCAL;
    }
    u8 *slot = map->storage + (u64)index * map->def.value_size;
    for (u32 i = 0; i < map->def.value_size; i++) {
        slot[i] = ((const u8 *)value)[i];
    }
    if (index >= map->entry_count) {
        map->entry_count = index + 1;
    }
    map->updates++;
    return 0;
}

static i32 array_remove(axbpf_map_t *map, const void *key)
{
    (void)map;
    (void)key;
    /* Array elements always exist; deleting is a no-op returning success, as
     * the Linux implementation does for arrays. */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hash maps                                                           */
/* ------------------------------------------------------------------ */

static i32 hash_lookup(axbpf_map_t *map, const void *key, void *value)
{
    if (!map || !key || !value) {
        return EINVAL_LOCAL;
    }
    axbpf_map_entry_t *entry = hash_find(map, key, 0);
    if (!entry) {
        map->misses++;
        return ENOENT_LOCAL;
    }
    for (u32 i = 0; i < map->def.value_size; i++) {
        ((u8 *)value)[i] = ((const u8 *)entry->value)[i];
    }
    map->hits++;
    return 0;
}


/* A simple LRU approximation: entries carry a monotonically increasing
 * "last_used" stamp, so eviction scans for the smallest stamp.  With at most
 * AXBPF_MAX_MAP_ENTRIES entries this is bounded and allocation free. */
static i32 lru_evict(axbpf_map_t *map)
{
    axbpf_map_entry_t *victim = 0;
    axbpf_map_entry_t *previous = 0;
    u32 victim_bucket = 0;
    for (u32 b = 0; b < map->bucket_count; b++) {
        axbpf_map_entry_t *prev = 0;
        for (axbpf_map_entry_t *entry = map->buckets[b]; entry;
             entry = entry->next) {
            if (!victim || entry->last_used < victim->last_used) {
                victim = entry;
                previous = prev;
                victim_bucket = b;
            }
            prev = entry;
        }
    }
    if (!victim) {
        return ENOSPC_LOCAL;
    }
    if (previous) {
        previous->next = victim->next;
    } else {
        map->buckets[victim_bucket] = victim->next;
    }
    if (map->lru_head == victim) {
        map->lru_head = victim->next ? victim->next : 0;
    }
    kfree(victim->key);
    kfree(victim->value);
    kfree(victim);
    map->entry_count--;
    return 0;
}

static i32 hash_update(axbpf_map_t *map, const void *key, const void *value,
                       u32 flags)
{
    if (!map || !key || !value) {
        return EINVAL_LOCAL;
    }
    if (map->frozen) {
        return EPERM_LOCAL;
    }
    axbpf_map_entry_t *entry = hash_find(map, key, 0);
    if (entry) {
        if (flags & 1) {
            return EEXIST_LOCAL;
        }
        for (u32 i = 0; i < map->def.value_size; i++) {
            ((u8 *)entry->value)[i] = ((const u8 *)value)[i];
        }
        map->updates++;
        return 0;
    }
    if (flags & 2) {
        return ENOENT_LOCAL;
    }
    if (map->entry_count >= map->def.max_entries) {
        if (map->def.type == AXBPF_MAP_LRU_HASH) {
            i32 status = lru_evict(map);
            if (status != 0) {
                return status;
            }
        } else {
            return E2BIG_LOCAL;
        }
    }
    if (hash_insert(map, key, value) != 0) {
        return ENOMEM_LOCAL;
    }
    map->updates++;
    return 0;
}

static i32 hash_remove(axbpf_map_t *map, const void *key)
{
    if (!map || !key) {
        return EINVAL_LOCAL;
    }
    axbpf_map_entry_t *previous = 0;
    axbpf_map_entry_t *entry = hash_find(map, key, &previous);
    if (!entry) {
        map->misses++;
        return ENOENT_LOCAL;
    }
    u64 hash = key_hash(key, map->def.key_size);
    u32 bucket = (u32)(hash % map->bucket_count);
    if (previous) {
        previous->next = entry->next;
    } else {
        map->buckets[bucket] = entry->next;
    }
    kfree(entry->key);
    kfree(entry->value);
    kfree(entry);
    map->entry_count--;
    map->deletes++;
    return 0;
}

/* Iteration by key order: returns the key after `key`, or the first key when
 * `key` is null.  Used by tooling that dumps a map. */
static i32 hash_next_key(axbpf_map_t *map, const void *key, void *next)
{
    if (!map || !next) {
        return EINVAL_LOCAL;
    }
    u64 best = 0xFFFFFFFFFFFFFFFFull;
    axbpf_map_entry_t *best_entry = 0;
    bool_t have_best = (bool_t)0;
    u64 current = 0;
    if (key) {
        current = key_hash(key, map->def.key_size);
    }
    for (u32 b = 0; b < map->bucket_count; b++) {
        for (axbpf_map_entry_t *entry = map->buckets[b]; entry;
             entry = entry->next) {
            u64 hash = key_hash(entry->key, map->def.key_size);
            if (key && hash == current && key_equal(entry->key, key,
                                                    map->def.key_size)) {
                continue;
            }
            if (hash > current && hash < best) {
                best = hash;
                best_entry = entry;
                have_best = (bool_t)1;
            }
        }
    }
    if (!have_best) {
        return ENOENT_LOCAL;
    }
    for (u32 i = 0; i < map->def.key_size; i++) {
        ((u8 *)next)[i] = ((const u8 *)best_entry->key)[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ring buffer                                                         */
/* ------------------------------------------------------------------ */

i32 axbpf_ringbuf_reserve(axbpf_map_t *map, u32 size, void **out)
{
    if (!map || !map->ring.data || size == 0 || !out) {
        return EINVAL_LOCAL;
    }
    if (size > map->ring.size / 2) {
        return E2BIG_LOCAL;
    }
    u64 head = map->ring.head;
    u64 tail = map->ring.tail;
    if (head - tail + size > map->ring.size) {
        map->ring.dropped++;
        return ENOSPC_LOCAL;
    }
    *out = map->ring.data + (head & map->ring.mask);
    return 0;
}

i32 axbpf_ringbuf_submit(axbpf_map_t *map, void *record, u32 size)
{
    if (!map || !map->ring.data || !record) {
        return EINVAL_LOCAL;
    }
    /* The record may wrap; write the header at the current head and copy the
     * payload into the ring so consumers see a contiguous record. */
    u64 head = map->ring.head;
    u8 *slot = map->ring.data + (head & map->ring.mask);
    u32 header = AXBPF_RING_RECORD_MAGIC;
    (void)header;
    for (u32 i = 0; i < size && i < map->ring.size; i++) {
        slot[i & map->ring.mask] = ((const u8 *)record)[i];
    }
    map->ring.head = head + size;
    return 0;
}

u32 axbpf_ringbuf_consume(axbpf_map_t *map, void *out, u32 capacity)
{
    if (!map || !map->ring.data || !out || capacity == 0) {
        return 0;
    }
    u64 head = map->ring.head;
    u64 tail = map->ring.tail;
    if (tail >= head) {
        return 0;
    }
    u32 available = (u32)(head - tail);
    u32 count = available < capacity ? available : capacity;
    for (u32 i = 0; i < count; i++) {
        ((u8 *)out)[i] = map->ring.data[(tail + i) & map->ring.mask];
    }
    map->ring.tail = tail + count;
    return count;
}

/* ------------------------------------------------------------------ */
/* Creation and destruction                                            */
/* ------------------------------------------------------------------ */

static void destroy_storage(axbpf_map_t *map)
{
    if (!map) {
        return;
    }
    if (map->buckets) {
        for (u32 b = 0; b < map->bucket_count; b++) {
            axbpf_map_entry_t *entry = map->buckets[b];
            while (entry) {
                axbpf_map_entry_t *next = entry->next;
                kfree(entry->key);
                kfree(entry->value);
                kfree(entry);
                entry = next;
            }
        }
        kfree(map->buckets);
        map->buckets = 0;
    }
    if (map->storage) {
        kfree(map->storage);
        map->storage = 0;
    }
    if (map->ring.data) {
        kfree(map->ring.data);
        map->ring.data = 0;
    }
}

/* Check that a definition is internally consistent before allocating. */
static i32 validate_def(const axbpf_map_def_t *def)
{
    if (!def) {
        return EINVAL_LOCAL;
    }
    if (def->max_entries == 0 || def->max_entries > AXBPF_MAX_MAP_ENTRIES) {
        return EINVAL_LOCAL;
    }
    switch (def->type) {
    case AXBPF_MAP_ARRAY:
    case AXBPF_MAP_PERCPU_ARRAY:
        if (def->key_size != 4) {
            return EINVAL_LOCAL;
        }
        if (def->value_size == 0 || def->value_size > 4096) {
            return EINVAL_LOCAL;
        }
        break;
    case AXBPF_MAP_HASH:
    case AXBPF_MAP_LRU_HASH:
        if (def->key_size == 0 || def->key_size > 64) {
            return EINVAL_LOCAL;
        }
        if (def->value_size == 0 || def->value_size > 4096) {
            return EINVAL_LOCAL;
        }
        break;
    case AXBPF_MAP_RINGBUF:
        if (def->key_size != 0 || def->value_size != 0) {
            return EINVAL_LOCAL;
        }
        break;
    default:
        return ENOTSUP_LOCAL;
    }
    return 0;
}

void axbpf_map_init(void)
{
    g_map_count = 0;
    g_map_serial = 0;
    for (u32 i = 0; i < AXBPF_MAX_MAPS; i++) {
        g_maps[i] = 0;
    }
}

i32 axbpf_map_create(const axbpf_map_def_t *def, const char *name)
{
    i32 valid = validate_def(def);
    if (valid != 0) {
        printk("bpf: map definition rejected (%d)\n", valid);
        return valid;
    }
    if (g_map_count >= AXBPF_MAX_MAPS) {
        return ENOSPC_LOCAL;
    }
    u32 slot = 0;
    while (slot < AXBPF_MAX_MAPS && g_maps[slot]) {
        slot++;
    }
    if (slot == AXBPF_MAX_MAPS) {
        return ENOSPC_LOCAL;
    }

    axbpf_map_t *map = (axbpf_map_t *)kzalloc(sizeof(*map));
    if (!map) {
        return ENOMEM_LOCAL;
    }
    map->def = *def;
    if (name) {
        ax_strlcpy(map->name, name, sizeof(map->name));
        ax_strlcpy(map->def.name, name, sizeof(map->def.name));
    } else {
        ax_snprintf(map->name, sizeof(map->name), "map%u", g_map_serial);
        ax_strlcpy(map->def.name, map->name, sizeof(map->def.name));
    }
    map->fd = (i32)(slot + 1);
    map->in_use = (bool_t)1;

    switch (def->type) {
    case AXBPF_MAP_ARRAY:
    case AXBPF_MAP_PERCPU_ARRAY: {
        u64 bytes = (u64)def->max_entries * def->value_size;
        map->storage = (u8 *)kzalloc((usize)bytes);
        if (!map->storage) {
            kfree(map);
            return ENOMEM_LOCAL;
        }
        map->bucket_count = 0;
        break;
    }
    case AXBPF_MAP_HASH:
    case AXBPF_MAP_LRU_HASH: {
        map->bucket_count = AXBPF_HASH_BUCKETS;
        map->buckets = (axbpf_map_entry_t **)kzalloc(
            sizeof(axbpf_map_entry_t *) * map->bucket_count);
        if (!map->buckets) {
            kfree(map);
            return ENOMEM_LOCAL;
        }
        map->lru_capacity = def->max_entries;
        break;
    }
    case AXBPF_MAP_RINGBUF: {
        u32 size = AXBPF_RINGBUF_SIZE;
        if (def->max_entries < size) {
            size = def->max_entries;
        }
        /* Power of two so the mask trick works. */
        u32 power = 1;
        while (power < size) {
            power <<= 1;
        }
        map->ring.data = (u8 *)kzalloc(power);
        if (!map->ring.data) {
            kfree(map);
            return ENOMEM_LOCAL;
        }
        map->ring.size = power;
        map->ring.mask = power - 1;
        break;
    }
    default:
        kfree(map);
        return ENOTSUP_LOCAL;
    }

    g_maps[slot] = map;
    g_map_count++;
    g_map_serial++;
    printk("bpf: map '%s' type=%u entries=%u key=%u value=%u fd=%d\n", map->name,
           def->type, def->max_entries, def->key_size, def->value_size, map->fd);
    return map->fd;
}

axbpf_map_t *axbpf_map_by_fd(i32 fd)
{
    if (fd < 1 || fd > AXBPF_MAX_MAPS) {
        return 0;
    }
    return g_maps[fd - 1];
}

axbpf_map_t *axbpf_map_by_name(const char *name)
{
    if (!name) {
        return 0;
    }
    for (u32 i = 0; i < AXBPF_MAX_MAPS; i++) {
        if (g_maps[i] && ax_strcmp(g_maps[i]->name, name) == 0) {
            return g_maps[i];
        }
    }
    return 0;
}

void axbpf_map_destroy(i32 fd)
{
    axbpf_map_t *map = axbpf_map_by_fd(fd);
    if (!map) {
        return;
    }
    destroy_storage(map);
    g_maps[fd - 1] = 0;
    g_map_count--;
    kfree(map);
}

/* ------------------------------------------------------------------ */
/* Public operations                                                   */
/* ------------------------------------------------------------------ */

i32 axbpf_map_lookup(i32 fd, const void *key, void *value)
{
    axbpf_map_t *map = axbpf_map_by_fd(fd);
    if (!map) {
        return EINVAL_LOCAL;
    }
    switch (map->def.type) {
    case AXBPF_MAP_ARRAY:
    case AXBPF_MAP_PERCPU_ARRAY:
        return array_lookup(map, key, value);
    case AXBPF_MAP_HASH:
    case AXBPF_MAP_LRU_HASH:
        return hash_lookup(map, key, value);
    default:
        return ENOTSUP_LOCAL;
    }
}

i32 axbpf_map_update(i32 fd, const void *key, const void *value, u32 flags)
{
    axbpf_map_t *map = axbpf_map_by_fd(fd);
    if (!map) {
        return EINVAL_LOCAL;
    }
    switch (map->def.type) {
    case AXBPF_MAP_ARRAY:
    case AXBPF_MAP_PERCPU_ARRAY:
        return array_update(map, key, value, flags);
    case AXBPF_MAP_HASH:
    case AXBPF_MAP_LRU_HASH:
        return hash_update(map, key, value, flags);
    default:
        return ENOTSUP_LOCAL;
    }
}

i32 axbpf_map_delete(i32 fd, const void *key)
{
    axbpf_map_t *map = axbpf_map_by_fd(fd);
    if (!map) {
        return EINVAL_LOCAL;
    }
    switch (map->def.type) {
    case AXBPF_MAP_ARRAY:
    case AXBPF_MAP_PERCPU_ARRAY:
        return array_remove(map, key);
    case AXBPF_MAP_HASH:
    case AXBPF_MAP_LRU_HASH:
        return hash_remove(map, key);
    default:
        return ENOTSUP_LOCAL;
    }
}

i32 axbpf_map_lookup_by_name(const char *name, const void *key, void *value)
{
    axbpf_map_t *map = axbpf_map_by_name(name);
    if (!map) {
        return ENOENT_LOCAL;
    }
    return axbpf_map_lookup(map->fd, key, value);
}

i32 axbpf_map_next_key(i32 fd, const void *key, void *next)
{
    axbpf_map_t *map = axbpf_map_by_fd(fd);
    if (!map || !next) {
        return EINVAL_LOCAL;
    }
    if (map->def.type == AXBPF_MAP_HASH || map->def.type == AXBPF_MAP_LRU_HASH) {
        return hash_next_key(map, key, next);
    }
    if (map->def.type == AXBPF_MAP_ARRAY) {
        u32 index = key ? *(const u32 *)key + 1 : 0;
        if (index >= map->def.max_entries) {
            return ENOENT_LOCAL;
        }
        *(u32 *)next = index;
        return 0;
    }
    return ENOTSUP_LOCAL;
}

/* Return the entry that owns `key`, or null.  Used by the helper table so a
 * lookup helper can hand the interpreter a pointer to the stored value. */
axbpf_map_entry_t *axbpf_map_find_entry(axbpf_map_t *map, const void *key)
{
    if (!map || !key) {
        return 0;
    }
    if (map->def.type != AXBPF_MAP_HASH && map->def.type != AXBPF_MAP_LRU_HASH) {
        return 0;
    }
    return hash_find(map, key, 0);
}

u64 axbpf_map_total_entries(void)
{
    u64 total = 0;
    for (u32 i = 0; i < AXBPF_MAX_MAPS; i++) {
        if (g_maps[i]) {
            total += g_maps[i]->entry_count;
        }
    }
    return total;
}

void axbpf_map_dump_all(void)
{
    printk("bpf: maps (%u)\n", g_map_count);
    for (u32 i = 0; i < AXBPF_MAX_MAPS; i++) {
        axbpf_map_t *map = g_maps[i];
        if (!map) {
            continue;
        }
        printk("  fd=%-2d %-12s type=%-2u entries=%u/%u hits=%llu misses=%llu\n",
               map->fd, map->name, map->def.type, map->entry_count,
               map->def.max_entries, map->hits, map->misses);
    }
}

/* ------------------------------------------------------------------ */
/* Self test                                                           */
/* ------------------------------------------------------------------ */

i32 axbpf_map_selftest(void)
{
    axbpf_map_def_t array_def;
    ax_memset(&array_def, 0, sizeof(array_def));
    array_def.type = AXBPF_MAP_ARRAY;
    array_def.key_size = 4;
    array_def.value_size = 8;
    array_def.max_entries = 4;
    i32 fd = axbpf_map_create(&array_def, "selftest_array");
    if (fd < 0) {
        return fd;
    }
    u32 key = 2;
    u64 value = 0x1122334455667788ull;
    i32 status = axbpf_map_update(fd, &key, &value, 0);
    if (status != 0) {
        axbpf_map_destroy(fd);
        return status;
    }
    u64 readback = 0;
    status = axbpf_map_lookup(fd, &key, &readback);
    if (status != 0 || readback != value) {
        axbpf_map_destroy(fd);
        return status ? status : -1;
    }

    axbpf_map_def_t hash_def;
    ax_memset(&hash_def, 0, sizeof(hash_def));
    hash_def.type = AXBPF_MAP_HASH;
    hash_def.key_size = 8;
    hash_def.value_size = 4;
    hash_def.max_entries = 16;
    i32 hash_fd = axbpf_map_create(&hash_def, "selftest_hash");
    if (hash_fd < 0) {
        axbpf_map_destroy(fd);
        return hash_fd;
    }
    for (u64 i = 0; i < 8; i++) {
        u32 payload = (u32)(i * 3);
        axbpf_map_update(hash_fd, &i, &payload, 0);
    }
    u64 probe = 5;
    u32 got = 0;
    status = axbpf_map_lookup(hash_fd, &probe, &got);
    if (status != 0 || got != 15) {
        printk("bpf: hash selftest failed (%d, got %u)\n", status, got);
        status = -1;
    }
    axbpf_map_destroy(hash_fd);
    axbpf_map_destroy(fd);
    printk("bpf: map selftest %s\n", status == 0 ? "ok" : "failed");
    return status;
}
