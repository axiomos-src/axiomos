/*
 * AxiomOS - eBPF subsystem: public interface.
 *
 * The kernel ships a small eBPF implementation so tracing and policy code can
 * be written once, verified, and run either interpreted or JIT compiled.
 *
 * Scope, stated plainly:
 *   - instruction set: the classic 64-bit eBPF ISA subset used by tracing
 *     programs (ALU, ALU64, jumps, load/store, calls, exit, atomics);
 *   - maps: array, hash, LRU hash, per-CPU array and a single ring buffer;
 *   - attachment: raw kernel function hooks, tracepoints, and XDP-style
 *     packet hooks are declared but only the first two are wired;
 *   - verification: full type and range tracking with bounded loops, no
 *     pointer arithmetic beyond approved offsets;
 *   - execution: interpreter and an x86-64 JIT for the integer subset.
 *
 * Programs are described by `axbpf_prog_t`, loaded through
 * `axbpf_prog_load()`, attached through `axbpf_prog_attach()` and run through
 * `axbpf_prog_run()`.  Nothing in this subsystem allocates on the hot path:
 * stack frames come from a per-CPU scratch area and map lookups are lockless
 * reads protected by an RCU-style generation counter.
 */

#ifndef AXIOM_AXBPF_H
#define AXIOM_AXBPF_H

#include <axiom/types.h>

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define AXBPF_MAX_INSNS         4096
#define AXBPF_MAX_PROGS         64
#define AXBPF_MAX_MAPS          32
#define AXBPF_MAX_MAP_ENTRIES   4096
#define AXBPF_MAX_HELPERS       64
#define AXBPF_STACK_SIZE        512
#define AXBPF_MAX_CALL_DEPTH    8
#define AXBPF_MAX_NAME          32
#define AXBPF_MAX_LOOP_ITERS    65536
#define AXBPF_JIT_CACHE_SIZE    (256 * 1024)
#define AXBPF_RINGBUF_SIZE      (64 * 1024)

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef u8  axbpf_u8;
typedef u16 axbpf_u16;
typedef u32 axbpf_u32;
typedef u64 axbpf_u64;
typedef i8  axbpf_s8;
typedef i16 axbpf_s16;
typedef i32 axbpf_s32;
typedef i64 axbpf_s64;

typedef enum axbpf_reg {
    AXBPF_REG_0 = 0,
    AXBPF_REG_1, AXBPF_REG_2, AXBPF_REG_3, AXBPF_REG_4, AXBPF_REG_5,
    AXBPF_REG_6, AXBPF_REG_7, AXBPF_REG_8, AXBPF_REG_9,
    AXBPF_REG_10,
    AXBPF_REG_COUNT
} axbpf_reg_t;

/* Instruction classes, values match the Linux ABI so object files and
 * disassembly tables are directly comparable. */
typedef enum axbpf_class {
    AXBPF_LD   = 0x00,
    AXBPF_LDX  = 0x01,
    AXBPF_ST   = 0x02,
    AXBPF_STX  = 0x03,
    AXBPF_ALU  = 0x04,
    AXBPF_JMP  = 0x05,
    AXBPF_JMP32 = 0x06,
    AXBPF_ALU64 = 0x07
} axbpf_class_t;

/* ALU operation codes (low nibble of the opcode byte). */
typedef enum axbpf_alu_op {
    AXBPF_ADD = 0x00,
    AXBPF_SUB = 0x10,
    AXBPF_MUL = 0x20,
    AXBPF_DIV = 0x30,
    AXBPF_OR  = 0x40,
    AXBPF_AND = 0x50,
    AXBPF_LSH = 0x60,
    AXBPF_RSH = 0x70,
    AXBPF_NEG = 0x80,
    AXBPF_MOD = 0x90,
    AXBPF_XOR = 0xA0,
    AXBPF_MOV = 0xB0,
    AXBPF_ARSH = 0xC0,
    AXBPF_END = 0xD0
} axbpf_alu_op_t;

typedef enum axbpf_jmp_op {
    AXBPF_JA   = 0x00,
    AXBPF_JEQ  = 0x10,
    AXBPF_JGT  = 0x20,
    AXBPF_JGE  = 0x30,
    AXBPF_JSET = 0x40,
    AXBPF_JNE  = 0x50,
    AXBPF_JSGT = 0x60,
    AXBPF_JSGE = 0x70,
    AXBPF_CALL = 0x80,
    AXBPF_EXIT = 0x90,
    AXBPF_JLT  = 0xA0,
    AXBPF_JLE  = 0xB0,
    AXBPF_JSLT = 0xC0,
    AXBPF_JSLE = 0xD0
} axbpf_jmp_op_t;

/* Source operand selector. */
#define AXBPF_SRC_K    0x00
#define AXBPF_SRC_X    0x08

/* Width selectors for load/store. */
#define AXBPF_SIZE_W   0x00
#define AXBPF_SIZE_H   0x08
#define AXBPF_SIZE_B   0x10
#define AXBPF_SIZE_DW  0x18

 /* Mode selectors. */
#define AXBPF_MODE_IMM  0x00
#define AXBPF_MODE_ABS  0x20
#define AXBPF_MODE_IND  0x40
#define AXBPF_MODE_MEM  0x60
#define AXBPF_MODE_ATOMIC 0xC0

#define AXBPF_PSEUDO_MAP_FD   1
#define AXBPF_PSEUDO_CALL     1
#define AXBPF_FETCH           0x01
#define AXBPF_XCHG            0xE1
#define AXBPF_CMPXCHG         0xF1

/* One instruction, eight bytes on the wire but a struct in memory. */
typedef struct axbpf_insn {
    u8  code;
    u8  dst_src;        /* low nibble dst, high nibble src */
    i16 off;
    i32 imm;
} axbpf_insn_t;

#define AXBPF_DST(insn)   ((u8)((insn)->dst_src & 0x0F))
#define AXBPF_SRC(insn)   ((u8)(((insn)->dst_src >> 4) & 0x0F))
#define AXBPF_CLASS(insn) ((u8)((insn)->code & 0x07))
#define AXBPF_OP(insn)    ((u8)((insn)->code & 0xF0))
#define AXBPF_SIZE(insn)  ((u8)((insn)->code & 0x18))
#define AXBPF_MODE(insn)  ((u8)((insn)->code & 0xE0))

/* ------------------------------------------------------------------ */
/* Register types tracked by the verifier                              */
/* ------------------------------------------------------------------ */

typedef enum axbpf_type {
    AXBPF_T_NOT_INIT = 0,
    AXBPF_T_MAP_VALUE,
    AXBPF_T_MAP_VALUE_OR_NULL,
    AXBPF_T_STACK_PTR,
    AXBPF_T_PACKET_PTR,
    AXBPF_T_PACKET_META,
    AXBPF_T_CONTEXT_PTR,
    AXBPF_T_SCALAR,
    AXBPF_T_SCALAR_OR_NULL,
    AXBPF_T_PTR_TO_MAP,
    AXBPF_T_CONST_PTR,
    AXBPF_T_MEM_ALLOC
} axbpf_type_t;

/* Access rights on a pointer-typed register. */
#define AXBPF_ACCESS_READ   0x01
#define AXBPF_ACCESS_WRITE  0x02
#define AXBPF_ACCESS_ATOMIC 0x04

typedef struct axbpf_reg_state {
    u8   type;
    u8   access;
    bool_t id;              /* register holds a known object id */
    u32  obj_id;
    i64  smin;              /* known signed range */
    i64  smax;
    u64  umin;
    u64  umax;
    u32  off;               /* constant offset into a pointed object */
    bool_t precise;         /* range came from a comparison, not arithmetic */
} axbpf_reg_state_t;

typedef struct axbpf_stack_slot {
    u8   type;
    u8   size;
    u32  obj_id;
    i64  smin;
    i64  smax;
} axbpf_stack_slot_t;

/* ------------------------------------------------------------------ */
/* Maps                                                                */
/* ------------------------------------------------------------------ */

typedef enum axbpf_map_type {
    AXBPF_MAP_ARRAY = 1,
    AXBPF_MAP_HASH,
    AXBPF_MAP_LRU_HASH,
    AXBPF_MAP_PERCPU_ARRAY,
    AXBPF_MAP_RINGBUF,
    AXBPF_MAP_STACK_TRACE,
    AXBPF_MAP_UNSUPPORTED
} axbpf_map_type_t;

#define AXBPF_MAP_FLAG_READONLY   0x01
#define AXBPF_MAP_FLAG_MMAPABLE   0x02
#define AXBPF_MAP_FLAG_NO_PREALLOC 0x04

typedef struct axbpf_map_def {
    u32 type;
    u32 key_size;
    u32 value_size;
    u32 max_entries;
    u32 flags;
    char name[AXBPF_MAX_NAME];
} axbpf_map_def_t;

typedef struct axbpf_map_entry {
    void *key;
    void *value;
    u64  generation;
    u64  last_used;
    struct axbpf_map_entry *next;
} axbpf_map_entry_t;

typedef struct axbpf_ringbuf {
    u8  *data;
    u32 size;
    u32 mask;
    u64 head;               /* written by producers */
    u64 tail;               /* advanced by consumers */
    u32 dropped;
    u32 reserved;
} axbpf_ringbuf_t;

typedef struct axbpf_map {
    axbpf_map_def_t def;
    u8  *storage;
    u32 entry_count;
    u64 hits;
    u64 misses;
    u64 updates;
    u64 deletes;
    u64 generation;
    axbpf_map_entry_t **buckets;
    u32 bucket_count;
    u32 lru_capacity;
    axbpf_map_entry_t *lru_head;
    axbpf_map_entry_t *lru_tail;
    axbpf_ringbuf_t ring;
    bool_t frozen;
    bool_t in_use;
    i32 fd;
    char name[AXBPF_MAX_NAME];
} axbpf_map_t;

typedef struct axbpf_map_ops {
    i32 (*create)(axbpf_map_t *map, const axbpf_map_def_t *def);
    void (*destroy)(axbpf_map_t *map);
    i32 (*lookup)(axbpf_map_t *map, const void *key, void *value);
    i32 (*update)(axbpf_map_t *map, const void *key, const void *value, u32 flags);
    i32 (*remove)(axbpf_map_t *map, const void *key);
    i32 (*next_key)(axbpf_map_t *map, const void *key, void *next);
} axbpf_map_ops_t;

/* ------------------------------------------------------------------ */
/* Programs                                                            */
/* ------------------------------------------------------------------ */

typedef enum axbpf_prog_type {
    AXBPF_PROG_SOCKET_FILTER = 1,
    AXBPF_PROG_KPROBE,
    AXBPF_PROG_TRACEPOINT,
    AXBPF_PROG_XDP,
    AXBPF_PROG_SCHED_CLS,
    AXBPF_PROG_PERF_EVENT,
    AXBPF_PROG_UNSPEC
} axbpf_prog_type_t;

typedef enum axbpf_attach_type {
    AXBPF_ATTACH_NONE = 0,
    AXBPF_ATTACH_ENTRY,
    AXBPF_ATTACH_EXIT,
    AXBPF_ATTACH_PACKET_IN,
    AXBPF_ATTACH_PACKET_OUT,
    AXBPF_ATTACH_SCHED,
    AXBPF_ATTACH_SYSCALL
} axbpf_attach_type_t;

#define AXBPF_PROG_FLAG_JIT       0x01
#define AXBPF_PROG_FLAG_VERBOSE   0x02
#define AXBPF_PROG_FLAG_TEST_RUN  0x04
#define AXBPF_PROG_FLAG_SLEEPABLE 0x08

typedef struct axbpf_run_ctx {
    u64  arg0;
    u64  arg1;
    u64  arg2;
    u64  arg3;
    u64  arg4;
    void *data;
    u32  data_len;
    u32  retval;
    u64  start_ns;
    bool_t in_nmi;
} axbpf_run_ctx_t;

typedef struct axbpf_prog {
    char name[AXBPF_MAX_NAME];
    u32  prog_type;
    u32  attach_type;
    u32  flags;
    u32  insn_count;
    axbpf_insn_t *insns;
    u32  id;
    u64  run_count;
    u64  run_ns;
    u64  verifier_ns;
    i32  fd;
    bool_t verified;
    bool_t jitted;
    void *jit_code;
    u32  jit_size;
    u8   jit_cache[AXBPF_JIT_CACHE_SIZE];
    void *attach_target;
    i32  next_prog_id;
    struct axbpf_prog *prev;
    struct axbpf_prog *next;
    u64  tag[2];
} axbpf_prog_t;

/* ------------------------------------------------------------------ */
/* Helper functions                                                    */
/* ------------------------------------------------------------------ */

typedef enum axbpf_helper_id {
    AXBPF_FUNC_unspec = 0,
    AXBPF_FUNC_map_lookup_elem = 1,
    AXBPF_FUNC_map_update_elem = 2,
    AXBPF_FUNC_map_delete_elem = 3,
    AXBPF_FUNC_probe_read = 4,
    AXBPF_FUNC_ktime_get_ns = 5,
    AXBPF_FUNC_trace_printk = 6,
    AXBPF_FUNC_get_prandom_u32 = 7,
    AXBPF_FUNC_get_smp_processor_id = 8,
    AXBPF_FUNC_tail_call = 9,
    AXBPF_FUNC_perf_event_output = 10,
    AXBPF_FUNC_ringbuf_output = 11,
    AXBPF_FUNC_ringbuf_reserve = 12,
    AXBPF_FUNC_ringbuf_submit = 13,
    AXBPF_FUNC_map_lookup_percpu_elem = 14,
    AXBPF_FUNC_strcmp = 15,
    AXBPF_FUNC_get_current_pid_tgid = 16,
    AXBPF_FUNC_get_current_comm = 17,
    AXBPF_FUNC_probe_read_kernel = 18,
    AXBPF_FUNC_probe_read_user = 19,
    AXBPF_FUNC_get_stack = 20,
    AXBPF_FUNC_trace_vprintk = 21,
    AXBPF_FUNC_jiffies64 = 22,
    AXBPF_FUNC_get_task_stack = 23,
    AXBPF_FUNC_copy_from_user = 24,
    AXBPF_FUNC_kptr = 25,
    AXBPF_FUNC_MAX
} axbpf_helper_id_t;

typedef u64 (*axbpf_helper_fn)(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4);

typedef struct axbpf_helper_def {
    axbpf_helper_id_t id;
    const char *name;
    char prototype[64];
    u8   arg_types[5];
    bool_t return_may_be_null;
} axbpf_helper_def_t;

typedef struct axbpf_helper_ctx {
    axbpf_map_t **maps;
    u32 map_count;
    axbpf_run_ctx_t *run;
    u64 random_state;
    u32 cpu;
} axbpf_helper_ctx_t;

/* ------------------------------------------------------------------ */
/* Verifier                                                            */
/* ------------------------------------------------------------------ */

typedef struct axbpf_verifier_env {
    axbpf_prog_t *prog;
    u32 insn_index;
    u32 stack_depth;
    u32 call_depth;
    u32 loop_depth;
    u32 iterations;
    u32 errors;
    u32 warnings;
    bool_t log;
    bool_t allow_loops;
    axbpf_reg_state_t regs[AXBPF_REG_COUNT];
    axbpf_stack_slot_t stack[AXBPF_STACK_SIZE / 8];
    u32 explored_states;
    u32 max_explored_states;
    u32 prune_hits;
    u32 branch_unreachable;
} axbpf_verifier_env_t;

typedef struct axbpf_verify_result {
    i32  status;
    u32 errors;
    u32 warnings;
    u32 max_stack_depth;
    u32 insns_checked;
    u64 elapsed_ns;
    char first_error[96];
} axbpf_verify_result_t;

/* ------------------------------------------------------------------ */
/* JIT                                                                 */
/* ------------------------------------------------------------------ */

typedef struct axbpf_jit_ctx {
    u8  *code;
    u32 capacity;
    u32 size;
    u32 prologue_size;
    u32 epilogue_size;
    bool_t failed;
    char error[64];
    struct {
        u32 insn_index;
        u32 code_offset;
    } map[AXBPF_MAX_INSNS];
    u32 map_count;
    u32 fixups;
} axbpf_jit_ctx_t;

/* ------------------------------------------------------------------ */
/* Tracing                                                             */
/* ------------------------------------------------------------------ */

typedef enum axbpf_trace_kind {
    AXBPF_TRACE_KPROBE = 1,
    AXBPF_TRACE_TRACEPOINT,
    AXBPF_TRACE_SYSCALL,
    AXBPF_TRACE_SCHED
} axbpf_trace_kind_t;

typedef struct axbpf_trace_event {
    u64 timestamp_ns;
    u32 cpu;
    u32 pid;
    u32 kind;
    u32 data_len;
    u64 args[6];
} axbpf_trace_event_t;

typedef struct axbpf_trace_sink {
    void (*emit)(const axbpf_trace_event_t *event);
    u32 capacity;
} axbpf_trace_sink_t;

/* ------------------------------------------------------------------ */
/* Subsystem API                                                       */
/* ------------------------------------------------------------------ */

/* Initialisation, called once from kmain after the VFS is up. */
u64  axbpf_now_ns(void);
void axbpf_init(void);
bool_t axbpf_ready(void);
u32  axbpf_prog_count(void);
u32  axbpf_prog_dispatch(void *target, axbpf_run_ctx_t *ctx);
i32  axbpf_prog_selftest(void);
u64  axbpf_jit_helper(u64 ctx, u32 id, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4);

/* Program management. */
axbpf_prog_t *axbpf_prog_alloc(const char *name, u32 type);
void axbpf_prog_free(axbpf_prog_t *prog);
i32  axbpf_prog_set_insns(axbpf_prog_t *prog, const axbpf_insn_t *insns, u32 count);
i32  axbpf_prog_load(const char *name, u32 type, const axbpf_insn_t *insns,
                     u32 count, u32 flags);
i32  axbpf_prog_attach(i32 fd, u32 attach_type, void *target);
i32  axbpf_prog_detach(i32 fd);
u64  axbpf_prog_run(axbpf_prog_t *prog, axbpf_run_ctx_t *ctx);
i32  axbpf_prog_find(const char *name);
void axbpf_prog_dump(axbpf_prog_t *prog);
void axbpf_prog_dump_all(void);
void axbpf_prog_stats(char *buffer, usize cap);

/* Verification. */
i32 axbpf_verify(axbpf_prog_t *prog, axbpf_verify_result_t *result);
i32 axbpf_verify_insn(axbpf_verifier_env_t *env, const axbpf_insn_t *insn);
const char *axbpf_type_name(u8 type);
const char *axbpf_verify_status_name(i32 status);

/* Interpretation. */
u64 axbpf_interp_run(axbpf_prog_t *prog, axbpf_run_ctx_t *ctx);
i32 axbpf_interp_check(const axbpf_prog_t *prog);
void axbpf_interp_stats(char *buffer, usize cap);
i32 axbpf_interp_selftest(void);

/* JIT. */
i32  axbpf_jit_compile(axbpf_prog_t *prog);
void axbpf_jit_release(axbpf_prog_t *prog);
bool_t axbpf_jit_supported(void);
void axbpf_jit_dump(const axbpf_prog_t *prog);

/* Maps. */
void axbpf_map_init(void);
i32  axbpf_map_create(const axbpf_map_def_t *def, const char *name);
i32  axbpf_map_lookup(i32 fd, const void *key, void *value);
i32  axbpf_map_update(i32 fd, const void *key, const void *value, u32 flags);
i32  axbpf_map_delete(i32 fd, const void *key);
i32  axbpf_map_lookup_by_name(const char *name, const void *key, void *value);
axbpf_map_t *axbpf_map_by_fd(i32 fd);
axbpf_map_entry_t *axbpf_map_find_entry(axbpf_map_t *map, const void *key);
void axbpf_trace_emit(const axbpf_trace_event_t *event);
const char *axbpf_helper_last_trace(void);
u64  axbpf_helper_trace_count(void);
axbpf_map_t *axbpf_map_by_name(const char *name);
i32  axbpf_map_next_key(i32 fd, const void *key, void *next);
void axbpf_map_destroy(i32 fd);
void axbpf_map_dump_all(void);
u64  axbpf_map_total_entries(void);
i32  axbpf_map_selftest(void);
i32  axbpf_ringbuf_reserve(axbpf_map_t *map, u32 size, void **out);
i32  axbpf_ringbuf_submit(axbpf_map_t *map, void *record, u32 size);
u32  axbpf_ringbuf_consume(axbpf_map_t *map, void *out, u32 capacity);

/* Helpers. */
void axbpf_helper_init(void);
u64  axbpf_helper_call(axbpf_helper_id_t id, axbpf_helper_ctx_t *ctx,
                       u64 *args, u64 *result);
const axbpf_helper_def_t *axbpf_helper_lookup(u32 id);
i32  axbpf_helper_check_args(u32 id, const axbpf_reg_state_t *regs,
                             char *error, usize error_cap);
void axbpf_helper_dump(void);
u64  axbpf_helper_count_calls(u32 id);

/* Tracing. */
void axbpf_trace_init(void);
i32  axbpf_trace_register(u32 kind, const char *name, axbpf_prog_t *prog);
i32  axbpf_trace_unregister(u32 kind, const char *name);
void axbpf_trace_fire(u32 kind, const char *name, u64 a0, u64 a1, u64 a2);
void axbpf_trace_sink_set(axbpf_trace_sink_t *sink);
void axbpf_trace_dump(void);
bool_t axbpf_trace_latest(axbpf_trace_event_t *out);
i32  axbpf_trace_selftest(void);
void axbpf_probe_syscall(u32 number, u64 arg0, u64 arg1);
void axbpf_probe_schedule(u32 previous_pid, u32 next_pid);
void axbpf_probe_page_fault(u64 address, u64 error);
void axbpf_probe_file_open(const char *path, u32 flags, u32 result);
u64  axbpf_trace_count(void);

/* Disassembly and helpers for tooling. */
const char *axbpf_class_name(u8 code);
const char *axbpf_op_name(u8 code);
i32  axbpf_disasm(const axbpf_insn_t *insn, char *out, usize cap);
void axbpf_prog_class_stats(const axbpf_prog_t *prog, u32 counts[8]);
i32  axbpf_first_touch(const axbpf_prog_t *prog, u8 reg);
void axbpf_disasm_prog(const axbpf_prog_t *prog, void (*emit)(const char *line));
u64  axbpf_hash_insns(const axbpf_insn_t *insns, u32 count);
bool_t axbpf_insn_is_jump(const axbpf_insn_t *insn);
bool_t axbpf_insn_writes_reg(const axbpf_insn_t *insn, u8 reg);
u32  axbpf_insn_stack_delta(const axbpf_insn_t *insn, u32 *depth);

/* Self tests. */
i32 axbpf_selftest(void);

#endif /* AXIOM_AXBPF_H */
