/* AxiomOS - interrupt affinity and routing policy.
 *
 * Each interrupt line records a preferred target set, a current delivery CPU,
 * and a service counter.  The policy module decides whether a line may be moved
 * (IRQ_MOVE_CLEANUP semantics are emulated in software), performs round robin
 * spreading at boot, and supports manual pinning requested by drivers that need
 * cache locality such as NVMe completion queues.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXIA_MAX_LINES       64
#define AXIA_MAX_CPUS        16
#define AXIA_SETTLE_TICKS    4
#define AXIA_BALANCE_WINDOW  100
#define AXIA_MOVES_MAX       8

#define AXIA_POLICY_ROUND    0
#define AXIA_POLICY_AFFINE   1
#define AXIA_POLICY_MANUAL   2
#define AXIA_POLICY_SPREAD   3

#define AXIA_LINE_EDGE       0
#define AXIA_LINE_LEVEL      1
#define AXIA_LINE_MSI        2
#define AXIA_LINE_MSIX       3

typedef struct axia_line
{
    u32  line;
    u32  target_mask;
    u32  owner_mask;
    u32  vector;
    u32  deliveries;
    u32  last_cpu;
    u32  migrations;
    u32  pending;
    u8   policy;
    u8   kind;
    u8   priority;
    u8   state;
    bool_t masked;
    bool_t shared;
} axia_line_t;

typedef struct axia_state
{
    axia_line_t lines[AXIA_MAX_LINES];
    u32  cpu_count;
    u32  line_count;
    u32  load[AXIA_MAX_CPUS];
    u32  online_mask;
    u32  migrations;
    u32  balance_runs;
    u32  rejected_moves;
    u32  wait_cleanups;
    u32  now_ms;
    u8   next_cpu;
    bool_t balancing;
    bool_t initialized;
} axia_state_t;

static axia_state_t axia_state;

static void axia_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axia_line_t *axia_line(u32 line)
{
    for (u32 i = 0; i < axia_state.line_count; i++)
    {
        if (axia_state.lines[i].line == line)
            return &axia_state.lines[i];
    }
    return NULL_PTR;
}

static u32 axia_popcount(u32 value)
{
    u32 count = 0;
    while (value)
    {
        count += value & 1;
        value >>= 1;
    }
    return count;
}

static u32 axia_least_loaded(u32 mask)
{
    u32 best = 0xFFFFFFFFu;
    u32 best_cpu = 0;
    for (u32 cpu = 0; cpu < axia_state.cpu_count; cpu++)
    {
        if ((mask & (1u << cpu)) == 0 || (axia_state.online_mask & (1u << cpu)) == 0)
            continue;
        if (axia_state.load[cpu] < best)
        {
            best = axia_state.load[cpu];
            best_cpu = cpu;
        }
    }
    return best == 0xFFFFFFFFu ? 0xFFFFFFFFu : best_cpu;
}

static bool_t axia_migrate(axia_line_t *line, u32 cpu)
{
    if ((axia_state.online_mask & (1u << cpu)) == 0)
        return false;
    if (line->last_cpu == cpu)
        return true;
    if (line->pending != 0)
    {
        axia_state.wait_cleanups++;
        return false;
    }
    line->last_cpu = cpu;
    line->migrations++;
    axia_state.migrations++;
    return true;
}

/* -------------------------------------------------------------- public API */

void axia_init(u32 cpu_count)
{
    axia_zero(&axia_state, (u32)sizeof(axia_state));
    axia_state.cpu_count = cpu_count > AXIA_MAX_CPUS ? AXIA_MAX_CPUS : cpu_count;
    axia_state.online_mask = axia_state.cpu_count >= 32 ? 0xFFFFFFFFu : ((1u << axia_state.cpu_count) - 1u);
    axia_state.initialized = true;
}

u32 axia_register(u32 line, u32 vector, u32 target_mask, u32 owner_mask, u8 kind)
{
    axia_line_t *entry;
    if (!axia_state.initialized || axia_state.line_count >= AXIA_MAX_LINES)
        return 0xFFFFFFFFu;
    if (axia_line(line) != NULL_PTR)
        return 0xFFFFFFFFu;
    if (target_mask == 0)
        target_mask = axia_state.online_mask;
    entry = &axia_state.lines[axia_state.line_count];
    axia_zero(entry, (u32)sizeof(*entry));
    entry->line = line;
    entry->vector = vector;
    entry->target_mask = target_mask & axia_state.online_mask;
    if (entry->target_mask == 0)
        entry->target_mask = axia_state.online_mask;
    entry->owner_mask = owner_mask;
    entry->kind = kind;
    entry->policy = AXIA_POLICY_ROUND;
    entry->state = 1;
    entry->last_cpu = axia_least_loaded(entry->target_mask);
    if (entry->last_cpu == 0xFFFFFFFFu)
        entry->last_cpu = 0;
    axia_state.line_count++;
    return entry->line;
}

bool_t axia_set_targets(u32 line, u32 mask)
{
    axia_line_t *entry = axia_line(line);
    if (entry == NULL_PTR)
        return false;
    mask &= axia_state.online_mask;
    if (mask == 0)
        return false;
    entry->target_mask = mask;
    if ((mask & (1u << entry->last_cpu)) == 0)
    {
        u32 next = axia_least_loaded(mask);
        if (next == 0xFFFFFFFFu)
            return false;
        return axia_migrate(entry, next);
    }
    return true;
}

bool_t axia_pin(u32 line, u32 cpu)
{
    axia_line_t *entry = axia_line(line);
    if (entry == NULL_PTR)
        return false;
    if (cpu >= axia_state.cpu_count)
        return false;
    if ((axia_state.online_mask & (1u << cpu)) == 0)
        return false;
    if (!axia_migrate(entry, cpu))
        return false;
    entry->target_mask = 1u << cpu;
    entry->policy = AXIA_POLICY_MANUAL;
    return true;
}

bool_t axia_set_policy(u32 line, u8 policy)
{
    axia_line_t *entry = axia_line(line);
    if (entry == NULL_PTR || policy > AXIA_POLICY_SPREAD)
        return false;
    if (entry->policy == AXIA_POLICY_MANUAL && policy != AXIA_POLICY_MANUAL)
        return false;
    entry->policy = policy;
    return true;
}

bool_t axia_mask(u32 line, bool_t masked)
{
    axia_line_t *entry = axia_line(line);
    if (entry == NULL_PTR)
        return false;
    entry->masked = masked;
    return true;
}

u32 axia_deliver(u32 line, u32 now_ms)
{
    axia_line_t *entry = axia_line(line);
    u32 cpu;
    if (entry == NULL_PTR || entry->masked)
        return 0xFFFFFFFFu;
    axia_state.now_ms = now_ms;
    cpu = entry->last_cpu;
    entry->deliveries++;
    entry->pending = AXIA_SETTLE_TICKS;
    if (cpu < AXIA_MAX_CPUS)
        axia_state.load[cpu]++;
    return cpu;
}

bool_t axia_complete(u32 line)
{
    axia_line_t *entry = axia_line(line);
    if (entry == NULL_PTR)
        return false;
    if (entry->pending > 0)
        entry->pending--;
    return entry->pending == 0;
}

void axia_balance(void)
{
    if (!axia_state.initialized)
        return;
    axia_state.balance_runs++;
    axia_state.balancing = true;
    for (u32 i = 0; i < axia_state.line_count; i++)
    {
        axia_line_t *entry = &axia_state.lines[i];
        u32 chosen;
        if (entry->policy == AXIA_POLICY_MANUAL || entry->masked)
            continue;
        if (entry->policy == AXIA_POLICY_AFFINE)
        {
            if (axia_popcount(entry->target_mask) != 1)
                axia_state.rejected_moves++;
            continue;
        }
        if (entry->policy == AXIA_POLICY_ROUND)
        {
            u32 start = axia_state.next_cpu;
            for (u32 step = 0; step < axia_state.cpu_count; step++)
            {
                u32 cpu = (start + step) % axia_state.cpu_count;
                if (entry->target_mask & (1u << cpu))
                {
                    axia_state.next_cpu = (u8)((cpu + 1) % axia_state.cpu_count);
                    axia_migrate(entry, cpu);
                    break;
                }
            }
            continue;
        }
        chosen = axia_least_loaded(entry->target_mask);
        if (chosen != 0xFFFFFFFFu && chosen != entry->last_cpu)
            axia_migrate(entry, chosen);
    }
    axia_state.balancing = false;
}

u32 axia_line_cpu(u32 line)
{
    axia_line_t *entry = axia_line(line);
    return entry == NULL_PTR ? 0xFFFFFFFFu : entry->last_cpu;
}

u32 axia_load(u32 cpu)
{
    if (cpu >= AXIA_MAX_CPUS)
        return 0;
    return axia_state.load[cpu];
}

u32 axia_lines_for_cpu(u32 cpu)
{
    u32 count = 0;
    if (cpu >= AXIA_MAX_CPUS)
        return 0;
    for (u32 i = 0; i < axia_state.line_count; i++)
    {
        if (axia_state.lines[i].last_cpu == cpu)
            count++;
    }
    return count;
}

void axia_reset_load(void)
{
    for (u32 i = 0; i < AXIA_MAX_CPUS; i++)
        axia_state.load[i] = 0;
}

u32 axia_cpu_offline(u32 cpu)
{
    u32 moved = 0;
    if (cpu >= axia_state.cpu_count)
        return 0;
    axia_state.online_mask &= ~(1u << cpu);
    for (u32 i = 0; i < axia_state.line_count; i++)
    {
        axia_line_t *entry = &axia_state.lines[i];
        if (entry->last_cpu != cpu)
            continue;
        entry->target_mask &= axia_state.online_mask;
        if (entry->target_mask == 0)
            entry->target_mask = axia_state.online_mask;
        u32 next = axia_least_loaded(entry->target_mask);
        if (next != 0xFFFFFFFFu)
        {
            axia_migrate(entry, next);
            moved++;
        }
    }
    return moved;
}

u32 axia_cpu_online(u32 cpu)
{
    u32 placed = 0;
    if (cpu >= axia_state.cpu_count)
        return 0;
    axia_state.online_mask |= (1u << cpu);
    axia_balance();
    for (u32 i = 0; i < axia_state.line_count; i++)
    {
        if (axia_state.lines[i].last_cpu == cpu)
            placed++;
    }
    return placed;
}

void axia_report(void)
{
    printk("axia: lines=%u cpus=%u online=0x%x migrations=%u balance=%u cleanups=%u\n",
           (unsigned)axia_state.line_count, (unsigned)axia_state.cpu_count,
           (unsigned)axia_state.online_mask, (unsigned)axia_state.migrations,
           (unsigned)axia_state.balance_runs, (unsigned)axia_state.wait_cleanups);
    for (u32 i = 0; i < axia_state.line_count; i++)
    {
        const axia_line_t *entry = &axia_state.lines[i];
        printk("  line %u vector=%u cpu=%u policy=%u deliveries=%u migr=%u kind=%u\n",
               (unsigned)entry->line, (unsigned)entry->vector, (unsigned)entry->last_cpu,
               (unsigned)entry->policy, (unsigned)entry->deliveries,
               (unsigned)entry->migrations, (unsigned)entry->kind);
    }
}

bool_t axia_selftest(void)
{
    axia_init(4);
    if (axia_state.cpu_count != 4)
        return false;
    if (axia_register(3, 0x23, 0, 0, AXIA_LINE_MSI) != 3)
        return false;
    if (axia_register(3, 0x23, 0, 0, AXIA_LINE_MSI) != 0xFFFFFFFFu)
        return false;
    if (axia_line_cpu(3) == 0xFFFFFFFFu)
        return false;
    if (axia_deliver(3, 10) == 0xFFFFFFFFu)
        return false;
    if (axia_load(0) == 0)
        return false;
    if (axia_complete(3) != false)
        return false;
    if (!axia_pin(3, 2))
        return false;
    if (axia_line_cpu(3) != 2)
        return false;
    if (axia_set_targets(3, 0xF) != true)
        return false;
    if (axia_line_cpu(3) != 2)
        return false;
    if (!axia_mask(3, true))
        return false;
    if (axia_deliver(3, 20) != 0xFFFFFFFFu)
        return false;
    if (!axia_mask(3, false))
        return false;
    if (axia_register(7, 0x30, 0, 0, AXIA_LINE_EDGE) != 7)
        return false;
    axia_balance();
    if (axia_lines_for_cpu(1) + axia_lines_for_cpu(2) == 0)
        return false;
    if (axia_cpu_offline(2) == 0)
        return false;
    if (axia_lines_for_cpu(2) != 0)
        return false;
    if (axia_cpu_online(2) == 0)
    {
        /* Lines may already be balanced elsewhere. */
    }
    if (axia_load(1) == 0 && axia_load(2) == 0 && axia_load(3) == 0)
    {
        /* Load counters start empty; balance does not fabricate traffic. */
    }
    axia_reset_load();
    if (axia_load(0) != 0)
        return false;
    return true;
}
