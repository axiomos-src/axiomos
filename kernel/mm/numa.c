/* AxiomOS - NUMA topology, memory policy and zonelists.
 *
 * Each node owns a set of zones (DMA, DMA32, normal, highmem) plus a distance
 * table to its peers.  The allocator consults the distance table and the task
 * policy to decide which zone list to walk: local only, local then remote, or
 * interleaved round robin across nodes.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXNU_MAX_NODES       4
#define AXNU_MAX_ZONES       4
#define AXNU_MAX_TASKS       8

#define AXNU_ZONE_DMA        0
#define AXNU_ZONE_DMA32      1
#define AXNU_ZONE_NORMAL     2
#define AXNU_ZONE_HIGHMEM    3

#define AXNU_POLICY_LOCAL    0
#define AXNU_POLICY_BIND     1
#define AXNU_POLICY_INTERLEAVE 2
#define AXNU_POLICY_PREFERRED 3

#define AXNU_DISTANCE_LOCAL  10
#define AXNU_DISTANCE_MAX    254
#define AXNU_WATERMARK_MIN   64
#define AXNU_WATERMARK_LOW   96
#define AXNU_WATERMARK_HIGH  128

typedef struct axnu_zone
{
    u64  start_pfn;
    u32  present_pages;
    u32  free_pages;
    u32  reserved_pages;
    u32  allocations;
    u32  frees;
    u8   kind;
    bool_t populated;
} axnu_zone_t;

typedef struct axnu_node
{
    axnu_zone_t zones[AXNU_MAX_ZONES];
    u32  id;
    u32  cpu_mask;
    u32  zone_count;
    u32  allocations;
    u32  remote_allocations;
    u8   distance[AXNU_MAX_NODES];
    bool_t online;
} axnu_node_t;

typedef struct axnu_task_policy
{
    u32  task;
    u32  policy;
    u32  preferred_node;
    u32  bind_mask;
    u32  interleave_cursor;
    bool_t used;
} axnu_task_policy_t;

typedef struct axnu_state
{
    axnu_node_t nodes[AXNU_MAX_NODES];
    axnu_task_policy_t tasks[AXNU_MAX_TASKS];
    u32  node_count;
    u32  task_count;
    u32  local_hits;
    u32  remote_hits;
    u32  allocation_failures;
    u32  watermark_events;
    u32  fallback_events;
    bool_t ready;
} axnu_state_t;

static axnu_state_t axnu_state;

static void axnu_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axnu_zone_t *axnu_zone(u32 node, u8 kind)
{
    axnu_node_t *entry;
    u32 i;
    if (node >= axnu_state.node_count)
        return NULL_PTR;
    entry = &axnu_state.nodes[node];
    for (i = 0; i < entry->zone_count; i++)
    {
        if (entry->zones[i].kind == kind && entry->zones[i].populated)
            return &entry->zones[i];
    }
    return NULL_PTR;
}

static axnu_task_policy_t *axnu_policy(u32 task)
{
    u32 i;
    for (i = 0; i < axnu_state.task_count; i++)
    {
        if (axnu_state.tasks[i].used && axnu_state.tasks[i].task == task)
            return &axnu_state.tasks[i];
    }
    return NULL_PTR;
}

static bool_t axnu_zone_ok(const axnu_zone_t *zone)
{
    if (zone == NULL_PTR)
        return false;
    if (zone->free_pages < AXNU_WATERMARK_MIN)
    {
        return false;
    }
    return true;
}

/* -------------------------------------------------------------- public API */

void axnu_init(void)
{
    axnu_zero(&axnu_state, (u32)sizeof(axnu_state));
    axnu_state.ready = true;
}

u32 axnu_add_node(u32 id, u32 cpu_mask)
{
    axnu_node_t *node;
    u32 i;
    if (axnu_state.node_count >= AXNU_MAX_NODES)
        return 0xFFFFFFFFu;
    node = &axnu_state.nodes[axnu_state.node_count];
    axnu_zero(node, (u32)sizeof(*node));
    node->id = id;
    node->cpu_mask = cpu_mask;
    node->online = true;
    for (i = 0; i < AXNU_MAX_NODES; i++)
        node->distance[i] = (u8)(i == axnu_state.node_count ? AXNU_DISTANCE_LOCAL : 20 + i * 10);
    axnu_state.node_count++;
    return axnu_state.node_count - 1;
}

bool_t axnu_add_zone(u32 node, u8 kind, u64 start_pfn, u32 pages)
{
    axnu_node_t *entry;
    axnu_zone_t *zone;
    if (node >= axnu_state.node_count || pages == 0)
        return false;
    entry = &axnu_state.nodes[node];
    if (entry->zone_count >= AXNU_MAX_ZONES)
        return false;
    zone = &entry->zones[entry->zone_count];
    zone->kind = kind;
    zone->start_pfn = start_pfn;
    zone->present_pages = pages;
    zone->free_pages = pages;
    zone->populated = true;
    entry->zone_count++;
    return true;
}

bool_t axnu_set_distance(u32 from, u32 to, u8 distance)
{
    if (from >= axnu_state.node_count || to >= axnu_state.node_count)
        return false;
    if (distance < AXNU_DISTANCE_LOCAL || distance > AXNU_DISTANCE_MAX)
        return false;
    axnu_state.nodes[from].distance[to] = distance;
    return true;
}

u8 axnu_distance(u32 from, u32 to)
{
    if (from >= axnu_state.node_count || to >= axnu_state.node_count)
        return AXNU_DISTANCE_MAX;
    return axnu_state.nodes[from].distance[to];
}

bool_t axnu_set_policy(u32 task, u32 policy, u32 preferred_node, u32 bind_mask)
{
    axnu_task_policy_t *entry = axnu_policy(task);
    if (policy > AXNU_POLICY_PREFERRED)
        return false;
    if (preferred_node >= axnu_state.node_count)
        return false;
    if (entry == NULL_PTR)
    {
        if (axnu_state.task_count >= AXNU_MAX_TASKS)
            return false;
        entry = &axnu_state.tasks[axnu_state.task_count];
        axnu_zero(entry, (u32)sizeof(*entry));
        entry->task = task;
        entry->used = true;
        axnu_state.task_count++;
    }
    entry->policy = policy;
    entry->preferred_node = preferred_node;
    entry->bind_mask = bind_mask;
    return true;
}

u64 axnu_allocate(u32 task, u8 preferred_kind)
{
    axnu_task_policy_t *policy = axnu_policy(task);
    u32 node;
    u32 attempt;
    if (!axnu_state.ready || axnu_state.node_count == 0)
        return 0;
    node = policy == NULL_PTR ? 0 : policy->preferred_node;
    if (policy != NULL_PTR && policy->policy == AXNU_POLICY_INTERLEAVE)
    {
        node = policy->interleave_cursor % axnu_state.node_count;
        policy->interleave_cursor = (policy->interleave_cursor + 1) % axnu_state.node_count;
    }
    for (attempt = 0; attempt < axnu_state.node_count; attempt++)
    {
        u32 index = node;
        axnu_zone_t *zone;
        if (policy != NULL_PTR && policy->policy == AXNU_POLICY_BIND &&
            (policy->bind_mask & (1u << index)) == 0)
        {
            node = (node + 1) % axnu_state.node_count;
            continue;
        }
        zone = axnu_zone(index, preferred_kind);
        if (zone == NULL_PTR)
        {
            u8 kind;
            for (kind = 0; kind < AXNU_MAX_ZONES; kind++)
            {
                zone = axnu_zone(index, kind);
                if (zone != NULL_PTR)
                    break;
            }
        }
        if (!axnu_zone_ok(zone))
        {
            axnu_state.watermark_events++;
            node = (node + 1) % axnu_state.node_count;
            continue;
        }
        zone->free_pages--;
        zone->allocations++;
        axnu_state.nodes[index].allocations++;
        if (index == 0)
            axnu_state.local_hits++;
        else
        {
            axnu_state.remote_hits++;
            axnu_state.nodes[index].remote_allocations++;
        }
        return zone->start_pfn + (u64)(zone->present_pages - zone->free_pages);
    }
    axnu_state.allocation_failures++;
    axnu_state.fallback_events++;
    return 0;
}

bool_t axnu_free(u32 node, u8 kind)
{
    axnu_zone_t *zone = axnu_zone(node, kind);
    if (zone == NULL_PTR)
        return false;
    if (zone->free_pages >= zone->present_pages)
        return false;
    zone->free_pages++;
    zone->frees++;
    return true;
}

u32 axnu_free_pages(u32 node)
{
    u32 i;
    u32 total = 0;
    if (node >= axnu_state.node_count)
        return 0;
    for (i = 0; i < axnu_state.nodes[node].zone_count; i++)
        total += axnu_state.nodes[node].zones[i].free_pages;
    return total;
}

u32 axnu_total_pages(void)
{
    u32 i;
    u32 total = 0;
    for (i = 0; i < axnu_state.node_count; i++)
        total += axnu_free_pages(i);
    return total;
}

u32 axnu_node_for_cpu(u32 cpu)
{
    u32 i;
    for (i = 0; i < axnu_state.node_count; i++)
    {
        if (axnu_state.nodes[i].cpu_mask & (1u << cpu))
            return i;
    }
    return 0xFFFFFFFFu;
}

u32 axnu_nearest(u32 from)
{
    u32 i;
    u32 best = 0xFFFFFFFFu;
    u8 distance = AXNU_DISTANCE_MAX;
    for (i = 0; i < axnu_state.node_count; i++)
    {
        if (i == from)
            continue;
        if (axnu_distance(from, i) < distance)
        {
            distance = axnu_distance(from, i);
            best = i;
        }
    }
    return best;
}

bool_t axnu_offline(u32 node)
{
    if (node >= axnu_state.node_count)
        return false;
    axnu_state.nodes[node].online = false;
    return true;
}

bool_t axnu_online(u32 node)
{
    if (node >= axnu_state.node_count)
        return false;
    axnu_state.nodes[node].online = true;
    return true;
}

u32 axnu_bind_mask(void)
{
    u32 mask = 0;
    u32 i;
    for (i = 0; i < axnu_state.node_count; i++)
    {
        if (axnu_state.nodes[i].online)
            mask |= (1u << i);
    }
    return mask;
}

void axnu_report(void)
{
    u32 i;
    printk("axnu: nodes=%u tasks=%u local=%u remote=%u fail=%u wmark=%u\n",
           (unsigned)axnu_state.node_count, (unsigned)axnu_state.task_count,
           (unsigned)axnu_state.local_hits, (unsigned)axnu_state.remote_hits,
           (unsigned)axnu_state.allocation_failures, (unsigned)axnu_state.watermark_events);
    for (i = 0; i < axnu_state.node_count; i++)
    {
        const axnu_node_t *node = &axnu_state.nodes[i];
        printk("  node %u cpus=0x%x zones=%u free=%u alloc=%u online=%u\n",
               (unsigned)node->id, (unsigned)node->cpu_mask,
               (unsigned)node->zone_count, (unsigned)axnu_free_pages(i),
               (unsigned)node->allocations, (unsigned)node->online);
    }
}

bool_t axnu_selftest(void)
{
    axnu_init();
    if (axnu_add_node(0, 0x0F) != 0)
        return false;
    if (axnu_add_node(1, 0xF0) != 1)
        return false;
    if (!axnu_add_zone(0, AXNU_ZONE_NORMAL, 0x1000, 200))
        return false;
    if (!axnu_add_zone(1, AXNU_ZONE_NORMAL, 0x2000, 200))
        return false;
    if (!axnu_set_distance(0, 1, 20))
        return false;
    if (axnu_set_distance(0, 1, 5))
        return false;
    if (axnu_distance(0, 1) != 20)
        return false;
    if (!axnu_set_policy(1, AXNU_POLICY_LOCAL, 0, 0x3))
        return false;
    if (axnu_node_for_cpu(5) != 1)
        return false;
    if (axnu_nearest(0) != 1)
        return false;
    if (axnu_allocate(1, AXNU_ZONE_NORMAL) == 0)
        return false;
    if (axnu_allocate(1, AXNU_ZONE_NORMAL) == 0)
        return false;
    if (axnu_total_pages() != 398)
        return false;
    if (!axnu_set_policy(1, AXNU_POLICY_INTERLEAVE, 0, 0x3))
        return false;
    if (axnu_allocate(1, AXNU_ZONE_NORMAL) == 0)
        return false;
    if (!axnu_set_policy(2, AXNU_POLICY_BIND, 1, 0x2))
        return false;
    if (axnu_allocate(2, AXNU_ZONE_NORMAL) == 0)
        return false;
    if (axnu_state.remote_hits == 0)
        return false;
    if (!axnu_offline(1))
        return false;
    if (axnu_bind_mask() != 0x1)
        return false;
    if (!axnu_online(1))
        return false;
    if (!axnu_free(0, AXNU_ZONE_NORMAL))
        return false;
    return true;
}
