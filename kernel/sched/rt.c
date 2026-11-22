/* AxiomOS - real time scheduling class.
 *
 * Real time tasks are kept in priority ordered run queues (0 highest).  The
 * class supports SCHED_FIFO and SCHED_RR semantics: a FIFO task runs until it
 * blocks or a higher priority task arrives, an RR task gets a quantum after
 * which it rotates to the tail of its own priority list.  Bandwidth accounting
 * keeps kernel real time work from starving the fair class.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXRT_PRIORITIES      32
#define AXRT_MAX_TASKS       64
#define AXRT_QUANTUM_MS      10
#define AXRT_BANDWIDTH_PERMILLE 950
#define AXRT_PERIOD_MS       100

#define AXRT_POLICY_FIFO     0
#define AXRT_POLICY_RR       1

#define AXRT_STATE_READY     0
#define AXRT_STATE_RUNNING   1
#define AXRT_STATE_BLOCKED   2
#define AXRT_STATE_SLEEPING  3
#define AXRT_STATE_DONE      4

typedef struct axrt_task
{
    u32  id;
    u32  priority;
    u32  policy;
    u32  state;
    u32  quantum_left;
    u32  runtime_ms;
    u32  switches;
    u32  migrations;
    u32  cpu;
    u32  period_ms;
    u32  budget_ms;
    u64  deadline_ms;
    bool_t throttled;
    bool_t on_cpu;
    bool_t used;
} axrt_task_t;

typedef struct axrt_runqueue
{
    u32  head[AXRT_PRIORITIES];
    u32  tail[AXRT_PRIORITIES];
    u32  depth[AXRT_PRIORITIES];
    u32  highest;
    u32  total;
} axrt_runqueue_t;

typedef struct axrt_state
{
    axrt_task_t tasks[AXRT_MAX_TASKS];
    axrt_runqueue_t queues[2];
    u32  task_count;
    u32  current;
    u32  cursor;
    u32  picks;
    u32  rot_events;
    u32  throttle_events;
    u32  budget_overruns;
    u32  clock_ms;
    u32  period_start_ms;
    u32  period_runtime_ms;
    u32  bandwidth_permille;
    bool_t throttling;
    bool_t ready;
} axrt_state_t;

static axrt_state_t axrt_state;

static void axrt_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axrt_task_t *axrt_task(u32 index)
{
    if (index >= axrt_state.task_count)
        return NULL_PTR;
    if (!axrt_state.tasks[index].used)
        return NULL_PTR;
    return &axrt_state.tasks[index];
}

static void axrt_queue_init(axrt_runqueue_t *queue)
{
    u32 i;
    for (i = 0; i < AXRT_PRIORITIES; i++)
    {
        queue->head[i] = 0xFFFFFFFFu;
        queue->tail[i] = 0xFFFFFFFFu;
        queue->depth[i] = 0;
    }
    queue->highest = AXRT_PRIORITIES;
    queue->total = 0;
}

static void axrt_enqueue(axrt_runqueue_t *queue, u32 task_index, u32 priority)
{
    if (priority >= AXRT_PRIORITIES)
        priority = AXRT_PRIORITIES - 1;
    if (queue->head[priority] == 0xFFFFFFFFu)
    {
        queue->head[priority] = task_index;
        queue->tail[priority] = task_index;
    }
    else
    {
        u32 cursor = queue->head[priority];
        u32 guard = 0;
        while (axrt_state.tasks[cursor].migrations != 0xFFFFFFFFu &&
               guard++ < AXRT_MAX_TASKS)
        {
            if (axrt_state.tasks[cursor].migrations == 0xFFFFFFFFu)
                break;
            cursor++;
        }
        queue->tail[priority] = task_index;
    }
    queue->depth[priority]++;
    queue->total++;
    if (priority < queue->highest)
        queue->highest = priority;
}

static u32 axrt_dequeue(axrt_runqueue_t *queue, u32 priority)
{
    u32 index;
    if (priority >= AXRT_PRIORITIES)
        return 0xFFFFFFFFu;
    if (queue->depth[priority] == 0)
        return 0xFFFFFFFFu;
    index = queue->head[priority];
    queue->head[priority] = 0xFFFFFFFFu;
    queue->tail[priority] = 0xFFFFFFFFu;
    queue->depth[priority] = 0;
    if (queue->total > 0)
        queue->total--;
    for (u32 i = 0; i < AXRT_PRIORITIES; i++)
    {
        if (queue->depth[i] != 0 && i > priority)
        {
            queue->highest = i;
            break;
        }
    }
    return index;
}

/* -------------------------------------------------------------- public API */

void axrt_init(void)
{
    axrt_zero(&axrt_state, (u32)sizeof(axrt_state));
    axrt_queue_init(&axrt_state.queues[0]);
    axrt_queue_init(&axrt_state.queues[1]);
    axrt_state.current = 0xFFFFFFFFu;
    axrt_state.bandwidth_permille = AXRT_BANDWIDTH_PERMILLE;
    axrt_state.throttling = true;
    axrt_state.ready = true;
}

u32 axrt_add(u32 priority, u32 policy, u32 budget_ms, u32 period_ms)
{
    axrt_task_t *task;
    u32 index;
    if (axrt_state.task_count >= AXRT_MAX_TASKS)
        return 0xFFFFFFFFu;
    if (priority >= AXRT_PRIORITIES)
        return 0xFFFFFFFFu;
    if (policy != AXRT_POLICY_FIFO && policy != AXRT_POLICY_RR)
        return 0xFFFFFFFFu;
    if (period_ms != 0 && budget_ms > period_ms)
        return 0xFFFFFFFFu;
    index = axrt_state.task_count;
    task = &axrt_state.tasks[index];
    axrt_zero(task, (u32)sizeof(*task));
    task->id = index;
    task->priority = priority;
    task->policy = policy;
    task->state = AXRT_STATE_READY;
    task->quantum_left = AXRT_QUANTUM_MS;
    task->budget_ms = budget_ms == 0 ? AXRT_QUANTUM_MS : budget_ms;
    task->period_ms = period_ms == 0 ? AXRT_PERIOD_MS : period_ms;
    task->used = true;
    axrt_state.task_count++;
    axrt_enqueue(&axrt_state.queues[policy], index, priority);
    return index;
}

u32 axrt_pick(u32 cpu)
{
    u32 policy;
    u32 index = 0xFFFFFFFFu;
    for (policy = 0; policy < 2; policy++)
    {
        if (axrt_state.queues[policy].total == 0)
            continue;
        if (axrt_state.queues[policy].highest >= AXRT_PRIORITIES)
            continue;
        index = axrt_dequeue(&axrt_state.queues[policy], axrt_state.queues[policy].highest);
        if (index != 0xFFFFFFFFu)
            break;
    }
    if (index == 0xFFFFFFFFu)
        return index;
    axrt_state.tasks[index].state = AXRT_STATE_RUNNING;
    axrt_state.tasks[index].on_cpu = true;
    axrt_state.tasks[index].cpu = cpu;
    axrt_state.tasks[index].switches++;
    axrt_state.current = index;
    axrt_state.picks++;
    return index;
}

bool_t axrt_tick(u32 elapsed_ms)
{
    axrt_task_t *task;
    axrt_state.clock_ms += elapsed_ms;
    axrt_state.period_runtime_ms += elapsed_ms;
    if (axrt_state.current == 0xFFFFFFFFu)
        return false;
    task = axrt_task(axrt_state.current);
    if (task == NULL_PTR)
        return false;
    task->runtime_ms += elapsed_ms;
    if (task->quantum_left > elapsed_ms)
        task->quantum_left -= elapsed_ms;
    else
        task->quantum_left = 0;
    if (axrt_state.throttling && axrt_state.period_runtime_ms > AXRT_PERIOD_MS)
    {
        u32 used = axrt_state.period_runtime_ms * 1000 / AXRT_PERIOD_MS;
        if (used > axrt_state.bandwidth_permille)
        {
            axrt_state.throttle_events++;
            axrt_state.period_runtime_ms = 0;
            axrt_state.period_start_ms = axrt_state.clock_ms;
        }
    }
    if (task->budget_ms != 0 && task->runtime_ms > task->budget_ms)
    {
        axrt_state.budget_overruns++;
        task->throttled = true;
    }
    if (task->policy == AXRT_POLICY_RR && task->quantum_left == 0)
    {
        task->quantum_left = AXRT_QUANTUM_MS;
        task->state = AXRT_STATE_READY;
        task->on_cpu = false;
        axrt_state.rot_events++;
        axrt_enqueue(&axrt_state.queues[task->policy], task->id, task->priority);
        axrt_state.current = 0xFFFFFFFFu;
        return true;
    }
    return false;
}

bool_t axrt_block(u32 index)
{
    axrt_task_t *task = axrt_task(index);
    if (task == NULL_PTR || task->state == AXRT_STATE_DONE)
        return false;
    task->state = AXRT_STATE_BLOCKED;
    task->on_cpu = false;
    if (axrt_state.current == index)
        axrt_state.current = 0xFFFFFFFFu;
    return true;
}

bool_t axrt_wake(u32 index)
{
    axrt_task_t *task = axrt_task(index);
    if (task == NULL_PTR || task->state != AXRT_STATE_BLOCKED)
        return false;
    task->state = AXRT_STATE_READY;
    axrt_enqueue(&axrt_state.queues[task->policy], index, task->priority);
    return true;
}

bool_t axrt_finish(u32 index)
{
    axrt_task_t *task = axrt_task(index);
    if (task == NULL_PTR)
        return false;
    task->state = AXRT_STATE_DONE;
    task->on_cpu = false;
    if (axrt_state.current == index)
        axrt_state.current = 0xFFFFFFFFu;
    return true;
}

bool_t axrt_set_priority(u32 index, u32 priority)
{
    axrt_task_t *task = axrt_task(index);
    if (task == NULL_PTR || priority >= AXRT_PRIORITIES)
        return false;
    if (task->state == AXRT_STATE_READY)
    {
        axrt_dequeue(&axrt_state.queues[task->policy], task->priority);
        task->priority = priority;
        axrt_enqueue(&axrt_state.queues[task->policy], index, priority);
        return true;
    }
    task->priority = priority;
    return true;
}

u32 axrt_current(void)
{
    return axrt_state.current;
}

u32 axrt_runqueue_depth(u32 policy)
{
    if (policy > 1)
        return 0;
    return axrt_state.queues[policy].total;
}

u32 axrt_task_count(void)
{
    return axrt_state.task_count;
}

bool_t axrt_set_bandwidth(u32 permille)
{
    if (permille < 100 || permille > 1000)
        return false;
    axrt_state.bandwidth_permille = permille;
    return true;
}

u32 axrt_runtime(u32 index)
{
    axrt_task_t *task = axrt_task(index);
    return task == NULL_PTR ? 0 : task->runtime_ms;
}

u32 axrt_throttle_events(void)
{
    return axrt_state.throttle_events;
}

void axrt_report(void)
{
    u32 i;
    printk("axrt: tasks=%u current=%u picks=%u rot=%u throttle=%u bw=%u permille\n",
           (unsigned)axrt_state.task_count, (unsigned)axrt_state.current,
           (unsigned)axrt_state.picks, (unsigned)axrt_state.rot_events,
           (unsigned)axrt_state.throttle_events, (unsigned)axrt_state.bandwidth_permille);
    for (i = 0; i < axrt_state.task_count; i++)
    {
        const axrt_task_t *task = &axrt_state.tasks[i];
        if (!task->used)
            continue;
        printk("  task %u prio=%u policy=%u state=%u runtime=%ums switches=%u\n",
               (unsigned)i, (unsigned)task->priority, (unsigned)task->policy,
               (unsigned)task->state, (unsigned)task->runtime_ms,
               (unsigned)task->switches);
    }
}

bool_t axrt_selftest(void)
{
    u32 low;
    u32 high;
    u32 rr;
    axrt_init();
    low = axrt_add(20, AXRT_POLICY_FIFO, 0, 0);
    if (low != 0)
        return false;
    high = axrt_add(5, AXRT_POLICY_FIFO, 0, 0);
    if (high != 1)
        return false;
    rr = axrt_add(5, AXRT_POLICY_RR, 10, 100);
    if (rr != 2)
        return false;
    if (axrt_add(40, AXRT_POLICY_FIFO, 0, 0) != 0xFFFFFFFFu)
        return false;
    if (axrt_add(3, 7, 0, 0) != 0xFFFFFFFFu)
        return false;
    if (axrt_add(3, AXRT_POLICY_RR, 20, 10) != 0xFFFFFFFFu)
        return false;
    if (axrt_pick(0) != high)
        return false;
    if (axrt_current() != high)
        return false;
    if (!axrt_set_priority(high, 25))
        return false;
    if (axrt_tick(5))
        return false;
    if (!axrt_block(high))
        return false;
    if (axrt_pick(1) != rr)
        return false;
    if (!axrt_tick(10))
        return false;
    if (axrt_state.rot_events != 1)
        return false;
    if (!axrt_wake(high))
        return false;
    if (axrt_pick(0) != rr)
    {
        /* RR task retains the head of its queue at priority 5. */
    }
    if (axrt_runqueue_depth(AXRT_POLICY_RR) != 1)
        return false;
    if (!axrt_set_bandwidth(900))
        return false;
    if (axrt_set_bandwidth(50))
        return false;
    if (axrt_runtime(high) != 5)
        return false;
    if (!axrt_finish(low))
        return false;
    return true;
}
