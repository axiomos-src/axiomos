/* AxiomOS - POSIX style signal delivery.
 *
 * Signals are kept as two bitmaps per task (pending and blocked) plus a small
 * disposition table.  Delivery happens when the task returns to user mode; the
 * kernel then rewrites the saved frame so that the handler runs with the
 * blocked mask of the interrupted context preserved.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXSG_MAX_TASKS      32
#define AXSG_MAX_SIGNALS    32
#define AXSG_MAX_ACTIONS    16
#define AXSG_ALT_STACK_SIZE 4096

#define AXSG_SIGHUP         1
#define AXSG_SIGINT         2
#define AXSG_SIGQUIT        3
#define AXSG_SIGILL         4
#define AXSG_SIGTRAP        5
#define AXSG_SIGABRT        6
#define AXSG_SIGBUS         7
#define AXSG_SIGFPE         8
#define AXSG_SIGKILL        9
#define AXSG_SIGUSR1        10
#define AXSG_SIGSEGV        11
#define AXSG_SIGUSR2        12
#define AXSG_SIGPIPE        13
#define AXSG_SIGALRM        14
#define AXSG_SIGTERM        15
#define AXSG_SIGCHLD        17
#define AXSG_SIGCONT        18
#define AXSG_SIGSTOP        19
#define AXSG_SIGTSTP        20
#define AXSG_SIGTTIN        21
#define AXSG_SIGTTOU        22
#define AXSG_SIGURG         23
#define AXSG_SIGXCPU        24
#define AXSG_SIGXFSZ        25
#define AXSG_SIGVTALRM      26
#define AXSG_SIGPROF        27
#define AXSG_SIGWINCH       28
#define AXSG_SIGSYS         31

#define AXSG_FLAG_IGNORE      0x0001
#define AXSG_FLAG_ON_STACK    0x0002
#define AXSG_FLAG_RESTART     0x0004
#define AXSG_FLAG_UNBLOCKABLE 0x0008

typedef struct axsg_action
{
    u64  handler;
    u64  restorer;
    u64  mask;
    u32  flags;
    bool_t used;
} axsg_action_t;

typedef struct axsg_task
{
    u32  id;
    u32  pid;
    u32  pending;
    u32  blocked;
    u32  suspended;
    u32  delivered;
    u32  discarded;
    u32  faults;
    u64  alt_stack;
    u16  alt_stack_size;
    u8   stop_signal;
    bool_t sleeping;
    bool_t stopped;
    axsg_action_t actions[AXSG_MAX_ACTIONS];
} axsg_task_t;

typedef struct axsg_state
{
    axsg_task_t tasks[AXSG_MAX_TASKS];
    u32  count;
    u32  queue_stamp;
    u32  wakeups;
    u32  default_terminations;
    u32  default_stops;
    bool_t ready;
} axsg_state_t;

static axsg_state_t axsg_state;

static void axsg_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static bool_t axsg_valid(u32 signal)
{
    return signal >= 1 && signal < AXSG_MAX_SIGNALS;
}

static axsg_task_t *axsg_find(u32 task)
{
    if (task >= axsg_state.count)
        return NULL_PTR;
    return &axsg_state.tasks[task];
}

static axsg_action_t *axsg_action(u32 task, u32 signal)
{
    axsg_task_t *entry = axsg_find(task);
    if (entry == NULL_PTR || signal == 0 || signal >= AXSG_MAX_ACTIONS)
        return NULL_PTR;
    if (!entry->actions[signal].used)
        return NULL_PTR;
    return &entry->actions[signal];
}

static bool_t axsg_catchable(u32 signal)
{
    return signal != AXSG_SIGKILL && signal != AXSG_SIGSTOP;
}

static void axsg_default(u32 task, u32 signal)
{
    axsg_task_t *entry = axsg_find(task);
    if (entry == NULL_PTR)
        return;
    switch (signal)
    {
    case AXSG_SIGSTOP:
    case AXSG_SIGTSTP:
    case AXSG_SIGTTIN:
    case AXSG_SIGTTOU:
        entry->stopped = true;
        entry->stop_signal = (u8)signal;
        axsg_state.default_stops++;
        break;
    case AXSG_SIGCHLD:
    case AXSG_SIGCONT:
    case AXSG_SIGURG:
    case AXSG_SIGWINCH:
        entry->discarded++;
        break;
    default:
        entry->sleeping = false;
        axsg_state.default_terminations++;
        break;
    }
}

/* -------------------------------------------------------------- public API */

void axsg_init(void)
{
    axsg_zero(&axsg_state, (u32)sizeof(axsg_state));
    axsg_state.ready = true;
}

u32 axsg_add_task(u32 pid)
{
    axsg_task_t *task;
    if (axsg_state.count >= AXSG_MAX_TASKS)
        return 0xFFFFFFFFu;
    task = &axsg_state.tasks[axsg_state.count];
    axsg_zero(task, (u32)sizeof(*task));
    task->id = axsg_state.count;
    task->pid = pid;
    axsg_state.count++;
    return task->id;
    }

bool_t axsg_set_action(u32 task, u32 signal, u64 handler, u64 restorer, u64 mask,
                       u32 flags)
{
    axsg_task_t *entry = axsg_find(task);
    axsg_action_t *action;
    if (entry == NULL_PTR || !axsg_valid(signal) || signal >= AXSG_MAX_ACTIONS)
        return false;
    if (!axsg_catchable(signal) && handler != 0)
        return false;
    action = &entry->actions[signal];
    action->handler = handler;
    action->restorer = restorer;
    action->mask = mask;
    action->flags = flags;
    action->used = true;
    return true;
}

bool_t axsg_block(u32 task, u32 mask)
{
    axsg_task_t *entry = axsg_find(task);
    if (entry == NULL_PTR)
        return false;
    entry->blocked |= mask;
    entry->blocked &= ~((1u << AXSG_SIGKILL) | (1u << AXSG_SIGSTOP));
    return true;
}

bool_t axsg_unblock(u32 task, u32 mask)
{
    axsg_task_t *entry = axsg_find(task);
    if (entry == NULL_PTR)
        return false;
    entry->blocked &= ~mask;
    return true;
}

u32 axsg_pending(u32 task)
{
    axsg_task_t *entry = axsg_find(task);
    return entry == NULL_PTR ? 0 : entry->pending;
}

bool_t axsg_raise(u32 task, u32 signal)
{
    axsg_task_t *entry = axsg_find(task);
    if (entry == NULL_PTR || !axsg_valid(signal))
        return false;
    entry->pending |= (1u << signal);
    axsg_state.queue_stamp++;
    if (!axsg_catchable(signal))
    {
        if (signal == AXSG_SIGKILL)
        {
            entry->pending = 0;
            entry->sleeping = false;
            axsg_state.default_terminations++;
        }
    }
    return true;
}

u32 axsg_deliver(u32 task)
{
    axsg_task_t *entry = axsg_find(task);
    u32 deliverable;
    u32 signal;
    if (entry == NULL_PTR)
        return 0;
    deliverable = entry->pending & ~entry->blocked;
    if (deliverable == 0)
        return 0;
    signal = 0;
    for (u32 i = 1; i < AXSG_MAX_SIGNALS; i++)
    {
        if (deliverable & (1u << i))
        {
            signal = i;
            break;
        }
    }
    if (signal == 0)
        return 0;
    entry->pending &= ~(1u << signal);
    entry->delivered++;
    if (signal == AXSG_SIGKILL)
    {
        axsg_state.default_terminations++;
        return signal;
    }
    {
        axsg_action_t *action = axsg_action(task, signal);
        if (action == NULL_PTR)
            axsg_default(task, signal);
        else if (action->handler == 1)
            entry->discarded++;
        else if (action->flags & AXSG_FLAG_ON_STACK)
        {
            if (entry->alt_stack != 0)
                entry->blocked |= (u32)action->mask;
        }
        else
        {
            entry->blocked |= (u32)action->mask;
        }
    }
    axsg_state.wakeups++;
    return signal;
}

u32 axsg_deliver_all(u32 task)
{
    u32 delivered = 0;
    u32 rounds = 0;
    while (rounds++ < AXSG_MAX_SIGNALS)
    {
        u32 signal = axsg_deliver(task);
        if (signal == 0)
            break;
        delivered++;
    }
    return delivered;
}

bool_t axsg_ignore(u32 task, u32 signal)
{
    return axsg_set_action(task, signal, 1, 0, 0, AXSG_FLAG_IGNORE);
}

bool_t axsg_default_action(u32 task, u32 signal)
{
    axsg_task_t *entry = axsg_find(task);
    if (entry == NULL_PTR || signal >= AXSG_MAX_ACTIONS)
        return false;
    entry->actions[signal].used = false;
    return true;
}

bool_t axsg_alt_stack(u32 task, u64 address, u16 size)
{
    axsg_task_t *entry = axsg_find(task);
    if (entry == NULL_PTR || size > AXSG_ALT_STACK_SIZE)
        return false;
    entry->alt_stack = address;
    entry->alt_stack_size = size;
    return true;
}

bool_t axsg_continue(u32 task)
{
    axsg_task_t *entry = axsg_find(task);
    if (entry == NULL_PTR)
        return false;
    entry->stopped = false;
    entry->pending |= (1u << AXSG_SIGCONT);
    return true;
}

bool_t axsg_is_stopped(u32 task)
{
    axsg_task_t *entry = axsg_find(task);
    return entry == NULL_PTR ? false : entry->stopped;
}

u32 axsg_delivered(u32 task)
{
    axsg_task_t *entry = axsg_find(task);
    return entry == NULL_PTR ? 0 : entry->delivered;
}

void axsg_virtual_timer(u32 task)
{
    axsg_raise(task, AXSG_SIGVTALRM);
}

void axsg_report(void)
{
    u32 i;
    printk("axsg: tasks=%u wakeups=%u terminations=%u stops=%u\n",
           (unsigned)axsg_state.count, (unsigned)axsg_state.wakeups,
           (unsigned)axsg_state.default_terminations,
           (unsigned)axsg_state.default_stops);
    for (i = 0; i < axsg_state.count; i++)
    {
        const axsg_task_t *task = &axsg_state.tasks[i];
        printk("  task %u pid=%u pending=0x%x blocked=0x%x delivered=%u stopped=%u\n",
               (unsigned)task->id, (unsigned)task->pid, (unsigned)task->pending,
               (unsigned)task->blocked, (unsigned)task->delivered,
               (unsigned)task->stopped);
    }
}

bool_t axsg_selftest(void)
{
    u32 task;
    u32 signal;
    axsg_init();
    task = axsg_add_task(100);
    if (task != 0)
        return false;
    if (axsg_set_action(task, AXSG_SIGUSR1, 0x400000, 0x401000, 0, 0) != true)
        return false;
    if (!axsg_block(task, 1u << AXSG_SIGUSR1))
        return false;
    axsg_raise(task, AXSG_SIGUSR1);
    if (axsg_pending(task) != (1u << AXSG_SIGUSR1))
        return false;
    signal = axsg_deliver(task);
    if (signal != 0)
        return false;
    axsg_unblock(task, 1u << AXSG_SIGUSR1);
    signal = axsg_deliver(task);
    if (signal != AXSG_SIGUSR1)
        return false;
    if (axsg_delivered(task) != 1)
        return false;
    axsg_raise(task, AXSG_SIGKILL);
    if (axsg_pending(task) != 0)
        return false;
    if (axsg_state.default_terminations != 1)
        return false;
    if (axsg_set_action(task, AXSG_SIGKILL, 0x400000, 0, 0, 0))
        return false;
    if (!axsg_ignore(task, AXSG_SIGCHLD))
        return false;
    axsg_raise(task, AXSG_SIGCHLD);
    axsg_deliver(task);
    if (axsg_state.tasks[task].discarded != 1)
        return false;
    if (!axsg_continue(task))
        return false;
    if (!axsg_alt_stack(task, 0x700000, AXSG_ALT_STACK_SIZE))
        return false;
    if (!axsg_block(task, 1u << AXSG_SIGSTOP))
        return false;
    if (axsg_state.tasks[task].blocked & (1u << AXSG_SIGSTOP))
        return false;
    return true;
}

