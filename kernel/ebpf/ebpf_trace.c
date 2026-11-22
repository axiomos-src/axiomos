/*
 * AxiomOS - eBPF tracepoints and event plumbing.
 *
 * Tracepoints are named hooks that a program can attach to.  When the kernel
 * reaches a hook it calls `axbpf_trace_fire`, which builds an event, hands it to
 * every attached program and then to an optional sink (the shell's event ring,
 * or a log).  Hooks are static strings so a lookup is a strcmp over a tiny
 * table; the table is sized for the number of hooks AxiomOS actually has.
 *
 * The event record is the contract between the firing site and a program: six
 * u64 arguments mirroring the SysV registers at the probe point, a timestamp,
 * and the CPU index.  Programs read those fields from the context pointer with
 * normal load instructions, which keeps the interpreter and JIT uniform.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>

#define AXBPF_MAX_HOOKS 32

typedef struct trace_hook {
    u32  kind;
    char name[AXBPF_MAX_NAME];
    i32  prog_fd;
    u64  fires;
    u64  total_ns;
    bool_t active;
} trace_hook_t;

static trace_hook_t g_hooks[AXBPF_MAX_HOOKS];
static u32 g_hook_count;
static u64 g_total_events;
static axbpf_trace_sink_t *g_sink;

/* A bounded event history so `bpf events` can show recent activity without a
 * ring buffer allocation at boot. */
#define AXBPF_EVENT_HISTORY 64
static axbpf_trace_event_t g_history[AXBPF_EVENT_HISTORY];
static u32 g_history_head;
static u32 g_history_count;

void axbpf_trace_init(void)
{
    g_hook_count = 0;
    g_total_events = 0;
    g_sink = 0;
    g_history_head = 0;
    g_history_count = 0;
    ax_memset(g_hooks, 0, sizeof(g_hooks));
    ax_memset(g_history, 0, sizeof(g_history));
}

void axbpf_trace_sink_set(axbpf_trace_sink_t *sink)
{
    g_sink = sink;
}

/* Register a program for a hook, creating the hook on first use. */
i32 axbpf_trace_register(u32 kind, const char *name, axbpf_prog_t *prog)
{
    if (!name || !prog) {
        return -22;
    }
    for (u32 i = 0; i < g_hook_count; i++) {
        if (ax_strcmp(g_hooks[i].name, name) == 0 && g_hooks[i].kind == kind) {
            g_hooks[i].prog_fd = prog->fd;
            g_hooks[i].active = (bool_t)1;
            return 0;
        }
    }
    if (g_hook_count >= AXBPF_MAX_HOOKS) {
        return -28;
    }
    trace_hook_t *hook = &g_hooks[g_hook_count++];
    ax_strlcpy(hook->name, name, sizeof(hook->name));
    hook->kind = kind;
    hook->prog_fd = prog->fd;
    hook->active = (bool_t)1;
    hook->fires = 0;
    hook->total_ns = 0;
    printk("bpf: tracepoint %s registered for %s\n", name, prog->name);
    return 0;
}

i32 axbpf_trace_unregister(u32 kind, const char *name)
{
    if (!name) {
        return -22;
    }
    for (u32 i = 0; i < g_hook_count; i++) {
        if (ax_strcmp(g_hooks[i].name, name) == 0 && g_hooks[i].kind == kind) {
            g_hooks[i].active = (bool_t)0;
            g_hooks[i].prog_fd = 0;
            return 0;
        }
    }
    return -2;
}

/* Record an event in the bounded history. */
static void history_push(const axbpf_trace_event_t *event)
{
    g_history[g_history_head] = *event;
    g_history_head = (g_history_head + 1) % AXBPF_EVENT_HISTORY;
    if (g_history_count < AXBPF_EVENT_HISTORY) {
        g_history_count++;
    }
}

void axbpf_trace_emit(const axbpf_trace_event_t *event)
{
    if (!event) {
        return;
    }
    history_push(event);
    g_total_events++;
    if (g_sink && g_sink->emit) {
        g_sink->emit(event);
    }
}

/* Fire a hook: build the event, run programs, then publish it downstream. */
void axbpf_trace_fire(u32 kind, const char *name, u64 a0, u64 a1, u64 a2)
{
    if (!name) {
        return;
    }
    trace_hook_t *hook = 0;
    for (u32 i = 0; i < g_hook_count; i++) {
        if (g_hooks[i].active && g_hooks[i].kind == kind &&
            ax_strcmp(g_hooks[i].name, name) == 0) {
            hook = &g_hooks[i];
            break;
        }
    }
    if (!hook) {
        return;
    }
    hook->fires++;

    axbpf_trace_event_t event;
    ax_memset(&event, 0, sizeof(event));
    event.timestamp_ns = axbpf_now_ns();
    event.kind = kind;
    event.cpu = 0;
    event.pid = 1;
    event.args[0] = a0;
    event.args[1] = a1;
    event.args[2] = a2;
    event.data_len = sizeof(event.args);

    u64 start = event.timestamp_ns;
    axbpf_run_ctx_t ctx;
    ax_memset(&ctx, 0, sizeof(ctx));
    ctx.arg0 = a0;
    ctx.arg1 = a1;
    ctx.arg2 = a2;
    ctx.data = &event;
    ctx.data_len = sizeof(event);
    ctx.start_ns = start;

    if (hook->prog_fd > 0) {
        /* The dispatch table matches on target name; the trace layer registers
         * the hook name as the target when the program attaches. */
        axbpf_prog_dispatch((void *)hook->name, &ctx);
    }
    u64 elapsed = axbpf_now_ns() - start;
    hook->total_ns += elapsed;

    axbpf_trace_emit(&event);
}

u64 axbpf_trace_count(void)
{
    return g_total_events;
}

void axbpf_trace_dump(void)
{
    printk("bpf: tracepoints (%u), events=%llu\n", g_hook_count, g_total_events);
    for (u32 i = 0; i < g_hook_count; i++) {
        trace_hook_t *hook = &g_hooks[i];
        printk("  %-16s kind=%u fd=%d fires=%llu avg_ns=%llu%s\n", hook->name,
               hook->kind, hook->prog_fd, hook->fires,
               hook->fires ? hook->total_ns / hook->fires : 0,
               hook->active ? "" : " (off)");
    }
    if (g_history_count) {
        printk("bpf: recent events\n");
        u32 start = (g_history_head + AXBPF_EVENT_HISTORY - g_history_count) %
                    AXBPF_EVENT_HISTORY;
        for (u32 i = 0; i < g_history_count; i++) {
            const axbpf_trace_event_t *event =
                &g_history[(start + i) % AXBPF_EVENT_HISTORY];
            printk("  t=%llu kind=%u pid=%u a0=%llx a1=%llx a2=%llx\n",
                   event->timestamp_ns, event->kind, event->pid, event->args[0],
                   event->args[1], event->args[2]);
        }
    }
}

/* Fetch the newest event; returns false when the history is empty. */
bool_t axbpf_trace_latest(axbpf_trace_event_t *out)
{
    if (!out || g_history_count == 0) {
        return (bool_t)0;
    }
    u32 index = (g_history_head + AXBPF_EVENT_HISTORY - 1) % AXBPF_EVENT_HISTORY;
    *out = g_history[index];
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Built-in probe points                                               */
/* ------------------------------------------------------------------ */

/* The kernel calls these from the corresponding sites.  Each has a stable name
 * that programs attach to by string. */
void axbpf_probe_syscall(u32 number, u64 arg0, u64 arg1)
{
    axbpf_trace_fire(AXBPF_TRACE_SYSCALL, "sys_enter", (u64)number, arg0, arg1);
}

void axbpf_probe_schedule(u32 previous_pid, u32 next_pid)
{
    axbpf_trace_fire(AXBPF_TRACE_SCHED, "sched_switch", (u64)previous_pid,
                     (u64)next_pid, 0);
}

void axbpf_probe_page_fault(u64 address, u64 error)
{
    axbpf_trace_fire(AXBPF_TRACE_KPROBE, "page_fault", address, error, 0);
}

void axbpf_probe_file_open(const char *path, u32 flags, u32 result)
{
    axbpf_trace_fire(AXBPF_TRACE_KPROBE, "file_open", (u64)path, (u64)flags,
                     (u64)result);
}

/* ------------------------------------------------------------------ */
/* Self test                                                           */
/* ------------------------------------------------------------------ */

/* Exercises the trace registry, a program run and the event history. */
i32 axbpf_trace_selftest(void)
{
    axbpf_map_def_t def;
    ax_memset(&def, 0, sizeof(def));
    def.type = AXBPF_MAP_HASH;
    def.key_size = 4;
    def.value_size = 8;
    def.max_entries = 32;
    i32 map_fd = axbpf_map_create(&def, "trace_hits");
    if (map_fd < 0) {
        return map_fd;
    }

    static axbpf_insn_t program[5];
    ax_memset(program, 0, sizeof(program));
    /* r0 = ctx->arg0; exit */
    program[0].code = (u8)(AXBPF_LDX | AXBPF_MODE_MEM | AXBPF_SIZE_DW);
    program[0].dst_src = 0x10;
    program[0].off = 0;
    program[0].imm = 0;
    program[1].code = (u8)(AXBPF_JMP | AXBPF_EXIT);

    i32 fd = axbpf_prog_load("trace_probe", AXBPF_PROG_TRACEPOINT, program, 2, 0);
    if (fd < 0) {
        axbpf_map_destroy(map_fd);
        return fd;
    }
    axbpf_prog_attach(fd, AXBPF_ATTACH_ENTRY, "selftest_hook");
    axbpf_trace_fire(AXBPF_TRACE_KPROBE, "selftest_hook", 0x11, 0x22, 0x33);
    axbpf_trace_fire(AXBPF_TRACE_KPROBE, "selftest_hook", 0x44, 0x55, 0x66);

    printk("bpf: trace selftest fired %llu events\n", axbpf_trace_count());
    axbpf_prog_detach(fd);
    axbpf_map_destroy(map_fd);
    return axbpf_trace_count() > 0 ? 0 : -1;
}
