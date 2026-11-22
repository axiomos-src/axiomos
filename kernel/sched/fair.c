/* AxiomOS - fair-share scheduler policy.
 *
 * The core scheduler in sched.c owns the run queue; this module supplies the
 * policy: virtual runtime accounting, sleeper credit, group shares and the
 * preemption decision.  Everything is integer math on a nanosecond-like tick
 * unit, so no floating point is pulled into the scheduler.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXFS_MAX_TASKS      64
#define AXFS_MAX_GROUPS     8
#define AXFS_PERIOD         1000000u
#define AXFS_MIN_SLICE      2000u
#define AXFS_MAX_SLICE      40000u
#define AXFS_LATENCY        12000u
#define AXFS_SLEEP_CREDIT   24u
#define AXFS_MAX_CREDIT     200u

typedef struct axsf_task
{
    u32  id;
    u32  group;
    u32  weight;
    u64  vruntime;
    u64  runtime;
    u32  slice;
    u32  slice_left;
    u32  credit;
    u32  wakeups;
    u32  migrations;
    u16  cpu;
    u8   nice;
    bool_t on_rq;
    bool_t sleeping;
} axsf_task_t;

typedef struct axsf_group
{
    u32  id;
    u32  weight;
    u64  vruntime;
    u32  shares;
    u32  tasks;
    u32  throttled;
    u32  quota;
    u32  used;
    bool_t limited;
} axsf_group_t;

typedef struct axsf_state
{
    axsf_task_t tasks[AXFS_MAX_TASKS];
    axsf_group_t groups[AXFS_MAX_GROUPS];
    u32  task_count;
    u32  group_count;
    u32  clock;
    u64  min_vruntime;
    u32  switches;
    u32  preemptions;
    u32  throttle_events;
    u32  cpu_count;
    bool_t ready;
} axsf_state_t;

static axsf_state_t axsf_state;

static const u32 axsf_weight_table[41] = {
    88761, 71755, 56483, 46273, 36291,
    29154, 23254, 18705, 14949, 11916,
     9548,  7620,  6100,  4904,  3906,
     3121,  2501,  1991,  1586,  1277,
     1024,   820,   655,   526,   423,
      335,   272,   215,   172,   137,
      110,    87,    70,    56,    45,
       36,    29,    23,    18,    15,
       12
};

static void axsf_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static u32 axsf_weight_for(i8 nice)
{
    i32 index = (i32)nice + 20;
    if (index < 0)
        index = 0;
    if (index > 40)
        index = 40;
    return axsf_weight_table[index];
}

static axsf_task_t *axsf_find(u32 id)
{
    u32 i;
    for (i = 0; i < axsf_state.task_count; i++)
    {
        if (axsf_state.tasks[i].id == id)
            return &axsf_state.tasks[i];
    }
    return NULL_PTR;
}

static u32 axsf_slice_for(u32 weight, u32 tasks)
{
    u64 slice;
    if (tasks == 0)
        tasks = 1;
    slice = (u64)AXFS_LATENCY * weight / 1024u;
    if (slice < AXFS_MIN_SLICE)
        slice = AXFS_MIN_SLICE;
    if (slice > AXFS_MAX_SLICE)
        slice = AXFS_MAX_SLICE;
    return (u32)slice;
}

static void axsf_update_vruntime(axsf_task_t *task, u32 delta)
{
    u64 scaled;
    if (delta == 0)
        return;
    scaled = (u64)delta * 1024ull / (task->weight == 0 ? 1 : task->weight);
    task->vruntime += scaled;
    task->runtime += delta;
    if (task->vruntime > axsf_state.min_vruntime + AXFS_PERIOD * 8ull)
        task->vruntime = axsf_state.min_vruntime + AXFS_PERIOD * 8ull;
}

/* -------------------------------------------------------------- public API */

void axsf_init(u32 cpus)
{
    axsf_zero(&axsf_state, (u32)sizeof(axsf_state));
    axsf_state.cpu_count = cpus == 0 ? 1 : cpus;
    axsf_state.ready = true;
}

u32 axsf_add_group(u32 id, u32 shares, u32 quota, bool_t limited)
{
    axsf_group_t *group;
    if (axsf_state.group_count >= AXFS_MAX_GROUPS)
        return 0xFFFFFFFFu;
    group = &axsf_state.groups[axsf_state.group_count];
    axsf_zero(group, (u32)sizeof(*group));
    group->id = id;
    group->shares = shares == 0 ? 1024 : shares;
    group->weight = group->shares;
    group->quota = quota;
    group->limited = limited;
    axsf_state.group_count++;
    return axsf_state.group_count - 1;
}

u32 axsf_add_task(u32 id, u32 group, i8 nice)
{
    axsf_task_t *task;
    if (axsf_state.task_count >= AXFS_MAX_TASKS)
        return 0xFFFFFFFFu;
    task = &axsf_state.tasks[axsf_state.task_count];
    axsf_zero(task, (u32)sizeof(*task));
    task->id = id;
    task->group = group;
    task->nice = nice;
    task->weight = axsf_weight_for(nice);
    task->vruntime = axsf_state.min_vruntime;
    task->credit = AXFS_SLEEP_CREDIT;
    task->on_rq = true;
    task->cpu = (u16)(axsf_state.task_count % axsf_state.cpu_count);
    axsf_state.task_count++;
    if (group < axsf_state.group_count)
        axsf_state.groups[group].tasks++;
    return axsf_state.task_count - 1;
}

bool_t axsf_dequeue(u32 id)
{
    axsf_task_t *task = axsf_find(id);
    if (task == NULL_PTR)
        return false;
    task->on_rq = false;
    task->sleeping = true;
    task->credit = AXFS_SLEEP_CREDIT;
    return true;
}

bool_t axsf_enqueue(u32 id)
{
    axsf_task_t *task = axsf_find(id);
    if (task == NULL_PTR)
        return false;
    task->on_rq = true;
    task->sleeping = false;
    task->wakeups++;
    if (task->vruntime > axsf_state.min_vruntime)
        task->vruntime = axsf_state.min_vruntime;
    return true;
}

u32 axsf_pick(u32 cpu)
{
    u32 i;
    u32 best = 0xFFFFFFFFu;
    u64 best_vruntime = 0;
    for (i = 0; i < axsf_state.task_count; i++)
    {
        axsf_task_t *task = &axsf_state.tasks[i];
        u64 effective;
        if (!task->on_rq || task->sleeping)
            continue;
        if (task->cpu != (u16)cpu && axsf_state.cpu_count > 1)
            continue;
        if (task->group < axsf_state.group_count)
        {
            axsf_group_t *group = &axsf_state.groups[task->group];
            if (group->limited && group->used >= group->quota)
                continue;
        }
        effective = task->vruntime;
        if (task->credit > 0)
            effective = effective > (u64)task->credit * 64ull ?
                        effective - (u64)task->credit * 64ull : 0;
        if (best == 0xFFFFFFFFu || effective < best_vruntime)
        {
            best = i;
            best_vruntime = effective;
        }
    }
    return best;
}

bool_t axsf_tick(u32 id, u32 delta)
{
    axsf_task_t *task = axsf_find(id);
    if (task == NULL_PTR)
        return false;
    if (!task->on_rq)
        return false;
    if (task->slice_left == 0)
    {
        task->slice = axsf_slice_for(task->weight, axsf_state.task_count);
        task->slice_left = task->slice;
    }
    if (delta >= task->slice_left)
    {
        axsf_update_vruntime(task, task->slice_left);
        task->slice_left = 0;
        axsf_state.switches++;
        return true;
    }
    axsf_update_vruntime(task, delta);
    task->slice_left -= delta;
    if (task->credit > 0)
        task->credit--;
    return false;
}

bool_t axsf_should_preempt(u32 running, u32 candidate)
{
    axsf_task_t *current = axsf_find(running);
    axsf_task_t *next = axsf_find(candidate);
    if (current == NULL_PTR || next == NULL_PTR)
        return false;
    if (next->vruntime + 1000 < current->vruntime)
    {
        axsf_state.preemptions++;
        return true;
    }
    return false;
}

void axsf_account_group(u32 group, u32 delta)
{
    if (group >= axsf_state.group_count)
        return;
    axsf_state.groups[group].used += delta;
    if (axsf_state.groups[group].used > AXFS_PERIOD)
    {
        axsf_state.groups[group].used = 0;
        if (axsf_state.groups[group].limited)
        {
            axsf_state.groups[group].throttled++;
            axsf_state.throttle_events++;
        }
    }
}

void axsf_recalc_min(void)
{
    u32 i;
    u64 min = 0xFFFFFFFFFFFFFFFFull;
    for (i = 0; i < axsf_state.task_count; i++)
    {
        if (!axsf_state.tasks[i].on_rq)
            continue;
        if (axsf_state.tasks[i].vruntime < min)
            min = axsf_state.tasks[i].vruntime;
    }
    if (min != 0xFFFFFFFFFFFFFFFFull)
        axsf_state.min_vruntime = min;
}

u64 axsf_vruntime(u32 id)
{
    axsf_task_t *task = axsf_find(id);
    return task == NULL_PTR ? 0 : task->vruntime;
}

u32 axsf_switches(void)
{
    return axsf_state.switches;
}

u32 axsf_preemptions(void)
{
    return axsf_state.preemptions;
}

void axsf_report(void)
{
    u32 i;
    printk("axfs: tasks=%u groups=%u switches=%u preempt=%u throttles=%u min=%llu\n",
           (unsigned)axsf_state.task_count, (unsigned)axsf_state.group_count,
           (unsigned)axsf_state.switches, (unsigned)axsf_state.preemptions,
           (unsigned)axsf_state.throttle_events,
           (unsigned long long)axsf_state.min_vruntime);
    for (i = 0; i < axsf_state.task_count; i++)
    {
        const axsf_task_t *task = &axsf_state.tasks[i];
        printk("  task %u group=%u nice=%d weight=%u vruntime=%llu slice=%u\n",
               (unsigned)task->id, (unsigned)task->group, (int)(i8)task->nice,
               (unsigned)task->weight, (unsigned long long)task->vruntime,
               (unsigned)task->slice);
    }
}

bool_t axsf_selftest(void)
{
    u32 a;
    u32 b;
    u32 picked;
    axsf_init(2);
    if (axsf_state.cpu_count != 2)
        return false;
    if (axsf_add_group(1, 2048, AXFS_PERIOD, true) != 0)
        return false;
    a = axsf_add_task(10, 0, 0);
    b = axsf_add_task(11, 0, 5);
    if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu)
        return false;
    if (axsf_weight_for(-20) != 88761)
        return false;
    if (axsf_weight_for(19) != 15)
        return false;
    if (axsf_weight_for(0) != 1024)
        return false;
    axsf_tick(a, 4000);
    if (axsf_vruntime(a) == 0)
        return false;
    if (axsf_vruntime(a) >= axsf_vruntime(b))
        return false;
    if (!axsf_should_preempt(b, a))
    {
        /* vruntime of b is zero, so b cannot preempt a; the reverse should. */
        if (!axsf_should_preempt(a, b))
            return false;
    }
    axsf_dequeue(a);
    picked = axsf_pick(0);
    if (picked != b)
        return false;
    axsf_enqueue(a);
    axsf_account_group(0, AXFS_PERIOD + 1);
    if (axsf_state.groups[0].throttled != 1)
        return false;
    axsf_recalc_min();
    if (axsf_state.min_vruntime != 0)
        return false;
    return true;
}

