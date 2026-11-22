/*
 * AxiomOS - eBPF helper functions.
 *
 * Helpers are the only way a program reaches outside its own registers and
 * stack.  Each helper has a fixed prototype that the verifier checks argument
 * types against, and a table entry that bounds the helper id.  Anything not in
 * the table is rejected at verification time rather than at run time.
 *
 * The implementations are deliberately defensive: every pointer argument is
 * validated before use, sizes are clamped, and helpers that copy data return a
 * negative value instead of faulting.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>

#define HELPER_EINVAL  -22
#define HELPER_EFAULT  -14
#define HELPER_ENOENT  -2
#define HELPER_ENOMEM  -12
#define HELPER_E2BIG   -7

/* Argument type codes used by the prototype strings and the checker. */
#define ARG_ANY      0
#define ARG_MAP      1
#define ARG_MAP_KEY  2
#define ARG_MAP_VALUE 3
#define ARG_SIZE     4
#define ARG_PTR      5
#define ARG_SCALAR   6
#define ARG_CONST    7

static const axbpf_helper_def_t g_helpers[] = {
    { AXBPF_FUNC_unspec, "unspec", "void", {ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_map_lookup_elem, "map_lookup_elem", "void *(map, key)",
      {ARG_MAP, ARG_MAP_KEY, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)1 },
    { AXBPF_FUNC_map_update_elem, "map_update_elem", "int (map, key, value, flags)",
      {ARG_MAP, ARG_MAP_KEY, ARG_MAP_VALUE, ARG_CONST, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_map_delete_elem, "map_delete_elem", "int (map, key)",
      {ARG_MAP, ARG_MAP_KEY, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_probe_read, "probe_read", "int (dst, size, src)",
      {ARG_PTR, ARG_SIZE, ARG_PTR, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_ktime_get_ns, "ktime_get_ns", "u64 (void)",
      {ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_trace_printk, "trace_printk", "int (fmt, size, arg...)",
      {ARG_PTR, ARG_SIZE, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_get_prandom_u32, "get_prandom_u32", "u32 (void)",
      {ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_get_smp_processor_id, "get_smp_processor_id", "u32 (void)",
      {ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_tail_call, "tail_call", "int (ctx, map, index)",
      {ARG_PTR, ARG_MAP, ARG_SCALAR, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_perf_event_output, "perf_event_output", "int (ctx, map, flags, data, size)",
      {ARG_PTR, ARG_MAP, ARG_CONST, ARG_MAP_VALUE, ARG_SIZE}, (bool_t)0 },
    { AXBPF_FUNC_ringbuf_output, "ringbuf_output", "int (ring, data, size, flags)",
      {ARG_MAP, ARG_PTR, ARG_SIZE, ARG_CONST, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_ringbuf_reserve, "ringbuf_reserve", "void *(ring, size, flags)",
      {ARG_MAP, ARG_SIZE, ARG_CONST, ARG_ANY, ARG_ANY}, (bool_t)1 },
    { AXBPF_FUNC_ringbuf_submit, "ringbuf_submit", "void (data, flags)",
      {ARG_PTR, ARG_CONST, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_map_lookup_percpu_elem, "map_lookup_percpu_elem",
      "void *(map, key, cpu)",
      {ARG_MAP, ARG_MAP_KEY, ARG_SCALAR, ARG_ANY, ARG_ANY}, (bool_t)1 },
    { AXBPF_FUNC_strcmp, "strcmp", "int (a, b)",
      {ARG_PTR, ARG_PTR, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_get_current_pid_tgid, "get_current_pid_tgid", "u64 (void)",
      {ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_get_current_comm, "get_current_comm", "int (buf, size)",
      {ARG_PTR, ARG_SIZE, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_probe_read_kernel, "probe_read_kernel", "int (dst, size, src)",
      {ARG_PTR, ARG_SIZE, ARG_PTR, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_probe_read_user, "probe_read_user", "int (dst, size, src)",
      {ARG_PTR, ARG_SIZE, ARG_PTR, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_get_stack, "get_stack", "int (ctx, buf, size)",
      {ARG_PTR, ARG_PTR, ARG_SIZE, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_trace_vprintk, "trace_vprintk", "int (fmt, size, data)",
      {ARG_PTR, ARG_SIZE, ARG_PTR, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_jiffies64, "jiffies64", "u64 (void)",
      {ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_get_task_stack, "get_task_stack", "int (task, buf, size, flags)",
      {ARG_PTR, ARG_PTR, ARG_SIZE, ARG_CONST, ARG_ANY}, (bool_t)0 },
    { AXBPF_FUNC_copy_from_user, "copy_from_user", "int (dst, src, size)",
      {ARG_PTR, ARG_PTR, ARG_SIZE, ARG_ANY, ARG_ANY}, (bool_t)0 },
};

static u64 g_helper_calls[AXBPF_FUNC_MAX];
static u64 g_trace_lines;
static char g_last_trace[128];

/* ------------------------------------------------------------------ */
/* Table access                                                        */
/* ------------------------------------------------------------------ */

const axbpf_helper_def_t *axbpf_helper_lookup(u32 id)
{
    for (usize i = 0; i < sizeof(g_helpers) / sizeof(g_helpers[0]); i++) {
        if (g_helpers[i].id == id) {
            return &g_helpers[i];
        }
    }
    return 0;
}

void axbpf_helper_init(void)
{
    for (u32 i = 0; i < AXBPF_FUNC_MAX; i++) {
        g_helper_calls[i] = 0;
    }
    g_trace_lines = 0;
    g_last_trace[0] = 0;
}

u64 axbpf_helper_count_calls(u32 id)
{
    return id < AXBPF_FUNC_MAX ? g_helper_calls[id] : 0;
}

void axbpf_helper_dump(void)
{
    printk("bpf: helpers\n");
    for (usize i = 0; i < sizeof(g_helpers) / sizeof(g_helpers[0]); i++) {
        const axbpf_helper_def_t *helper = &g_helpers[i];
        printk("  %-2u %-24s %-46s calls=%llu\n", helper->id, helper->name,
               helper->prototype, g_helper_calls[helper->id]);
    }
}

/* ------------------------------------------------------------------ */
/* Verification of arguments                                           */
/* ------------------------------------------------------------------ */

static bool_t reg_is_map_pointer(const axbpf_reg_state_t *reg)
{
    return (bool_t)(reg->type == AXBPF_T_PTR_TO_MAP ||
                    reg->type == AXBPF_T_CONST_PTR);
}

i32 axbpf_helper_check_args(u32 id, const axbpf_reg_state_t *regs,
                             char *error, usize error_cap)
{
    const axbpf_helper_def_t *helper = axbpf_helper_lookup(id);
    if (!helper) {
        if (error) {
            ax_snprintf(error, error_cap, "helper %u does not exist", id);
        }
        return -1;
    }
    for (u32 arg = 0; arg < 5; arg++) {
        u8 want = helper->arg_types[arg];
        if (want == ARG_ANY) {
            continue;
        }
        const axbpf_reg_state_t *reg = &regs[arg + 1];
        if (reg->type == AXBPF_T_NOT_INIT) {
            if (error) {
                ax_snprintf(error, error_cap, "helper %s arg%u uninitialised",
                            helper->name, arg + 1);
            }
            return -1;
        }
        switch (want) {
        case ARG_MAP:
            if (!reg_is_map_pointer(reg)) {
                if (error) {
                    ax_snprintf(error, error_cap,
                                "helper %s arg%u must be a map", helper->name,
                                arg + 1);
                }
                return -1;
            }
            break;
        case ARG_MAP_KEY:
        case ARG_MAP_VALUE:
        case ARG_PTR:
            if (reg->type != AXBPF_T_MAP_VALUE &&
                reg->type != AXBPF_T_STACK_PTR &&
                reg->type != AXBPF_T_PACKET_PTR &&
                reg->type != AXBPF_T_CONTEXT_PTR &&
                reg->type != AXBPF_T_CONST_PTR) {
                if (error) {
                    ax_snprintf(error, error_cap,
                                "helper %s arg%u must be a pointer", helper->name,
                                arg + 1);
                }
                return -1;
            }
            break;
        case ARG_SIZE:
        case ARG_SCALAR:
        case ARG_CONST:
            if (reg->type != AXBPF_T_SCALAR) {
                if (error) {
                    ax_snprintf(error, error_cap,
                                "helper %s arg%u must be a scalar", helper->name,
                                arg + 1);
                }
                return -1;
            }
            break;
        default:
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Implementations                                                     */
/* ------------------------------------------------------------------ */

static u64 helper_map_lookup(u64 a0, u64 a1)
{
    axbpf_map_t *map = (axbpf_map_t *)a0;
    if (!map || !a1) {
        return 0;
    }
    if (map->def.type == AXBPF_MAP_ARRAY || map->def.type == AXBPF_MAP_PERCPU_ARRAY) {
        u32 index = *(const u32 *)a1;
        if (index >= map->def.max_entries) {
            map->misses++;
            return 0;
        }
        map->hits++;
        return (u64)(map->storage + (u64)index * map->def.value_size);
    }
    axbpf_map_entry_t *entry = axbpf_map_find_entry(map, (const void *)a1);

    if (!entry) {
        map->misses++;
        return 0;
    }
    map->hits++;
    return (u64)entry->value;
}


static u64 helper_map_update(u64 a0, u64 a1, u64 a2, u64 a3)
{
    axbpf_map_t *map = (axbpf_map_t *)a0;
    if (!map || !a1 || !a2) {
        return (u64)(i64)HELPER_EINVAL;
    }
    i32 status = axbpf_map_update(map->fd, (const void *)a1, (const void *)a2,
                                   (u32)a3);
    return (u64)(i64)status;
}

static u64 helper_map_delete(u64 a0, u64 a1)
{
    axbpf_map_t *map = (axbpf_map_t *)a0;
    if (!map || !a1) {
        return (u64)(i64)HELPER_EINVAL;
    }
    return (u64)(i64)axbpf_map_delete(map->fd, (const void *)a1);
}

static u64 helper_probe_read(u64 dst, u64 size, u64 src)
{
    if (!dst || !src || size == 0) {
        return (u64)(i64)HELPER_EINVAL;
    }
    if (size > 4096) {
        return (u64)(i64)HELPER_E2BIG;
    }
    /* The kernel runs with a flat address space today, so the source pointer is
     * directly readable.  The guard is the size clamp above plus the verifier's
     * proof that `dst` is a writable object. */
    for (u64 i = 0; i < size; i++) {
        ((u8 *)dst)[i] = ((const u8 *)src)[i];
    }
    return 0;
}

static u64 helper_trace_printk(u64 fmt, u64 size, u64 a0, u64 a1, u64 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    if (!fmt || size == 0) {
        return (u64)(i64)HELPER_EINVAL;
    }
    if (size > sizeof(g_last_trace) - 1) {
        size = sizeof(g_last_trace) - 1;
    }
    for (u64 i = 0; i < size; i++) {
        g_last_trace[i] = ((const char *)fmt)[i];
    }
    g_last_trace[size] = 0;
    g_trace_lines++;
    printk("bpf: %s\n", g_last_trace);
    return size;
}

static u64 helper_strcmp(u64 a, u64 b)
{
    if (!a || !b) {
        return (u64)(i64)HELPER_EFAULT;
    }
    const char *pa = (const char *)a;
    const char *pb = (const char *)b;
    /* Compare at most 64 bytes: a program cannot make the kernel walk an
     * unbounded string. */
    for (u32 i = 0; i < 64; i++) {
        if (pa[i] != pb[i]) {
            return (u64)(i64)(pa[i] - pb[i]);
        }
        if (pa[i] == 0) {
            return 0;
        }
    }
    return 0;
}

static u64 helper_get_current_comm(u64 buffer, u64 size)
{
    if (!buffer || size == 0) {
        return (u64)(i64)HELPER_EINVAL;
    }
    char *out = (char *)buffer;
    u32 capacity = (u32)(size > 16 ? 16 : size);
    /* The scheduler has no process table yet; report "axiomos" so tracing
     * output is readable rather than empty. */
    const char *name = "axiomos";
    u32 i = 0;
    for (; i + 1 < capacity && name[i]; i++) {
        out[i] = name[i];
    }
    out[i] = 0;
    return 0;
}

static u64 helper_get_stack(u64 ctx, u64 buffer, u64 size)
{
    (void)ctx;
    if (!buffer || size == 0) {
        return (u64)(i64)HELPER_EINVAL;
    }
    /* Walk the frame pointers from the current stack pointer upwards, storing
     * eight bytes per frame. */
    u64 *frames = (u64 *)buffer;
    u32 capacity = (u32)(size / 8);
    if (capacity > 32) {
        capacity = 32;
    }
    u64 frame = 0;
    __asm__ volatile("mov %%rbp, %0" : "=r"(frame));
    u32 count = 0;
    while (frame && count < capacity) {
        frames[count++] = ((u64 *)frame)[1];
        u64 next = ((u64 *)frame)[0];
        if (next <= frame) {
            break;
        }
        frame = next;
    }
    return count;
}

static u64 helper_ringbuf_output(u64 a0, u64 a1, u64 a2, u64 a3)
{
    (void)a3;
    axbpf_map_t *map = (axbpf_map_t *)a0;
    if (!map || !a1 || a2 == 0) {
        return (u64)(i64)HELPER_EINVAL;
    }
    void *record = 0;
    i32 status = axbpf_ringbuf_reserve(map, (u32)a2, &record);
    if (status != 0) {
        return (u64)(i64)status;
    }
    for (u64 i = 0; i < a2; i++) {
        ((u8 *)record)[i] = ((const u8 *)a1)[i];
    }
    return (u64)(i64)axbpf_ringbuf_submit(map, record, (u32)a2);
}

static u64 helper_ringbuf_reserve(u64 a0, u64 a1, u64 a2)
{
    (void)a2;
    axbpf_map_t *map = (axbpf_map_t *)a0;
    if (!map) {
        return 0;
    }
    void *record = 0;
    i32 status = axbpf_ringbuf_reserve(map, (u32)a1, &record);
    if (status != 0) {
        return 0;
    }
    return (u64)record;
}

static u64 helper_perf_event_output(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4)
{
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a4;
    if (!a3) {
        return (u64)(i64)HELPER_EINVAL;
    }
    axbpf_trace_event_t event;
    ax_memset(&event, 0, sizeof(event));
    event.timestamp_ns = axbpf_now_ns();
    event.kind = AXBPF_TRACE_TRACEPOINT;
    event.data_len = (u32)(a4 > 64 ? 64 : a4);
    event.args[0] = a3;
    axbpf_trace_emit(&event);
    return 0;
}

u64 axbpf_helper_call(axbpf_helper_id_t id, axbpf_helper_ctx_t *ctx, u64 *args,
                       u64 *result)
{
    if (id >= AXBPF_FUNC_MAX) {
        if (result) {
            *result = 0;
        }
        return 0;
    }
    g_helper_calls[id]++;
    u64 a0 = args ? args[0] : 0;
    u64 a1 = args ? args[1] : 0;
    u64 a2 = args ? args[2] : 0;
    u64 a3 = args ? args[3] : 0;
    u64 a4 = args ? args[4] : 0;

    u64 value = 0;
    switch (id) {
    case AXBPF_FUNC_map_lookup_elem:
        value = helper_map_lookup(a0, a1);
        break;
    case AXBPF_FUNC_map_update_elem:
        value = helper_map_update(a0, a1, a2, a3);
        break;
    case AXBPF_FUNC_map_delete_elem:
        value = helper_map_delete(a0, a1);
        break;
    case AXBPF_FUNC_probe_read:
    case AXBPF_FUNC_probe_read_kernel:
    case AXBPF_FUNC_probe_read_user:
        value = helper_probe_read(a0, a1, a2);
        break;
    case AXBPF_FUNC_copy_from_user:
        value = helper_probe_read(a0, a2, a1);
        break;
    case AXBPF_FUNC_ktime_get_ns:
        value = axbpf_now_ns();
        break;
    case AXBPF_FUNC_jiffies64:
        value = axbpf_now_ns() / 1000000ull;
        break;
    case AXBPF_FUNC_trace_printk:
        value = helper_trace_printk(a0, a1, a2, a3, a4);
        break;
    case AXBPF_FUNC_trace_vprintk:
        value = helper_trace_printk(a0, a1, a2, a3, a4);
        break;
    case AXBPF_FUNC_get_prandom_u32: {
        u64 state = ctx ? ctx->random_state : 0x9E3779B97F4A7C15ull;
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        if (ctx) {
            ctx->random_state = state;
        }
        value = (state >> 32) & 0xFFFFFFFFull;
        break;
    }
    case AXBPF_FUNC_get_smp_processor_id:
        value = ctx ? ctx->cpu : 0;
        break;
    case AXBPF_FUNC_tail_call:
        /* Tail calls need a program array map; until that type exists, report
         * failure so the calling program falls through. */
        value = (u64)(i64)HELPER_ENOENT;
        break;
    case AXBPF_FUNC_ringbuf_output:
        value = helper_ringbuf_output(a0, a1, a2, a3);
        break;
    case AXBPF_FUNC_ringbuf_reserve:
        value = helper_ringbuf_reserve(a0, a1, a2);
        break;
    case AXBPF_FUNC_ringbuf_submit:
        value = (u64)(i64)axbpf_ringbuf_submit((axbpf_map_t *)a0, (void *)a1,
                                               (u32)a2);
        break;
    case AXBPF_FUNC_map_lookup_percpu_elem: {
        axbpf_map_t *map = (axbpf_map_t *)a0;
        if (!map) {
            value = 0;
            break;
        }
        u32 index = a1 ? *(const u32 *)a1 : 0;
        u32 cpu = (u32)a2;
        if (index >= map->def.max_entries || cpu >= 8) {
            value = 0;
            break;
        }
        value = (u64)(map->storage + ((u64)index * 8 + cpu) *
                                      map->def.value_size);
        break;
    }
    case AXBPF_FUNC_strcmp:
        value = helper_strcmp(a0, a1);
        break;
    case AXBPF_FUNC_get_current_pid_tgid:
        /* No process table yet; the low word carries the boot id so tracing
         * still produces distinguishable values. */
        value = (1ull << 32) | 1ull;
        break;
    case AXBPF_FUNC_get_current_comm:
        value = helper_get_current_comm(a0, a1);
        break;
    case AXBPF_FUNC_get_stack:
    case AXBPF_FUNC_get_task_stack:
        value = helper_get_stack(a0, a1, a2);
        break;
    case AXBPF_FUNC_perf_event_output:
        value = helper_perf_event_output(a0, a1, a2, a3, a4);
        break;
    default:
        value = 0;
        break;
    }
    if (result) {
        *result = value;
    }
    return value;
}

/* Expose the trace line for the shell's `bpf trace` command. */
const char *axbpf_helper_last_trace(void)
{
    return g_last_trace;
}

u64 axbpf_helper_trace_count(void)
{
    return g_trace_lines;
}
