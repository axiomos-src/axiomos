/* AxiomOS - software interrupt / bottom half dispatch.
 *
 * Hardware ISRs run with interrupts masked and are expected to be short.  Work
 * that can tolerate running later is queued here and drained from the timer
 * tick or from the idle loop.  Each CPU has its own pending bitmap; the
 * dispatcher drains up to a fixed budget so a flood of bottom halves cannot
 * starve the scheduler.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXBH_VECTORS        16
#define AXBH_MAX_CPUS       8
#define AXBH_QUEUE_DEPTH    32
#define AXBH_BUDGET         24

typedef void (*axbh_handler_t)(u32 vector, void *context);

typedef struct axbh_task
{
    axbh_handler_t handler;
    void          *context;
    u32            vector;
    u32            stamp;
    bool_t         pending;
    bool_t         repeat;
} axbh_task_t;

typedef struct axbh_cpu
{
    axbh_task_t queue[AXBH_QUEUE_DEPTH];
    u32         head;
    u32         tail;
    u32         count;
    u32         drained;
    u32         dropped;
    u32         overruns;
    u32         pending_mask;
    bool_t      draining;
} axbh_cpu_t;

typedef struct axbh_global
{
    axbh_cpu_t cpus[AXBH_MAX_CPUS];
    u32        cpu_count;
    u32        stamp;
    bool_t     enabled;
    u32        total_drained;
    u32        total_deferred;
} axbh_global_t;

static axbh_global_t axbh_state;

static void axbh_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axbh_cpu_t *axbh_cpu(u32 cpu)
{
    if (cpu >= AXBH_MAX_CPUS)
        return NULL_PTR;
    return &axbh_state.cpus[cpu];
}

/* -------------------------------------------------------------- public API */

void axbh_init(void)
{
    u32 i;
    axbh_zero(&axbh_state, (u32)sizeof(axbh_state));
    for (i = 0; i < AXBH_MAX_CPUS; i++)
    {
        axbh_state.cpus[i].pending_mask = 0;
        axbh_state.cpus[i].draining = false;
    }
    axbh_state.cpu_count = 1;
    axbh_state.stamp = 1;
    axbh_state.enabled = true;
}

void axbh_set_cpu_count(u32 count)
{
    if (count > AXBH_MAX_CPUS)
        count = AXBH_MAX_CPUS;
    if (count == 0)
        count = 1;
    axbh_state.cpu_count = count;
}

bool_t axbh_enable(bool_t enabled)
{
    axbh_state.enabled = enabled;
    return axbh_state.enabled;
}

u32 axbh_schedule(u32 cpu, u32 vector, axbh_handler_t handler, void *context,
                  bool_t repeat)
{
    axbh_cpu_t *target = axbh_cpu(cpu);
    axbh_task_t *slot;
    if (target == NULL_PTR || handler == NULL_PTR || vector >= AXBH_VECTORS)
        return 0;
    if (!axbh_state.enabled)
        return 0;
    if (target->count >= AXBH_QUEUE_DEPTH)
    {
        target->dropped++;
        return 0;
    }
    slot = &target->queue[target->tail];
    slot->handler = handler;
    slot->context = context;
    slot->vector = vector;
    slot->stamp = axbh_state.stamp++;
    slot->pending = true;
    slot->repeat = repeat;
    target->tail = (target->tail + 1) % AXBH_QUEUE_DEPTH;
    target->count++;
    target->pending_mask |= (1u << vector);
    axbh_state.total_deferred++;
    return slot->stamp;
}

u32 axbh_raise(u32 cpu, u32 vector)
{
    axbh_cpu_t *target = axbh_cpu(cpu);
    if (target == NULL_PTR || vector >= AXBH_VECTORS)
        return 0;
    target->pending_mask |= (1u << vector);
    return target->pending_mask;
}

u32 axbh_drain(u32 cpu)
{
    axbh_cpu_t *target = axbh_cpu(cpu);
    u32 budget = AXBH_BUDGET;
    u32 processed = 0;
    if (target == NULL_PTR)
        return 0;
    if (!axbh_state.enabled)
        return 0;
    if (target->draining)
    {
        target->overruns++;
        return 0;
    }
    target->draining = true;
    while (budget > 0 && target->count > 0)
    {
        axbh_task_t *slot = &target->queue[target->head];
        target->head = (target->head + 1) % AXBH_QUEUE_DEPTH;
        target->count--;
        if (slot->pending)
        {
            slot->pending = false;
            slot->handler(slot->vector, slot->context);
            processed++;
            target->drained++;
            axbh_state.total_drained++;
        }
        if (slot->repeat && target->count < AXBH_QUEUE_DEPTH)
        {
            axbh_task_t *again = &target->queue[target->tail];
            again->handler = slot->handler;
            again->context = slot->context;
            again->vector = slot->vector;
            again->stamp = axbh_state.stamp++;
            again->pending = true;
            again->repeat = true;
            target->tail = (target->tail + 1) % AXBH_QUEUE_DEPTH;
            target->count++;
        }
        slot->handler = NULL_PTR;
        budget--;
    }
    if (target->count == 0)
        target->pending_mask = 0;
    target->draining = false;
    return processed;
}

u32 axbh_pending(u32 cpu)
{
    axbh_cpu_t *target = axbh_cpu(cpu);
    return target == NULL_PTR ? 0 : target->count;
}

u32 axbh_pending_mask(u32 cpu)
{
    axbh_cpu_t *target = axbh_cpu(cpu);
    return target == NULL_PTR ? 0 : target->pending_mask;
}

void axbh_report(void)
{
    u32 i;
    printk("axbh: cpus=%u enabled=%u drained=%u deferred=%u\n",
           (unsigned)axbh_state.cpu_count, (unsigned)axbh_state.enabled,
           (unsigned)axbh_state.total_drained, (unsigned)axbh_state.total_deferred);
    for (i = 0; i < axbh_state.cpu_count; i++)
    {
        const axbh_cpu_t *cpu = &axbh_state.cpus[i];
        printk("  cpu %u: queued=%u drained=%u dropped=%u mask=0x%x\n", (unsigned)i,
               (unsigned)cpu->count, (unsigned)cpu->drained, (unsigned)cpu->dropped,
               (unsigned)cpu->pending_mask);
    }
}

/* ------------------------------------------------------------ selftest hooks */

static u32 axbh_test_runs;

static void axbh_test_handler(u32 vector, void *context)
{
    AXIOM_UNUSED_VAR(context);
    if (vector < AXBH_VECTORS)
        axbh_test_runs++;
}

bool_t axbh_selftest(void)
{
    u32 stamp;
    axbh_init();
    axbh_set_cpu_count(2);
    if (axbh_state.cpu_count != 2)
        return false;
    axbh_test_runs = 0;
    stamp = axbh_schedule(0, 3, axbh_test_handler, NULL_PTR, false);
    if (stamp == 0)
        return false;
    if (axbh_pending(0) != 1)
        return false;
    if (axbh_drain(0) != 1)
        return false;
    if (axbh_test_runs != 1)
        return false;
    if (axbh_pending(0) != 0)
        return false;
    axbh_schedule(1, 5, axbh_test_handler, NULL_PTR, false);
    if (axbh_pending_mask(1) == 0)
        return false;
    axbh_drain(1);
    if (axbh_test_runs != 2)
        return false;
    if (!axbh_enable(false))
        return true;
    stamp = axbh_schedule(0, 1, axbh_test_handler, NULL_PTR, false);
    if (stamp != 0)
        return false;
    axbh_enable(true);
    return true;
}
