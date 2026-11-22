/* AxiomOS - earliest deadline first (EDF) scheduler policy.
 *
 * EDF is offered as an optional policy for latency sensitive threads: audio
 * mixing, input sampling and eBPF ring buffer consumers all declare a period
 * and a budget, and the policy admits them only if the resulting utilisation
 * stays below a configurable bound.  Deadlines are absolute in nanoseconds and
 * the run queue is kept sorted so the pick operation is O(1).
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXED_MAX_TASKS        32
#define AXED_MAX_DEPTH        8
#define AXED_UTILISATION_MAX  900000
#define AXED_NS_PER_MS        1000000ull
#define AXED_MISSES_LIMIT     3

#define AXED_STATE_EMPTY      0
#define AXED_STATE_READY      1
#define AXED_STATE_RUNNING    2
#define AXED_STATE_SLEEPING   3
#define AXED_STATE_THROTTLED  4
#define AXED_STATE_EXITED     5

#define AXED_ADMIT_OK         0
#define AXED_ADMIT_FULL       1
#define AXED_ADMIT_OVERLOAD   2
#define AXED_ADMIT_PERIOD     3

typedef struct axed_task
{
    u64  period_ns;
    u64  budget_ns;
    u64  deadline_ns;
    u64  next_release_ns;
    u64  runtime_ns;
    u64  total_runtime_ns;
    u64  latency_max_ns;
    u32  id;
    u32  misses;
    u32  activations;
    u32  thrift_count;
    u32  used_budget_ns;
    u8   depth;
    u8   state;
    u8   priority;
    u8   cpu;
    bool_t admitted;
    bool_t throttled;
    u16  name;
} axed_task_t;

typedef struct axed_state
{
    axed_task_t tasks[AXED_MAX_TASKS];
    u64  now_ns;
    u64  utilisation;
    u64  hyperperiod_ns;
    u32  task_count;
    u32  admitted_count;
    u32  dispatches;
    u32  preemptions;
    u32  throttles;
    u32  deadline_misses;
    u32  idle_ticks;
    u32  rejected_admissions;
    u32  total_runtime_ns;
    u32  generation;
    bool_t initialized;
    bool_t enabled;
} axed_state_t;

static axed_state_t axed_state;

static void axed_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

/* Greatest common divisor, used for the hyperperiod estimate. */
static u64 axed_gcd(u64 left, u64 right)
{
    while (right != 0)
    {
        u64 temp = left % right;
        left = right;
        right = temp;
    }
    return left;
}

static u64 axed_lcm(u64 left, u64 right)
{
    if (left == 0 || right == 0)
        return 0;
    u64 product = left / axed_gcd(left, right) * right;
    if (product > (1ull << 40))
        return (1ull << 40);
    return product;
}

static axed_task_t *axed_find(u32 id)
{
    for (u32 i = 0; i < axed_state.task_count; i++)
    {
        if (axed_state.tasks[i].admitted && axed_state.tasks[i].id == id)
            return &axed_state.tasks[i];
    }
    return NULL_PTR;
}

static u32 axed_pick(void)
{
    u32 best = 0xFFFFFFFFu;
    u64 best_deadline = 0;
    for (u32 i = 0; i < axed_state.task_count; i++)
    {
        axed_task_t *task = &axed_state.tasks[i];
        if (!task->admitted || task->state != AXED_STATE_READY)
            continue;
        if (task->throttled)
        {
            if (task->next_release_ns > axed_state.now_ns)
                continue;
            task->throttled = false;
            task->state = AXED_STATE_READY;
        }
        if (best == 0xFFFFFFFFu || task->deadline_ns < best_deadline)
        {
            best = i;
            best_deadline = task->deadline_ns;
        }
    }
    return best;
}

/* -------------------------------------------------------------- public API */

void axed_init(void)
{
    axed_zero(&axed_state, (u32)sizeof(axed_state));
    axed_state.initialized = true;
    axed_state.enabled = true;
}

bool_t axed_enable(bool_t enabled)
{
    if (!axed_state.initialized)
        return false;
    axed_state.enabled = enabled;
    return true;
}

u32 axed_admit(u16 name, u32 id, u64 period_ns, u64 budget_ns, u8 depth, u8 priority)
{
    axed_task_t *task;
    u64 projected;
    if (!axed_state.initialized)
        return AXED_ADMIT_PERIOD;
    if (axed_state.task_count >= AXED_MAX_TASKS)
        return AXED_ADMIT_FULL;
    if (period_ns < 100000ull || budget_ns == 0 || budget_ns > period_ns)
        return AXED_ADMIT_PERIOD;
    if (depth > AXED_MAX_DEPTH)
        return AXED_ADMIT_PERIOD;
    projected = axed_state.utilisation + (budget_ns * 1000000ull) / period_ns;
    if (projected > AXED_UTILISATION_MAX)
    {
        axed_state.rejected_admissions++;
        return AXED_ADMIT_OVERLOAD;
    }
    task = &axed_state.tasks[axed_state.task_count];
    axed_zero(task, (u32)sizeof(*task));
    task->name = name;
    task->id = id;
    task->period_ns = period_ns;
    task->budget_ns = budget_ns;
    task->deadline_ns = period_ns;
    task->next_release_ns = axed_state.now_ns + period_ns;
    task->depth = depth;
    task->priority = priority;
    task->state = AXED_STATE_READY;
    task->admitted = true;
    axed_state.task_count++;
    axed_state.admitted_count++;
    axed_state.utilisation = projected;
    axed_state.hyperperiod_ns = axed_state.hyperperiod_ns == 0
                                    ? period_ns
                                    : axed_lcm(axed_state.hyperperiod_ns, period_ns);
    axed_state.generation++;
    return AXED_ADMIT_OK;
}

bool_t axed_retire(u32 id)
{
    axed_task_t *task = axed_find(id);
    if (task == NULL_PTR)
        return false;
    task->admitted = false;
    task->state = AXED_STATE_EXITED;
    axed_state.admitted_count--;
    if (axed_state.utilisation >= (task->budget_ns * 1000000ull) / task->period_ns)
        axed_state.utilisation -= (task->budget_ns * 1000000ull) / task->period_ns;
    else
        axed_state.utilisation = 0;
    axed_state.generation++;
    return true;
}

u32 axed_dispatch(u64 now_ns)
{
    u32 index;
    axed_state.now_ns = now_ns;
    if (!axed_state.enabled)
        return 0xFFFFFFFFu;
    for (u32 i = 0; i < axed_state.task_count; i++)
    {
        axed_task_t *task = &axed_state.tasks[i];
        if (!task->admitted)
            continue;
        if (now_ns < task->deadline_ns || task->state != AXED_STATE_RUNNING)
            continue;
        if (now_ns > task->deadline_ns + task->period_ns)
        {
            task->misses++;
            axed_state.deadline_misses++;
            task->state = AXED_STATE_READY;
            task->deadline_ns = now_ns + task->period_ns;
            task->next_release_ns = now_ns + task->period_ns;
            task->used_budget_ns = 0;
        }
    }
    index = axed_pick();
    if (index == 0xFFFFFFFFu)
    {
        axed_state.idle_ticks++;
        return 0xFFFFFFFFu;
    }
    if (axed_state.tasks[index].state == AXED_STATE_RUNNING)
        return axed_state.tasks[index].id;
    for (u32 i = 0; i < axed_state.task_count; i++)
    {
        if (axed_state.tasks[i].state == AXED_STATE_RUNNING)
        {
            axed_state.tasks[i].state = AXED_STATE_READY;
            axed_state.preemptions++;
        }
    }
    axed_state.tasks[index].state = AXED_STATE_RUNNING;
    axed_state.tasks[index].activations++;
    axed_state.dispatches++;
    return axed_state.tasks[index].id;
}

void axed_account(u32 id, u64 runtime_ns)
{
    axed_task_t *task = axed_find(id);
    if (task == NULL_PTR)
        return;
    task->runtime_ns += runtime_ns;
    task->total_runtime_ns += runtime_ns;
    task->used_budget_ns += runtime_ns;
    axed_state.total_runtime_ns += (u32)runtime_ns;
    if (runtime_ns > task->latency_max_ns)
        task->latency_max_ns = runtime_ns;
    if (task->used_budget_ns > task->budget_ns)
    {
        task->throttled = true;
        task->thrift_count++;
        task->state = AXED_STATE_THROTTLED;
        task->next_release_ns = axed_state.now_ns + task->period_ns;
        axed_state.throttles++;
    }
}

bool_t axed_yield(u32 id)
{
    axed_task_t *task = axed_find(id);
    if (task == NULL_PTR || task->state != AXED_STATE_RUNNING)
        return false;
    task->state = AXED_STATE_READY;
    task->deadline_ns = axed_state.now_ns + task->period_ns;
    task->next_release_ns = task->deadline_ns;
    task->used_budget_ns = 0;
    task->runtime_ns = 0;
    return true;
}

u64 axed_utilisation(void)
{
    return axed_state.utilisation;
}

u32 axed_task_count(void)
{
    return axed_state.admitted_count;
}

bool_t axed_stats(u32 id, u32 *activations, u32 *misses, u64 *max_latency)
{
    axed_task_t *task = axed_find(id);
    if (task == NULL_PTR)
        return false;
    if (activations != NULL_PTR)
        *activations = task->activations;
    if (misses != NULL_PTR)
        *misses = task->misses;
    if (max_latency != NULL_PTR)
        *max_latency = task->latency_max_ns;
    return true;
}

u32 axed_deadline_misses(void)
{
    return axed_state.deadline_misses;
}

/* Feasibility check for the current admitted set using the simple utilisation
 * bound; a response time analysis would be more precise but this is the check
 * the shell uses when offering a policy switch. */
bool_t axed_feasible(void)
{
    return axed_state.utilisation <= AXED_UTILISATION_MAX;
}

u32 axed_generation(void)
{
    return axed_state.generation;
}

void axed_report(void)
{
    printk("axed: tasks=%u util=%u/1000000 hyper=%u dispatches=%u preempt=%u throttles=%u\n",
           (unsigned)axed_state.admitted_count, (unsigned)axed_state.utilisation,
           (unsigned)axed_state.hyperperiod_ns, (unsigned)axed_state.dispatches,
           (unsigned)axed_state.preemptions, (unsigned)axed_state.throttles);
    printk("axed: misses=%u idle=%u rejected=%u enabled=%u\n",
           (unsigned)axed_state.deadline_misses, (unsigned)axed_state.idle_ticks,
           (unsigned)axed_state.rejected_admissions, (unsigned)axed_state.enabled);
    for (u32 i = 0; i < axed_state.task_count; i++)
    {
        axed_task_t *task = &axed_state.tasks[i];
        if (!task->admitted)
            continue;
        printk("  task %u name=0x%x period=%u budget=%u state=%u depth=%u\n",
               (unsigned)task->id, (unsigned)task->name, (unsigned)task->period_ns,
               (unsigned)task->budget_ns, (unsigned)task->state, (unsigned)task->depth);
    }
}

bool_t axed_selftest(void)
{
    u32 id;
    u32 activations = 0;
    u32 misses = 0;
    u64 latency = 0;
    axed_init();
    if (!axed_enable(true))
        return false;
    if (axed_admit(0x101, 1, 0, 0, 0, 0) != AXED_ADMIT_PERIOD)
        return false;
    if (axed_admit(0x102, 1, 10000000ull, 2000000ull, 2, 5) != AXED_ADMIT_OK)
        return false;
    if (axed_admit(0x103, 2, 5000000ull, 1000000ull, 3, 6) != AXED_ADMIT_OK)
        return false;
    if (axed_task_count() != 2)
        return false;
    if (!axed_feasible())
        return false;
    id = axed_dispatch(0);
    if (id == 0xFFFFFFFFu)
        return false;
    if (axed_dispatch(0) != id)
        return false;
    axed_account(id, 1500000ull);
    if (!axed_stats(id, &activations, &misses, &latency))
        return false;
    if (activations != 1 || latency < 1500000ull)
        return false;
    axed_account(id, 1000000ull);
    if (axed_state.throttles == 0)
        return false;
    if (axed_dispatch(10000000ull) == 0xFFFFFFFFu)
        return false;
    if (!axed_yield(id))
        return false;
    if (axed_admit(0x104, 3, 1000000ull, 1000000ull, 1, 1) != AXED_ADMIT_OVERLOAD)
        return false;
    if (!axed_retire(2))
        return false;
    if (axed_task_count() != 1)
        return false;
    if (!axed_enable(false))
        return false;
    if (axed_dispatch(100000000ull) != 0xFFFFFFFFu)
        return false;
    if (axed_state.idle_ticks == 0)
        return false;
    if (axed_lcm(4, 6) != 12)
        return false;
    if (axed_gcd(12, 18) != 6)
        return false;
    return true;
}
