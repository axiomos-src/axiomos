/* AxiomOS - thread scheduler.
 *
 * Phase 1 scheduler: a round-robin run queue over kernel threads, switched
 * cooperatively (thread_yield) and at explicit block points.  The timer
 * interrupt records the desired reschedule but does not switch contexts from
 * the interrupt handler itself, because switching inside an IRQ frame
 * requires saving the full interrupt frame including SSE state, which is on
 * the roadmap (docs/sched.md) rather than in this version.
 *
 * Threads created here are kernel threads running in ring 0.  User processes
 * are built on top of the same structure by syscall.c, which fills in the
 * ring-3 return frame instead of the trampoline return address.
 */

#include <axiom/kernel.h>

#define MAX_THREADS        128
#define THREAD_STACK_SIZE  (32 * 1024)
#define THREAD_QUANTUM     10      /* ticks */

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD
};

struct thread {
    u64          rsp;
    u64          id;
    u64          stack_base;
    u64          stack_size;
    void       (*entry)(void *);
    void        *arg;
    u8           state;
    u8           ring;
    char         name[32];
    u64          cpu_ticks;
    u64          wake_at_tick;
    u64          cr3;
};

static struct thread g_threads[MAX_THREADS];
static u32  g_thread_count;
static u32  g_current;
static u32  g_next_id = 1;
static u32  g_quantum_left;
static bool_t g_sched_ready;
static u32  g_switch_count;

extern void context_switch(void **save_rsp, void *new_rsp);
extern void thread_bootstrap_trampoline(void);

/* The trampoline needs to know which function to call: it is set immediately
 * before the first switch into a fresh thread. */
static struct thread *g_starting_thread;

static void thread_start_stub(void)
{
    struct thread *t = g_starting_thread;
    if (!t) {
        thread_exit();
    }
    t->entry(t->arg);
    thread_exit();
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */

void sched_init(void)
{
    for (u32 i = 0; i < MAX_THREADS; i++) {
        g_threads[i].state = THREAD_UNUSED;
        g_threads[i].id = 0;
    }

    /* Thread 0 is the boot context, already running on the loader stack. */
    struct thread *boot = &g_threads[0];
    boot->rsp = 0;
    boot->id = 0;
    boot->state = THREAD_RUNNING;
    boot->ring = 0;
    boot->stack_base = 0;
    boot->stack_size = 0;
    boot->cr3 = vmm_kernel_pml4();
    boot->entry = NULL_PTR;
    boot->arg = NULL_PTR;
    memcpy(boot->name, "kernel-main", 12);

    g_current = 0;
    g_thread_count = 1;
    g_next_id = 1;
    g_quantum_left = THREAD_QUANTUM;
    g_switch_count = 0;
    g_sched_ready = true;

    printk("sched: round-robin, %u tick quantum, max %u threads\n",
           (u64)THREAD_QUANTUM, (u64)MAX_THREADS);
}

/* ------------------------------------------------------------------ */
/* Creation                                                           */
/* ------------------------------------------------------------------ */

static u32 find_free_slot(void)
{
    for (u32 i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].state == THREAD_UNUSED || g_threads[i].state == THREAD_DEAD)
            return i;
    }
    return MAX_THREADS;
}

u64 thread_create(void (*entry)(void *), void *arg, const char *name)
{
    if (!g_sched_ready)
        return 0;

    u32 slot = find_free_slot();
    if (slot >= MAX_THREADS) {
        printk("sched: out of thread slots\n");
        return 0;
    }

    struct thread *t = &g_threads[slot];
    memset(t, 0, sizeof(*t));

    u64 pages = THREAD_STACK_SIZE / AXIOM_PAGE_SIZE;
    u64 stack = pmm_alloc_pages(pages);
    if (!stack)
        return 0;

    t->stack_base = stack;
    t->stack_size = THREAD_STACK_SIZE;
    t->entry = entry;
    t->arg = arg;
    t->id = g_next_id++;
    t->state = THREAD_READY;
    t->ring = 0;
    t->cr3 = vmm_kernel_pml4();
    t->cpu_ticks = 0;
    {
        u32 n = 0;
        while (name && name[n] && n < 31) {
            t->name[n] = name[n];
            n++;
        }
        t->name[n] = 0;
    }

    /* Build the initial context: the stack layout expected by
     * context_switch's restore path. */
    {
        u64 *sp = (u64 *)(stack + THREAD_STACK_SIZE);
        sp = (u64 *)(((u64)sp) & ~0xFULL);
        *--sp = 0;                          /* fake return address */
        *--sp = (u64)thread_bootstrap_trampoline;
        *--sp = 0x202;                      /* rflags: IF set         */
        *--sp = 0;                          /* r15 */
        *--sp = 0;                          /* r14 */
        *--sp = 0;                          /* r13 */
        *--sp = 0;                          /* r12 */
        *--sp = 0;                          /* rbx */
        *--sp = 0;                          /* rbp */
        t->rsp = (u64)sp;
    }

    g_thread_count++;
    printk("sched: thread %u '%s' created (stack %p, entry %p)\n", t->id,
           t->name, (void *)stack, (void *)entry);
    return t->id;
}

/* ------------------------------------------------------------------ */
/* Switching                                                          */
/* ------------------------------------------------------------------ */

static u32 pick_next(void)
{
    for (u32 step = 1; step <= MAX_THREADS; step++) {
        u32 candidate = (g_current + step) % MAX_THREADS;
        if (g_threads[candidate].state == THREAD_READY)
            return candidate;
    }
    return g_current;
}

void thread_yield(void)
{
    if (!g_sched_ready)
        return;

    u32 next = pick_next();
    if (next == g_current) {
        g_quantum_left = THREAD_QUANTUM;
        return;
    }

    struct thread *cur = &g_threads[g_current];
    struct thread *nxt = &g_threads[next];

    if (cur->state == THREAD_RUNNING)
        cur->state = THREAD_READY;
    nxt->state = THREAD_RUNNING;
    g_starting_thread = nxt;
    g_current = next;
    g_quantum_left = THREAD_QUANTUM;
    g_switch_count++;

    /* Switch kernel page tables if the target has its own address space. */
    if (nxt->cr3 && nxt->cr3 != read_cr3())
        write_cr3(nxt->cr3);

    context_switch(&cur->rsp, (void *)nxt->rsp);
}

void sched_tick(void)
{
    if (!g_sched_ready)
        return;
    g_quantum_left = (g_quantum_left > 0) ? g_quantum_left - 1 : 0;
    g_threads[g_current].cpu_ticks++;
    /* Preemption is applied at the next explicit yield point; see the file
     * header for the rationale. */
}

void thread_exit(void)
{
    struct thread *t = &g_threads[g_current];
    t->state = THREAD_DEAD;
    if (t->stack_base)
        pmm_free_pages(t->stack_base, t->stack_size / AXIOM_PAGE_SIZE);
    t->stack_base = 0;

    /* Wake the main thread so that the kernel keeps running. */
    g_threads[0].state = THREAD_READY;
    thread_yield();
    for (;;)
        hlt();
}

u64 sched_current_id(void)
{
    return g_threads[g_current].id;
}

const char *sched_current_name(void)
{
    return g_threads[g_current].name;
}

u32 sched_thread_count(void)
{
    return g_thread_count;
}

void sched_dump(void)
{
    printk("sched: %u threads, current %u '%s', %u switches\n", g_thread_count,
           g_current, g_threads[g_current].name, g_switch_count);
    for (u32 i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &g_threads[i];
        if (t->state == THREAD_UNUSED)
            continue;
        const char *state = "?";
        switch (t->state) {
        case THREAD_READY:   state = "ready";   break;
        case THREAD_RUNNING: state = "running"; break;
        case THREAD_BLOCKED: state = "blocked"; break;
        case THREAD_DEAD:    state = "dead";    break;
        default: break;
        }
        printk("  [%u] id %u '%s' %s ticks %u stack %p\n", (u64)i, t->id,
               t->name, state, t->cpu_ticks, (void *)t->stack_base);
    }
}

/* Sleep support used by drivers waiting for hardware without spinning. */
void thread_sleep_ticks(u64 ticks)
{
    struct thread *t = &g_threads[g_current];
    t->wake_at_tick = timer_ticks() + ticks;
    t->state = THREAD_BLOCKED;
    thread_yield();
    t->state = THREAD_RUNNING;
}

void sched_wake_sleepers(void)
{
    u64 now = timer_ticks();
    for (u32 i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &g_threads[i];
        if (t->state == THREAD_BLOCKED && t->wake_at_tick && t->wake_at_tick <= now) {
            t->wake_at_tick = 0;
            t->state = THREAD_READY;
        }
    }
}
