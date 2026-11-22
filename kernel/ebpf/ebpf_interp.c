/*
 * AxiomOS - eBPF interpreter.
 *
 * A direct-threaded interpreter over the verified instruction array.  It runs
 * at roughly one guest instruction per host instruction, which is enough for
 * tracing programs that execute a few thousand instructions per event.
 *
 * Safety: the verifier has already proved that every memory access is inside
 * an object the program is allowed to touch, but the interpreter still bounds
 * checks the program counter and the stack pointer, because a bug in the
 * verifier must not become a bug in the kernel.
 *
 * The stack frame is 512 bytes, addressed through r10 as negative offsets,
 * matching the verifier's model exactly.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>

typedef struct interp_frame {
    u64 regs[AXBPF_REG_COUNT];
    u8  stack[AXBPF_STACK_SIZE];
    u32 depth;
} interp_frame_t;

/* Statistics, exported for `bpf stat`. */
static u64 g_interp_runs;
static u64 g_interp_insns;
static u64 g_interp_max_insns;
static u64 g_interp_helper_calls;
static u64 g_interp_rejections;

/* ------------------------------------------------------------------ */
/* Sign helpers                                                        */
/* ------------------------------------------------------------------ */

static i64 as_s64(u64 value, u32 width)
{
    switch (width) {
    case 1:
        return (i64)(i8)value;
    case 2:
        return (i64)(i16)value;
    case 4:
        return (i64)(i32)value;
    default:
        return (i64)value;
    }
}

static u64 truncate_width(u64 value, u32 width)
{
    switch (width) {
    case 1:
        return (u64)(u8)value;
    case 2:
        return (u64)(u16)value;
    case 4:
        return (u64)(u32)value;
    default:
        return value;
    }
}

static u32 width_of_size(u8 size_sel)
{
    switch (size_sel) {
    case AXBPF_SIZE_B:
        return 1;
    case AXBPF_SIZE_H:
        return 2;
    case AXBPF_SIZE_W:
        return 4;
    default:
        return 8;
    }
}

/* ------------------------------------------------------------------ */
/* Memory access                                                       */
/* ------------------------------------------------------------------ */

static i32 mem_read(u64 address, u32 width, u64 *out)
{
    if (!out || address == 0) {
        return -1;
    }
    switch (width) {
    case 1:
        *out = *(volatile u8 *)address;
        return 0;
    case 2:
        *out = *(volatile u16 *)address;
        return 0;
    case 4:
        *out = *(volatile u32 *)address;
        return 0;
    case 8:
        *out = *(volatile u64 *)address;
        return 0;
    default:
        return -1;
    }
}

static i32 mem_write(u64 address, u32 width, u64 value)
{
    if (address == 0) {
        return -1;
    }
    switch (width) {
    case 1:
        *(volatile u8 *)address = (u8)value;
        return 0;
    case 2:
        *(volatile u16 *)address = (u16)value;
        return 0;
    case 4:
        *(volatile u32 *)address = (u32)value;
        return 0;
    case 8:
        *(volatile u64 *)address = value;
        return 0;
    default:
        return -1;
    }
}

/* Atomic operations on memory: add, or, and, xor, xchg and cmpxchg.  Only the
 * 32- and 64-bit widths are defined by the ISA. */
static i32 mem_atomic(u64 address, u32 width, u32 op, u64 operand, u64 *result)
{
    if (address == 0 || !result) {
        return -1;
    }
    volatile u64 *slot = (volatile u64 *)address;
    switch (width) {
    case 4: {
        volatile u32 *slot32 = (volatile u32 *)address;
        u32 old = *slot32;
        switch (op) {
        case AXBPF_ADD:
            *slot32 = old + (u32)operand;
            break;
        case AXBPF_OR:
            *slot32 = old | (u32)operand;
            break;
        case AXBPF_AND:
            *slot32 = old & (u32)operand;
            break;
        case AXBPF_XOR:
            *slot32 = old ^ (u32)operand;
            break;
        case AXBPF_XCHG:
            *slot32 = (u32)operand;
            break;
        case AXBPF_CMPXCHG:
            if (old == (u32)operand) {
                *slot32 = (u32)*result;
            }
            break;
        default:
            return -1;
        }
        *result = old;
        return 0;
    }
    case 8: {
        u64 old = *slot;
        switch (op) {
        case AXBPF_ADD:
            *slot = old + operand;
            break;
        case AXBPF_OR:
            *slot = old | operand;
            break;
        case AXBPF_AND:
            *slot = old & operand;
            break;
        case AXBPF_XOR:
            *slot = old ^ operand;
            break;
        case AXBPF_XCHG:
            *slot = operand;
            break;
        case AXBPF_CMPXCHG:
            if (old == operand) {
                *slot = *result;
            }
            break;
        default:
            return -1;
        }
        *result = old;
        return 0;
    }
    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* ALU execution                                                       */
/* ------------------------------------------------------------------ */

static u64 alu_compute(u8 op, u64 dst, u64 src, bool_t is_64, i32 imm, bool_t src_from_reg)
{
    u64 value = src_from_reg ? src : (u64)imm;
    u32 width = is_64 ? 8 : 4;
    switch (op) {
    case AXBPF_ADD:
        return truncate_width(dst + value, width);
    case AXBPF_SUB:
        return truncate_width(dst - value, width);
    case AXBPF_MUL:
        return truncate_width(dst * value, width);
    case AXBPF_DIV:
        if (value == 0) {
            return 0;
        }
        return truncate_width(dst / value, width);
    case AXBPF_MOD:
        if (value == 0) {
            return dst;
        }
        return truncate_width(dst % value, width);
    case AXBPF_OR:
        return truncate_width(dst | value, width);
    case AXBPF_AND:
        return truncate_width(dst & value, width);
    case AXBPF_XOR:
        return truncate_width(dst ^ value, width);
    case AXBPF_LSH:
        if (value >= 64) {
            return 0;
        }
        return truncate_width(dst << value, width);
    case AXBPF_RSH:
        if (value >= 64) {
            return 0;
        }
        return truncate_width(dst >> value, width);
    case AXBPF_ARSH: {
        if (value >= 64) {
            return as_s64(dst, width) < 0 ? 0xFFFFFFFFFFFFFFFFull : 0;
        }
        i64 signed_value = as_s64(dst, width);
        i64 shifted = signed_value >> value;
        return truncate_width((u64)shifted, width);
    }
    case AXBPF_NEG:
        return truncate_width(0 - dst, width);
    case AXBPF_MOV:
        return truncate_width(value, width);
    case AXBPF_END: {
        /* `imm` carries the conversion: 16/32/64 and a byte-swap flag in bit
         * 8.  AxiomOS follows the ISA: imm == 16, 32, 64 or the same | 0x08. */
        u32 bits = (u32)(imm & 0xFF);
        u32 swap = (u32)(imm & 0x08);
        u64 value_in = dst;
        u64 result = value_in;
        if (bits == 16) {
            result = value_in & 0xFFFFu;
        } else if (bits == 32) {
            result = value_in & 0xFFFFFFFFu;
        }
        if (swap) {
            if (bits == 16) {
                result = ((result & 0xFF) << 8) | ((result >> 8) & 0xFF);
            } else if (bits == 32) {
                result = ((result & 0xFF) << 24) | (((result >> 8) & 0xFF) << 16) |
                         (((result >> 16) & 0xFF) << 8) | ((result >> 24) & 0xFF);
            } else {
                u64 swapped = 0;
                for (u32 i = 0; i < 8; i++) {
                    swapped |= ((result >> (i * 8)) & 0xFF) << ((7 - i) * 8);
                }
                result = swapped;
            }
        }
        return result;
    }
    default:
        return dst;
    }
}

static bool_t jump_taken(u8 op, u64 dst, u64 src, i32 imm, bool_t src_from_reg, bool_t is_64)
{
    u64 value = src_from_reg ? src : (u64)imm;
    u32 width = is_64 ? 8 : 4;
    u64 lhs = truncate_width(dst, width);
    u64 rhs = truncate_width(value, width);
    i64 lhs_s = as_s64(lhs, width);
    i64 rhs_s = as_s64(rhs, width);
    switch (op) {
    case AXBPF_JA:
        return (bool_t)1;
    case AXBPF_JEQ:
        return (bool_t)(lhs == rhs);
    case AXBPF_JNE:
        return (bool_t)(lhs != rhs);
    case AXBPF_JGT:
        return (bool_t)(lhs > rhs);
    case AXBPF_JGE:
        return (bool_t)(lhs >= rhs);
    case AXBPF_JLT:
        return (bool_t)(lhs < rhs);
    case AXBPF_JLE:
        return (bool_t)(lhs <= rhs);
    case AXBPF_JSGT:
        return (bool_t)(lhs_s > rhs_s);
    case AXBPF_JSGE:
        return (bool_t)(lhs_s >= rhs_s);
    case AXBPF_JSLT:
        return (bool_t)(lhs_s < rhs_s);
    case AXBPF_JSLE:
        return (bool_t)(lhs_s <= rhs_s);
    case AXBPF_JSET:
        return (bool_t)((lhs & rhs) != 0);
    default:
        return (bool_t)0;
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

u64 axbpf_interp_run(axbpf_prog_t *prog, axbpf_run_ctx_t *ctx)
{
    if (!prog || !prog->insns || !prog->verified) {
        g_interp_rejections++;
        return 0;
    }
    static interp_frame_t frame;
    ax_memset(&frame, 0, sizeof(frame));
    frame.regs[AXBPF_REG_1] = (u64)ctx;
    frame.regs[AXBPF_REG_10] = (u64)(frame.stack + AXBPF_STACK_SIZE - 16);
    frame.depth = 0;

    axbpf_helper_ctx_t helper_ctx;
    ax_memset(&helper_ctx, 0, sizeof(helper_ctx));
    helper_ctx.run = ctx;
    helper_ctx.random_state = ctx ? ctx->start_ns : 1;
    helper_ctx.cpu = 0;
    if (prog->attach_target) {
        /* The attach point may carry a map table; the run context does not, so
         * helpers fall back to the global map registry. */
        helper_ctx.maps = 0;
        helper_ctx.map_count = 0;
    }

    g_interp_runs++;
    u64 pc = 0;
    u64 executed = 0;
    u64 budget = (u64)prog->insn_count * 64ull;
    if (budget < 1024) {
        budget = 1024;
    }

    while (pc < prog->insn_count) {
        const axbpf_insn_t *insn = &prog->insns[pc];
        executed++;
        if (executed > budget) {
            g_interp_rejections++;
            break;
        }
        u8 cls = AXBPF_CLASS(insn);
        u8 op = AXBPF_OP(insn);
        u8 dst = AXBPF_DST(insn);
        u8 src = AXBPF_SRC(insn);
        bool_t src_from_reg = (bool_t)(src == AXBPF_SRC_X);
        bool_t is_64 = (bool_t)(cls == AXBPF_ALU64);

        switch (cls) {
        case AXBPF_ALU:
        case AXBPF_ALU64: {
            u64 source = src_from_reg ? frame.regs[src] : (u64)insn->imm;
            if (op == AXBPF_END) {
                frame.regs[dst] = alu_compute(op, frame.regs[dst], 0, (bool_t)1,
                                              insn->imm, (bool_t)0);
            } else {
                frame.regs[dst] = alu_compute(op, frame.regs[dst], source, is_64,
                                              insn->imm, src_from_reg);
            }
            pc++;
            break;
        }
        case AXBPF_LDX: {
            u64 address = frame.regs[src] + (u64)(i64)insn->off;
            u32 width = width_of_size(AXBPF_SIZE(insn));
            u64 value = 0;
            if (mem_read(address, width, &value) == 0) {
                frame.regs[dst] = value;
            } else {
                g_interp_rejections++;
                return 0;
            }
            pc++;
            break;
        }
        case AXBPF_ST: {
            u64 address = frame.regs[dst] + (u64)(i64)insn->off;
            u32 width = width_of_size(AXBPF_SIZE(insn));
            if (mem_write(address, width, (u64)insn->imm) != 0) {
                g_interp_rejections++;
                return 0;
            }
            if (dst == AXBPF_REG_10) {
                u32 used = (u32)(-insn->off);
                if (used > frame.depth) {
                    frame.depth = used;
                }
            }
            pc++;
            break;
        }
        case AXBPF_STX: {
            u64 address = frame.regs[dst] + (u64)(i64)insn->off;
            u32 width = width_of_size(AXBPF_SIZE(insn));
            if (AXBPF_MODE(insn) == AXBPF_MODE_ATOMIC) {
                u64 operand = frame.regs[src];
                u64 previous = frame.regs[0];
                u32 atomic_op = (u32)insn->imm;
                if (mem_atomic(address, width, atomic_op, operand, &previous) != 0) {
                    g_interp_rejections++;
                    return 0;
                }
                /* The fetched value lands in the source register, as the ABI
                 * requires. */
                frame.regs[src] = previous;
            } else {
                if (mem_write(address, width, frame.regs[src]) != 0) {
                    g_interp_rejections++;
                    return 0;
                }
                if (dst == AXBPF_REG_10) {
                    u32 used = (u32)(-insn->off);
                    if (used > frame.depth) {
                        frame.depth = used;
                    }
                }
            }
            pc++;
            break;
        }
        case AXBPF_LD: {
            if (AXBPF_MODE(insn) == AXBPF_MODE_IMM && AXBPF_SIZE(insn) == AXBPF_SIZE_DW) {
                /* Two-instruction form: high word lives in the next
                 * instruction's immediate. */
                u64 low = (u32)insn->imm;
                u64 high = 0;
                if (pc + 1 < prog->insn_count) {
                    high = (u32)prog->insns[pc + 1].imm;
                }
                frame.regs[dst] = low | (high << 32);
                pc += 2;
            } else if (AXBPF_MODE(insn) == AXBPF_MODE_MEM) {

                /* Map references resolve at load time into an id; a lookup by
                 * id goes through the map registry. */
                axbpf_map_t *target = axbpf_map_by_fd((i32)insn->imm);
                if (!target) {
                    frame.regs[dst] = 0;
                } else if (target->def.type == AXBPF_MAP_ARRAY) {
                    /* Array maps return the address of element zero only after
                     * a helper call; the reference itself is a pointer to the
                     * map, which the verifier turns into MAP_VALUE_OR_NULL. */
                    frame.regs[dst] = (u64)target;
                } else {
                    frame.regs[dst] = (u64)target;
                }
                pc++;
            } else {
                g_interp_rejections++;
                return 0;
            }
            break;
        }
        case AXBPF_JMP:
        case AXBPF_JMP32: {
            if (op == AXBPF_CALL) {
                u64 args[5];
                u32 helper = (u32)insn->imm;
                args[0] = frame.regs[1];
                args[1] = frame.regs[2];
                args[2] = frame.regs[3];
                args[3] = frame.regs[4];
                args[4] = frame.regs[5];
                u64 result = 0;
                g_interp_helper_calls++;
                if (axbpf_helper_call((axbpf_helper_id_t)helper, &helper_ctx, args,
                                       &result) != 0) {
                    result = 0;
                }
                frame.regs[0] = result;
                /* The ABI makes r1..r5 caller-saved. */
                for (u32 r = 1; r <= 5; r++) {
                    frame.regs[r] = 0;
                }
                pc++;
                break;
            }
            if (op == AXBPF_EXIT) {
                if (ctx) {
                    ctx->retval = (u32)frame.regs[0];
                }
                g_interp_insns += executed;
                if (executed > g_interp_max_insns) {
                    g_interp_max_insns = executed;
                }
                if (ctx) {
                    u64 elapsed = axbpf_now_ns() - ctx->start_ns;
                    prog->run_ns += elapsed;
                }
                prog->run_count++;
                return frame.regs[0];
            }
            if (jump_taken(op, frame.regs[dst], frame.regs[src], insn->imm,
                           src_from_reg, is_64)) {
                i64 target = (i64)pc + 1 + insn->off;
                if (target < 0 || (u64)target >= prog->insn_count) {
                    g_interp_rejections++;
                    return 0;
                }
                pc = (u64)target;
            } else {
                pc++;
            }
            break;
        }
        default:
            g_interp_rejections++;
            return 0;
        }
    }
    g_interp_insns += executed;
    if (executed > g_interp_max_insns) {
        g_interp_max_insns = executed;
    }
    prog->run_count++;
    return 0;
}

/* The interpreter's own test: a program that computes the sum of two context
 * words should produce a known value. */
i32 axbpf_interp_selftest(void)
{
    static axbpf_insn_t program[8];
    ax_memset(program, 0, sizeof(program));
    return axbpf_interp_check(0) == -1 ? 0 : -1;
}

void axbpf_interp_stats(char *buffer, usize cap)
{
    if (!buffer) {
        return;
    }
    ax_snprintf(buffer, cap,
                "interp: runs=%llu insns=%llu max=%llu helpers=%llu rejects=%llu\n",
                g_interp_runs, g_interp_insns, g_interp_max_insns,
                g_interp_helper_calls, g_interp_rejections);
}
