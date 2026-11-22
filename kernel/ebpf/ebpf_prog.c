/*
 * AxiomOS - eBPF program lifecycle.
 *
 * Loading, verification, attachment and dispatch.  A program moves through
 * these states:
 *
 *     allocated -> instructions set -> verified -> (jitted) -> attached
 *
 * `axbpf_prog_load` runs the whole chain and returns a descriptor; attach only
 * succeeds for verified programs.  Dispatch chooses the JIT when one exists and
 * falls back to the interpreter otherwise, so a JIT failure never takes a
 * program out of service.
 *
 * The program table is a doubly linked list ordered by id.  Sixty-four
 * programs is the ceiling; freeing a program unlinks it and releases its
 * instruction array, JIT code and any attachment record.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>

#define PROG_EINVAL  -22
#define PROG_ENOENT  -2
#define PROG_ENOMEM  -12
#define PROG_EBUSY   -16
#define PROG_ENOSPC  -28

static axbpf_prog_t *g_prog_head;
static axbpf_prog_t *g_prog_tail;
static u32 g_prog_count;
static u32 g_prog_serial;
static bool_t g_ready;
static u64 g_boot_tsc;
static u64 g_tsc_hz = 3000000000ull;    /* conservative default */

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static u64 read_tsc(void)
{
    u32 low = 0;
    u32 high = 0;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((u64)high << 32) | low;
}

u64 axbpf_now_ns(void)
{
    if (g_tsc_hz == 0) {
        return read_tsc();
    }
    u64 current = read_tsc();
    if (current < g_boot_tsc) {
        return 0;
    }
    /* Multiply first, then divide; the TSC difference for a boot session fits
     * comfortably in 64 bits at 3 GHz for several years. */
    return (current - g_boot_tsc) * 1000000000ull / g_tsc_hz;
}

/* ------------------------------------------------------------------ */
/* Program table                                                       */
/* ------------------------------------------------------------------ */

static void link_prog(axbpf_prog_t *prog)
{
    prog->next = 0;
    prog->prev = g_prog_tail;
    if (g_prog_tail) {
        g_prog_tail->next = prog;
    } else {
        g_prog_head = prog;
    }
    g_prog_tail = prog;
}

static void unlink_prog(axbpf_prog_t *prog)
{
    if (prog->prev) {
        prog->prev->next = prog->next;
    } else {
        g_prog_head = prog->next;
    }
    if (prog->next) {
        prog->next->prev = prog->prev;
    } else {
        g_prog_tail = prog->prev;
    }
    prog->prev = 0;
    prog->next = 0;
}

axbpf_prog_t *axbpf_prog_alloc(const char *name, u32 type)
{
    if (g_prog_count >= AXBPF_MAX_PROGS) {
        return 0;
    }
    axbpf_prog_t *prog = (axbpf_prog_t *)kzalloc(sizeof(*prog));
    if (!prog) {
        return 0;
    }
    if (name) {
        ax_strlcpy(prog->name, name, sizeof(prog->name));
    } else {
        ax_snprintf(prog->name, sizeof(prog->name), "prog%u", g_prog_serial);
    }
    prog->prog_type = type;
    prog->id = ++g_prog_serial;
    prog->fd = (i32)prog->id;
    prog->verified = (bool_t)0;
    prog->jitted = (bool_t)0;
    link_prog(prog);
    g_prog_count++;
    return prog;
}

void axbpf_prog_free(axbpf_prog_t *prog)
{
    if (!prog) {
        return;
    }
    if (prog->jitted) {
        axbpf_jit_release(prog);
    }
    if (prog->insns) {
        kfree(prog->insns);
        prog->insns = 0;
    }
    unlink_prog(prog);
    if (g_prog_count > 0) {
        g_prog_count--;
    }
    kfree(prog);
}

i32 axbpf_prog_set_insns(axbpf_prog_t *prog, const axbpf_insn_t *insns, u32 count)
{
    if (!prog || !insns || count == 0) {
        return PROG_EINVAL;
    }
    if (count > AXBPF_MAX_INSNS) {
        return PROG_ENOSPC;
    }
    if (prog->insns) {
        kfree(prog->insns);
    }
    prog->insns = (axbpf_insn_t *)kzalloc(sizeof(axbpf_insn_t) * count);
    if (!prog->insns) {
        return PROG_ENOMEM;
    }
    for (u32 i = 0; i < count; i++) {
        prog->insns[i] = insns[i];
    }
    prog->insn_count = count;
    prog->verified = (bool_t)0;
    prog->jitted = (bool_t)0;
    /* A program's tag is a hash of its instructions, which is how two
     * identical loads can be recognised as the same object later. */
    u64 tag = axbpf_hash_insns(prog->insns, count);
    prog->tag[0] = tag;
    prog->tag[1] = tag ^ 0x9E3779B97F4A7C15ull;
    return 0;
}

i32 axbpf_prog_load(const char *name, u32 type, const axbpf_insn_t *insns,
                    u32 count, u32 flags)
{
    if (!g_ready) {
        axbpf_init();
    }
    axbpf_prog_t *prog = axbpf_prog_alloc(name, type);
    if (!prog) {
        return PROG_ENOSPC;
    }
    prog->flags = flags;
    i32 status = axbpf_prog_set_insns(prog, insns, count);
    if (status != 0) {
        axbpf_prog_free(prog);
        return status;
    }
    axbpf_verify_result_t result;
    status = axbpf_verify(prog, &result);
    if (status != 0) {
        printk("bpf: load of %s rejected: %s\n", prog->name,
               result.first_error);
        axbpf_prog_free(prog);
        return status;
    }
    if (flags & AXBPF_PROG_FLAG_JIT) {
        i32 jit_status = axbpf_jit_compile(prog);
        if (jit_status != 0) {
            printk("bpf: %s stays interpreted (jit status %d)\n", prog->name,
                   jit_status);
        }
    }
    printk("bpf: loaded %s id=%u type=%u insns=%u%s%s\n", prog->name, prog->id,
           type, count, prog->verified ? " verified" : "",
           prog->jitted ? " jitted" : "");
    return prog->fd;
}

i32 axbpf_prog_find(const char *name)
{
    if (!name) {
        return PROG_ENOENT;
    }
    for (axbpf_prog_t *prog = g_prog_head; prog; prog = prog->next) {
        if (ax_strcmp(prog->name, name) == 0) {
            return prog->fd;
        }
    }
    return PROG_ENOENT;
}

static axbpf_prog_t *prog_by_fd(i32 fd)
{
    for (axbpf_prog_t *prog = g_prog_head; prog; prog = prog->next) {
        if (prog->fd == fd) {
            return prog;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Attachment                                                          */
/* ------------------------------------------------------------------ */

typedef struct attach_entry {
    i32  fd;
    u32  attach_type;
    void *target;
    u64  hits;
    struct attach_entry *next;
} attach_entry_t;

static attach_entry_t *g_attachments;
static u32 g_attach_count;

i32 axbpf_prog_attach(i32 fd, u32 attach_type, void *target)
{
    axbpf_prog_t *prog = prog_by_fd(fd);
    if (!prog) {
        return PROG_ENOENT;
    }
    if (!prog->verified) {
        return PROG_EINVAL;
    }
    for (attach_entry_t *entry = g_attachments; entry; entry = entry->next) {
        if (entry->fd == fd && entry->attach_type == attach_type &&
            entry->target == target) {
            return PROG_EBUSY;
        }
    }
    attach_entry_t *entry = (attach_entry_t *)kzalloc(sizeof(*entry));
    if (!entry) {
        return PROG_ENOMEM;
    }
    entry->fd = fd;
    entry->attach_type = attach_type;
    entry->target = target;
    entry->next = g_attachments;
    g_attachments = entry;
    g_attach_count++;
    prog->attach_type = attach_type;
    prog->attach_target = target;
    printk("bpf: attached %s to %s (type %u)\n", prog->name,
           target ? (const char *)target : "<anonymous>", attach_type);
    return 0;
}

i32 axbpf_prog_detach(i32 fd)
{
    attach_entry_t *previous = 0;
    bool_t found = (bool_t)0;
    attach_entry_t *entry = g_attachments;
    while (entry) {
        if (entry->fd == fd) {
            attach_entry_t *next = entry->next;
            if (previous) {
                previous->next = next;
            } else {
                g_attachments = next;
            }
            kfree(entry);
            found = (bool_t)1;
            g_attach_count--;
            entry = next;
            continue;
        }
        previous = entry;
        entry = entry->next;
    }
    axbpf_prog_t *prog = prog_by_fd(fd);
    if (prog) {
        prog->attach_type = AXBPF_ATTACH_NONE;
        prog->attach_target = 0;
    }
    return found ? 0 : PROG_ENOENT;
}

/* Fire every program attached to a given target.  Called by the trace layer
 * when an event arrives. */
u32 axbpf_prog_dispatch(void *target, axbpf_run_ctx_t *ctx)
{
    u32 ran = 0;
    for (attach_entry_t *entry = g_attachments; entry; entry = entry->next) {
        if (entry->target != target) {
            continue;
        }
        axbpf_prog_t *prog = prog_by_fd(entry->fd);
        if (!prog) {
            continue;
        }
        entry->hits++;
        axbpf_prog_run(prog, ctx);
        ran++;
    }
    return ran;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

typedef u64 (*jit_entry_fn)(axbpf_run_ctx_t *ctx);

u64 axbpf_prog_run(axbpf_prog_t *prog, axbpf_run_ctx_t *ctx)
{
    if (!prog || !prog->verified) {
        return 0;
    }
    axbpf_run_ctx_t local;
    if (!ctx) {
        ax_memset(&local, 0, sizeof(local));
        ctx = &local;
    }
    if (ctx->start_ns == 0) {
        ctx->start_ns = axbpf_now_ns();
    }
    if (prog->jitted && prog->jit_code) {
        jit_entry_fn entry = (jit_entry_fn)prog->jit_code;
        u64 result = entry(ctx);
        prog->run_count++;
        return result;
    }
    return axbpf_interp_run(prog, ctx);
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

void axbpf_prog_dump(axbpf_prog_t *prog)
{
    if (!prog) {
        return;
    }
    printk("bpf: program %s id=%u fd=%d type=%u attach=%u insns=%u%s%s\n",
           prog->name, prog->id, prog->fd, prog->prog_type, prog->attach_type,
           prog->insn_count, prog->verified ? " verified" : "",
           prog->jitted ? " jitted" : "");
    printk("  runs=%llu ns=%llu verify_ns=%llu tag=%016llx\n", prog->run_count,
           prog->run_ns, prog->verifier_ns, prog->tag[0]);
    char line[128];
    for (u32 i = 0; i < prog->insn_count && i < 16; i++) {
        char text[96];
        if (axbpf_disasm(&prog->insns[i], text, sizeof(text)) != 0) {
            ax_strlcpy(text, "<invalid>", sizeof(text));
        }
        ax_snprintf(line, sizeof(line), "    %3u: %s", i, text);
        printk("%s\n", line);
    }
    if (prog->insn_count > 16) {
        printk("    ... %u more\n", prog->insn_count - 16);
    }
}

void axbpf_prog_dump_all(void)
{
    printk("bpf: programs (%u)\n", g_prog_count);
    for (axbpf_prog_t *prog = g_prog_head; prog; prog = prog->next) {
        printk("  fd=%-3d %-16s type=%-2u insns=%-4u runs=%llu%s%s\n", prog->fd,
               prog->name, prog->prog_type, prog->insn_count, prog->run_count,
               prog->verified ? " v" : "", prog->jitted ? " jit" : "");
    }
    printk("bpf: attachments (%u)\n", g_attach_count);
    for (attach_entry_t *entry = g_attachments; entry; entry = entry->next) {
        printk("  fd=%d type=%u target=%s hits=%llu\n", entry->fd,
               entry->attach_type,
               entry->target ? (const char *)entry->target : "<none>",
               entry->hits);
    }
}

void axbpf_prog_stats(char *buffer, usize cap)
{
    if (!buffer) {
        return;
    }
    u64 runs = 0;
    u64 ns = 0;
    u64 jitted = 0;
    for (axbpf_prog_t *prog = g_prog_head; prog; prog = prog->next) {
        runs += prog->run_count;
        ns += prog->run_ns;
        if (prog->jitted) {
            jitted++;
        }
    }
    ax_snprintf(buffer, cap,
                "progs=%u attachments=%u runs=%llu ns=%llu jitted=%llu\n",
                g_prog_count, g_attach_count, runs, ns, jitted);
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

void axbpf_init(void)
{
    if (g_ready) {
        return;
    }
    g_prog_head = 0;
    g_prog_tail = 0;
    g_prog_count = 0;
    g_prog_serial = 0;
    g_attachments = 0;
    g_attach_count = 0;
    g_boot_tsc = read_tsc();
    axbpf_map_init();
    axbpf_helper_init();
    axbpf_trace_init();
    g_ready = (bool_t)1;
    printk("bpf: subsystem ready, jit=%s, %u helpers, %u maps max\n",
           axbpf_jit_supported() ? "available" : "disabled", AXBPF_FUNC_MAX - 1,
           AXBPF_MAX_MAPS);
}

bool_t axbpf_ready(void)
{
    return g_ready;
}

u32 axbpf_prog_count(void)
{
    return g_prog_count;
}

/* ------------------------------------------------------------------ */
/* Built-in demonstration programs                                     */
/* ------------------------------------------------------------------ */

/* A program that counts invocations in an array map slot zero.
 *
 * r1 = context; r2 = pointer to a map; ...  The map is referenced by its
 * descriptor, which for this built-in is 1 (the first map created at init). */
static const axbpf_insn_t g_counter_program[] = {
    /* r6 = ctx */
    { (AXBPF_ALU64 | AXBPF_MOV | AXBPF_SRC_X), 0x06, 0, 0 },
    /* r1 = map reference (lddw placeholder: map id in imm) */
    { (AXBPF_LD | AXBPF_MODE_MEM | AXBPF_SIZE_DW), 0x01, 0, 1 },
    /* r2 = &key (stack) */
    { (AXBPF_ALU64 | AXBPF_MOV | AXBPF_SRC_X), 0x2A, 0, 0 },
    /* *(u32 *)(r10 - 4) = 0 */
    { (AXBPF_ST | AXBPF_MODE_MEM | AXBPF_SIZE_W), 0x0A, -4, 0 },
    /* r2 = r10; r2 += -4 */
    { (AXBPF_ALU64 | AXBPF_ADD | AXBPF_SRC_K), 0x0A, 0, -4 },
    /* call map_lookup_elem */
    { (AXBPF_JMP | AXBPF_CALL), 0x00, 0, 1 },
    /* if r0 == 0 goto exit */
    { (AXBPF_JMP | AXBPF_JEQ | AXBPF_SRC_K), 0x00, 2, 0 },
    /* r1 = *(u64 *)(r0 + 0) */
    { (AXBPF_LDX | AXBPF_MODE_MEM | AXBPF_SIZE_DW), 0x10, 0, 0 },
    /* r1 += 1 */
    { (AXBPF_ALU64 | AXBPF_ADD | AXBPF_SRC_K), 0x01, 0, 1 },
    /* *(u64 *)(r0 + 0) = r1 */
    { (AXBPF_STX | AXBPF_MODE_MEM | AXBPF_SIZE_DW), 0x10, 0, 1 },
    /* r0 = 0 */
    { (AXBPF_ALU64 | AXBPF_MOV | AXBPF_SRC_K), 0x00, 0, 0 },
    /* exit */
    { (AXBPF_JMP | AXBPF_EXIT), 0x00, 0, 0 },
};

/* Count how often the counter program can be verified and run.  Used at boot
 * when `bpftest=1` is on the command line, and by the shell's `bpf selftest`. */
i32 axbpf_prog_selftest(void)
{
    axbpf_map_def_t def;
    ax_memset(&def, 0, sizeof(def));
    def.type = AXBPF_MAP_ARRAY;
    def.key_size = 4;
    def.value_size = 8;
    def.max_entries = 1;
    i32 map_fd = axbpf_map_create(&def, "counter");
    if (map_fd < 0) {
        return map_fd;
    }

    i32 fd = axbpf_prog_load("selftest_counter", AXBPF_PROG_TRACEPOINT,
                             g_counter_program,
                             sizeof(g_counter_program) / sizeof(g_counter_program[0]),
                             AXBPF_PROG_FLAG_JIT);
    if (fd < 0) {
        axbpf_map_destroy(map_fd);
        return fd;
    }
    axbpf_prog_t *prog = prog_by_fd(fd);
    axbpf_run_ctx_t ctx;
    ax_memset(&ctx, 0, sizeof(ctx));
    ctx.start_ns = axbpf_now_ns();
    for (u32 i = 0; i < 4; i++) {
        axbpf_prog_run(prog, &ctx);
    }
    u32 key = 0;
    u64 counter = 0;
    i32 status = axbpf_map_lookup(map_fd, &key, &counter);
    printk("bpf: counter after 4 runs = %llu (lookup %d)\n", counter, status);
    return status;
}
