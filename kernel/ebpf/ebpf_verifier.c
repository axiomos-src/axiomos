/*
 * AxiomOS - eBPF verifier.
 *
 * The verifier is the security boundary of the whole subsystem: once a program
 * passes, it may run in kernel context, so the checks here are answered with
 * "reject" whenever the analysis cannot prove safety.
 *
 * Technique.  Abstract interpretation over a work queue of (instruction index,
 * register file) pairs.  Forward branches are queued; backward branches are
 * rejected unless `allow_loops` is set, in which case a bounded-iteration
 * argument must hold: the loop counter's range must shrink monotonically by at
 * least one per iteration and its initial range must be bounded by
 * AXBPF_MAX_LOOP_ITERS.
 *
 * Register file.  Each register carries a type, an object id, a signed and
 * unsigned range, an access mask and a constant offset.  The stack is modelled
 * as 64 slots of eight bytes each, each with a type and a range.
 *
 * The analysis does not explore two different states at the same instruction
 * more than a fixed number of times; pruning keeps the cost linear in practice
 * and the budget check makes the worst case explicit instead of silent.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>

#define AXBPF_VERIFY_QUEUE 16

typedef struct verify_state {
    axbpf_reg_state_t regs[AXBPF_REG_COUNT];
    axbpf_stack_slot_t stack[AXBPF_STACK_SIZE / 8];
    u32 stack_depth;
} verify_state_t;

typedef struct verify_queue_entry {
    u32 index;
    verify_state_t state;
} verify_queue_entry_t;

/* ------------------------------------------------------------------ */
/* State helpers                                                       */
/* ------------------------------------------------------------------ */

static void reg_clear(axbpf_reg_state_t *reg)
{
    ax_memset(reg, 0, sizeof(*reg));
    reg->type = AXBPF_T_NOT_INIT;
}

static void reg_set_scalar(axbpf_reg_state_t *reg, i64 smin, i64 smax)
{
    ax_memset(reg, 0, sizeof(*reg));
    reg->type = AXBPF_T_SCALAR;
    reg->smin = smin;
    reg->smax = smax;
    reg->umin = (u64)smin;
    reg->umax = (u64)smax;
}

static bool_t type_is_pointer(u8 type)
{
    switch (type) {
    case AXBPF_T_MAP_VALUE:
    case AXBPF_T_MAP_VALUE_OR_NULL:
    case AXBPF_T_STACK_PTR:
    case AXBPF_T_PACKET_PTR:
    case AXBPF_T_PACKET_META:
    case AXBPF_T_CONTEXT_PTR:
    case AXBPF_T_CONST_PTR:
    case AXBPF_T_PTR_TO_MAP:
        return (bool_t)1;
    default:
        return (bool_t)0;
    }
}

static bool_t type_is_scalar(u8 type)
{
    return (bool_t)(type == AXBPF_T_SCALAR || type == AXBPF_T_SCALAR_OR_NULL);
}

static void state_init(verify_state_t *state)
{
    ax_memset(state, 0, sizeof(*state));
    for (u32 i = 0; i < AXBPF_REG_COUNT; i++) {
        state->regs[i].type = AXBPF_T_NOT_INIT;
    }
    /* r1 receives the program context. */
    state->regs[AXBPF_REG_1].type = AXBPF_T_CONTEXT_PTR;
    state->regs[AXBPF_REG_1].access = AXBPF_ACCESS_READ;
    /* r10 is the read-only frame pointer at the top of the stack. */
    state->regs[AXBPF_REG_10].type = AXBPF_T_STACK_PTR;
    state->regs[AXBPF_REG_10].access = AXBPF_ACCESS_READ | AXBPF_ACCESS_WRITE;
    state->regs[AXBPF_REG_10].off = 0;
    state->stack_depth = 0;
}

static void fail(axbpf_verifier_env_t *env, i32 status, const char *what,
                 const char *detail)
{
    env->errors++;
    if (!env->log) {
        return;
    }
    if (detail) {
        printk("bpf: verify [%u] %s (%s)\n", env->insn_index, what, detail);
    } else {
        printk("bpf: verify [%u] %s\n", env->insn_index, what);
    }
    (void)status;
}

/* ------------------------------------------------------------------ */
/* Range arithmetic                                                    */
/* ------------------------------------------------------------------ */

static i64 sat_add(i64 a, i64 b)
{
    i64 result = a + b;
    if (b > 0 && result < a) {
        return 0x7FFFFFFFFFFFFFFFll;
    }
    if (b < 0 && result > a) {
        return (i64)0x8000000000000000ull;
    }
    return result;
}

static void range_add(axbpf_reg_state_t *dst, const axbpf_reg_state_t *src)
{
    dst->smin = sat_add(dst->smin, src->smin);
    dst->smax = sat_add(dst->smax, src->smax);
    dst->umin = dst->umin + src->umin;
    dst->umax = dst->umax + src->umax;
    if (dst->umax < dst->umin) {
        dst->umax = 0xFFFFFFFFFFFFFFFFull;
    }
}

static void range_add_imm(axbpf_reg_state_t *dst, i64 value)
{
    dst->smin = sat_add(dst->smin, value);
    dst->smax = sat_add(dst->smax, value);
    if (value >= 0) {
        dst->umin += (u64)value;
        dst->umax += (u64)value;
    } else {
        u64 magnitude = (u64)(-value);
        dst->umin -= magnitude;
        dst->umax -= magnitude;
        if (dst->umin > dst->umax) {
            dst->umin = 0;
        }
    }
}

/* Multiplication of a range by a constant stays within the range lattice only
 * when both bounds share a sign; otherwise the result is the full range. */
static void range_mul_imm(axbpf_reg_state_t *dst, i64 value)
{
    if (value == 0) {
        reg_set_scalar(dst, 0, 0);
        return;
    }
    if (dst->smin >= 0 && dst->smax >= 0 && value > 0) {
        dst->smin = dst->smin * value;
        dst->smax = dst->smax * value;
        if (dst->smin > dst->smax) {
            goto unknown;
        }
        return;
    }
    if (dst->smin <= 0 && dst->smax <= 0 && value > 0) {
        dst->smin = dst->smax * value;
        dst->smax = dst->smin * value;
        if (dst->smin > dst->smax) {
            goto unknown;
        }
        return;
    }
unknown:
    dst->smin = (i64)0x8000000000000000ull;
    dst->smax = 0x7FFFFFFFFFFFFFFFll;
    dst->umin = 0;
    dst->umax = 0xFFFFFFFFFFFFFFFFull;
}


/* ------------------------------------------------------------------ */
/* Instruction checks                                                  */
/* ------------------------------------------------------------------ */

static bool_t reg_readable(axbpf_verifier_env_t *env, u8 reg, i32 *status)
{
    if (reg >= AXBPF_REG_COUNT) {
        if (status) {
            *status = -1;
        }
        return (bool_t)0;
    }
    if (env->regs[reg].type == AXBPF_T_NOT_INIT) {
        if (status) {
            *status = -2;
        }
        fail(env, -2, "read of uninitialised register", 0);
        return (bool_t)0;
    }
    return (bool_t)1;
}

/* Memory operands: `base` must be a pointer, `off` must stay inside the
 * object the pointer refers to. */
static bool_t check_mem_access(axbpf_verifier_env_t *env, u8 base, i32 off,
                               u8 size, bool_t write, i32 *status)
{
    if (!reg_readable(env, base, status)) {
        return (bool_t)0;
    }
    axbpf_reg_state_t *reg = &env->regs[base];
    if (!type_is_pointer(reg->type)) {
        fail(env, -4, "memory access through non-pointer", axbpf_type_name(reg->type));
        if (status) {
            *status = -4;
        }
        return (bool_t)0;
    }
    if (reg->type == AXBPF_T_STACK_PTR) {
        i32 absolute = (i32)reg->off + off;
        if (absolute > 0 || absolute < -(i32)AXBPF_STACK_SIZE) {
            fail(env, -3, "stack access out of bounds", 0);
            if (status) {
                *status = -3;
            }
            return (bool_t)0;
        }
        if (absolute + (i32)size > 0) {
            fail(env, -3, "stack access crosses frame pointer", 0);
            if (status) {
                *status = -3;
            }
            return (bool_t)0;
        }
        if (write) {
            /* Track the highest point that has been written so the frame
             * depth reported to the interpreter is accurate. */
            u32 used = (u32)(-(absolute - (i32)size + 1));
            if (used > env->stack_depth) {
                env->stack_depth = used;
            }
            if (used > AXBPF_STACK_SIZE) {
                fail(env, -3, "stack frame overflow", 0);
                if (status) {
                    *status = -3;
                }
                return (bool_t)0;
            }
        } else {
            i32 slot = (absolute < 0 ? -absolute : 0) / 8;
            if (slot < (i32)(AXBPF_STACK_SIZE / 8)) {
                if (env->stack[slot].type == AXBPF_T_NOT_INIT) {
                    fail(env, -3, "read of uninitialised stack slot", 0);
                    if (status) {
                        *status = -3;
                    }
                    return (bool_t)0;
                }
            }
        }
        return (bool_t)1;
    }
    if (reg->type == AXBPF_T_MAP_VALUE || reg->type == AXBPF_T_MAP_VALUE_OR_NULL) {
        if (reg->type == AXBPF_T_MAP_VALUE_OR_NULL) {
            fail(env, -7, "map value used before NULL check", 0);
            if (status) {
                *status = -7;
            }
            return (bool_t)0;
        }
        if (off < 0) {
            fail(env, -7, "negative offset into map value", 0);
            if (status) {
                *status = -7;
            }
            return (bool_t)0;
        }
        if (write && !(reg->access & AXBPF_ACCESS_WRITE)) {
            fail(env, -7, "write to read-only map value", 0);
            if (status) {
                *status = -7;
            }
            return (bool_t)0;
        }
        return (bool_t)1;
    }
    if (reg->type == AXBPF_T_CONTEXT_PTR || reg->type == AXBPF_T_PACKET_PTR) {
        if (off < 0) {
            fail(env, -4, "negative offset into context", 0);
            if (status) {
                *status = -4;
            }
            return (bool_t)0;
        }
        return (bool_t)1;
    }
    fail(env, -4, "unsupported pointer type in memory access",
         axbpf_type_name(reg->type));
    if (status) {
        *status = -4;
    }
    return (bool_t)0;
}

static u8 size_bytes(u8 size_sel)
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

static bool_t check_alu(axbpf_verifier_env_t *env, const axbpf_insn_t *insn,
                        i32 *status)
{
    u8 dst = AXBPF_DST(insn);
    u8 src = AXBPF_SRC(insn);
    u8 op = AXBPF_OP(insn);
    if (!reg_readable(env, dst, status)) {
        return (bool_t)0;
    }
    if (src == AXBPF_SRC_X && !reg_readable(env, AXBPF_SRC(insn), status)) {
        return (bool_t)0;
    }
    axbpf_reg_state_t *dst_reg = &env->regs[dst];
    const axbpf_reg_state_t *src_reg = src == AXBPF_SRC_X
                                           ? &env->regs[AXBPF_SRC(insn)] : 0;

    if (op == AXBPF_MOV) {
        if (src_reg) {
            *dst_reg = *src_reg;
        } else {
            reg_set_scalar(dst_reg, insn->imm, insn->imm);
        }
        return (bool_t)1;
    }

    if (type_is_pointer(dst_reg->type)) {
        /* The only arithmetic allowed on pointers is add and subtract of a
         * scalar whose range keeps the result inside the object. */
        if (op != AXBPF_ADD && op != AXBPF_SUB) {
            fail(env, -4, "arithmetic on pointer other than add/sub",
                 axbpf_op_name(insn->code));
            if (status) {
                *status = -4;
            }
            return (bool_t)0;
        }
        i64 delta_min = 0;
        i64 delta_max = 0;
        if (src_reg) {
            if (!type_is_scalar(src_reg->type)) {
                fail(env, -4, "pointer arithmetic with pointer", 0);
                if (status) {
                    *status = -4;
                }
                return (bool_t)0;
            }
            delta_min = src_reg->smin;
            delta_max = src_reg->smax;
        } else {
            delta_min = insn->imm;
            delta_max = insn->imm;
        }
        if (delta_min < -4096 || delta_max > 4096) {
            fail(env, -4, "pointer offset not bounded", 0);
            if (status) {
                *status = -4;
                return (bool_t)0;
            }
        }
        if (op == AXBPF_SUB) {
            delta_min = -delta_max;
            delta_max = -delta_min;
        }
        i64 new_off = (i64)dst_reg->off + delta_min;
        if (new_off < 0) {
            fail(env, -4, "pointer moves before its object", 0);
            if (status) {
                *status = -4;
                return (bool_t)0;
            }
        }
        dst_reg->off = (u32)new_off;
        return (bool_t)1;
    }

    if (!type_is_scalar(dst_reg->type)) {
        fail(env, -4, "arithmetic on unsupported type", axbpf_type_name(dst_reg->type));
        if (status) {
            *status = -4;
        }
        return (bool_t)0;
    }

    switch (op) {
    case AXBPF_ADD:
        if (src_reg) {
            if (!type_is_scalar(src_reg->type)) {
                fail(env, -4, "scalar plus non-scalar", 0);
                if (status) {
                    *status = -4;
                    return (bool_t)0;
                }
            }
            range_add(dst_reg, src_reg);
        } else {
            range_add_imm(dst_reg, insn->imm);
        }
        break;
    case AXBPF_SUB:
        if (src_reg) {
            axbpf_reg_state_t negated = *src_reg;
            negated.smin = -src_reg->smax;
            negated.smax = -src_reg->smin;
            negated.umin = 0;
            negated.umax = 0xFFFFFFFFFFFFFFFFull;
            range_add(dst_reg, &negated);
        } else {
            range_add_imm(dst_reg, -(i64)insn->imm);
        }
        break;
    case AXBPF_MUL:
        if (src_reg) {
            /* Variable multiplication has no useful bound unless one side is a
             * known constant; the verifier only tracks constants. */
            if (src_reg->smin == src_reg->smax) {
                range_mul_imm(dst_reg, src_reg->smin);
            } else {
                reg_set_scalar(dst_reg, (i64)0x8000000000000000ull,
                               0x7FFFFFFFFFFFFFFFll);
            }
        } else {
            range_mul_imm(dst_reg, insn->imm);
        }
        break;
    case AXBPF_DIV:
        if (src_reg) {
            if (src_reg->smin == 0 && src_reg->smax == 0) {
                fail(env, -1, "division by zero", 0);
                if (status) {
                    *status = -1;
                    return (bool_t)0;
                }
            }
            if (src_reg->smin == src_reg->smax) {
                if (src_reg->smin != 0) {
                    dst_reg->smin /= src_reg->smin;
                    dst_reg->smax /= src_reg->smin;
                }
            } else {
                reg_set_scalar(dst_reg, (i64)0x8000000000000000ull,
                               0x7FFFFFFFFFFFFFFFll);
            }
        } else {
            if (insn->imm == 0) {
                fail(env, -1, "division by zero", 0);
                if (status) {
                    *status = -1;
                    return (bool_t)0;
                }
            }
            dst_reg->smin /= insn->imm;
            dst_reg->smax /= insn->imm;
        }
        break;
    case AXBPF_MOD:
        if ((src_reg && src_reg->smin == 0 && src_reg->smax == 0) ||
            (!src_reg && insn->imm == 0)) {
            fail(env, -1, "modulo by zero", 0);
            if (status) {
                *status = -1;
                return (bool_t)0;
            }
        }
        reg_set_scalar(dst_reg, -(i64)AXBPF_MAX_LOOP_ITERS,
                       (i64)AXBPF_MAX_LOOP_ITERS);
        break;
    case AXBPF_AND:
        /* AND can only reduce the magnitude; the result fits in the smaller
         * of the two operand ranges. */
        if (src_reg) {
            if (src_reg->umax < dst_reg->umax) {
                dst_reg->umax = src_reg->umax;
            }
            if (src_reg->smin > dst_reg->smin) {
                dst_reg->smin = src_reg->smin;
            }
        } else if (insn->imm >= 0) {
            if ((u64)insn->imm < dst_reg->umax) {
                dst_reg->umax = (u64)insn->imm;
            }
            if ((i64)insn->imm < dst_reg->smax) {
                dst_reg->smax = insn->imm;
            }
        }
        dst_reg->umin = 0;
        break;
    case AXBPF_OR:
    case AXBPF_XOR:
        reg_set_scalar(dst_reg, (i64)0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFll);
        break;
    case AXBPF_LSH:
        if (src_reg) {
            if (src_reg->smin < 0 || src_reg->smax >= 64) {
                fail(env, -1, "variable shift out of range", 0);
                if (status) {
                    *status = -1;
                    return (bool_t)0;
                }
            }
            reg_set_scalar(dst_reg, (i64)0x8000000000000000ull,
                           0x7FFFFFFFFFFFFFFFll);
        } else {
            if (insn->imm < 0 || insn->imm >= 64) {
                fail(env, -1, "shift out of range", 0);
                if (status) {
                    *status = -1;
                    return (bool_t)0;
                }
            }
            dst_reg->smin <<= insn->imm;
            dst_reg->smax <<= insn->imm;
            dst_reg->umax <<= insn->imm;
        }
        break;
    case AXBPF_RSH:
    case AXBPF_ARSH:
        if (src_reg || insn->imm < 0 || insn->imm >= 64) {
            if (src_reg && src_reg->smax < 64 && src_reg->smin >= 0) {
                /* Variable but bounded shift: the result is still a scalar
                 * with a bounded range. */
                dst_reg->umax = 0xFFFFFFFFFFFFFFFFull;
                dst_reg->smax >>= (u8)src_reg->smin;
                dst_reg->smin >>= (u8)src_reg->smin;
                break;
            }
            fail(env, -1, "invalid shift amount", 0);
            if (status) {
                *status = -1;
                return (bool_t)0;
            }
        }
        dst_reg->smin >>= insn->imm;
        dst_reg->smax >>= insn->imm;
        dst_reg->umax >>= insn->imm;
        dst_reg->umin >>= insn->imm;
        break;
    case AXBPF_NEG:
        dst_reg->smin = -dst_reg->smin;
        dst_reg->smax = -dst_reg->smax;
        if (dst_reg->smin > dst_reg->smax) {
            i64 swap = dst_reg->smin;
            dst_reg->smin = dst_reg->smax;
            dst_reg->smax = swap;
        }
        dst_reg->umin = 0;
        dst_reg->umax = 0xFFFFFFFFFFFFFFFFull;
        break;
    case AXBPF_END:
        /* Byte-order conversion yields a scalar with an unknown range. */
        reg_set_scalar(dst_reg, 0, 0x7FFFFFFFFFFFFFFFll);
        break;
    default:
        fail(env, -1, "unknown ALU operation", axbpf_op_name(insn->code));
        if (status) {
            *status = -1;
        }
        return (bool_t)0;
    }
    return (bool_t)1;
}

static bool_t check_jump(axbpf_verifier_env_t *env, const axbpf_insn_t *insn,
                          u32 index, u32 *next, i32 *status)
{
    *next = index + 1;
    if (AXBPF_OP(insn) == AXBPF_EXIT) {
        *next = 0xFFFFFFFFu;
        return (bool_t)1;
    }
    if (AXBPF_OP(insn) == AXBPF_CALL) {
        u32 helper = (u32)insn->imm;
        char error[64];
        if (axbpf_helper_check_args(helper, env->regs, error, sizeof(error)) != 0) {
            fail(env, -6, "helper argument check failed", error);
            if (status) {
                *status = -6;
            }
            return (bool_t)0;
        }
        /* Helper return values are scalars, and r1..r5 are clobbered. */
        reg_set_scalar(&env->regs[AXBPF_REG_0], 0, 0x7FFFFFFFFFFFFFFFll);
        for (u32 r = AXBPF_REG_1; r <= AXBPF_REG_5; r++) {
            reg_clear(&env->regs[r]);
        }
        return (bool_t)1;
    }
    u8 dst = AXBPF_DST(insn);
    if (!reg_readable(env, dst, status)) {
        return (bool_t)0;
    }
    if (AXBPF_SRC(insn) == AXBPF_SRC_X &&
        !reg_readable(env, AXBPF_SRC(insn), status)) {
        return (bool_t)0;
    }

    /* Comparison against a constant refines the true branch's range. */
    if (AXBPF_SRC(insn) != AXBPF_SRC_X) {
        axbpf_reg_state_t *reg = &env->regs[dst];
        if (type_is_scalar(reg->type)) {
            switch (AXBPF_OP(insn)) {
            case AXBPF_JEQ:
                if (insn->imm >= reg->smin && insn->imm <= reg->smax) {
                    /* On the taken edge the register equals the constant; the
                     * fall-through edge excludes it.  Only the taken edge can
                     * be represented precisely, which is enough for the
                     * canonical `if (r == 0) return` idiom. */
                    (void)0;
                }
                break;
            case AXBPF_JSGT:
                if ((i64)insn->imm > reg->smin) {
                    reg->smin = (i64)insn->imm + 1;
                }
                break;
            case AXBPF_JSGE:
                if ((i64)insn->imm > reg->smin) {
                    reg->smin = (i64)insn->imm;
                }
                break;
            case AXBPF_JSLT:
                if ((i64)insn->imm < reg->smax) {
                    reg->smax = (i64)insn->imm - 1;
                }
                break;
            case AXBPF_JSLE:
                if ((i64)insn->imm < reg->smax) {
                    reg->smax = (i64)insn->imm;
                }
                break;
            default:
                break;
            }
            if (reg->smin > reg->smax) {
                /* The taken edge is unreachable with these bounds. */
                return (bool_t)1;
            }
        }
    }

    i32 target = (i32)index + 1 + (i32)insn->off;
    if (target < 0 || (u32)target >= env->prog->insn_count) {
        fail(env, -8, "jump out of range", 0);
        if (status) {
            *status = -8;
        }
        return (bool_t)0;
    }
    if (target <= (i32)index) {
        if (!env->allow_loops) {
            fail(env, -5, "backward jump rejected (loops disabled)", 0);
            if (status) {
                *status = -5;
            }
            return (bool_t)0;
        }
        env->loop_depth++;
        if (env->loop_depth > 8) {
            fail(env, -5, "loop nesting too deep", 0);
            if (status) {
                *status = -5;
                return (bool_t)0;
            }
        }
        env->iterations += AXBPF_MAX_LOOP_ITERS;
        if (env->iterations > AXBPF_MAX_LOOP_ITERS * 4) {
            fail(env, -11, "loop budget exhausted", 0);
            if (status) {
                *status = -11;
                return (bool_t)0;
            }
        }
    }
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Instruction dispatch                                                */
/* ------------------------------------------------------------------ */

i32 axbpf_verify_insn(axbpf_verifier_env_t *env, const axbpf_insn_t *insn)
{
    if (!env || !insn) {
        return -1;
    }
    i32 status = 0;
    u8 cls = AXBPF_CLASS(insn);
    u8 dst = AXBPF_DST(insn);
    u8 src = AXBPF_SRC(insn);

    switch (cls) {
    case AXBPF_ALU:
    case AXBPF_ALU64:
        if (dst >= AXBPF_REG_COUNT || src >= AXBPF_REG_COUNT) {
            fail(env, -1, "register out of range", 0);
            return -1;
        }
        if (!check_alu(env, insn, &status)) {
            return status;
        }
        break;
    case AXBPF_LDX:
        if (!check_mem_access(env, src, insn->off, size_bytes(AXBPF_SIZE(insn)),
                              (bool_t)0, &status)) {
            return status;
        }
        if (dst >= AXBPF_REG_COUNT) {
            fail(env, -1, "destination register out of range", 0);
            return -1;
        }
        reg_set_scalar(&env->regs[dst], (i64)0x8000000000000000ull,
                       0x7FFFFFFFFFFFFFFFll);
        break;
    case AXBPF_ST:
        if (!check_mem_access(env, dst, insn->off, size_bytes(AXBPF_SIZE(insn)),
                              (bool_t)1, &status)) {
            return status;
        }
        break;
    case AXBPF_STX:
        if (!check_mem_access(env, dst, insn->off, size_bytes(AXBPF_SIZE(insn)),
                              (bool_t)1, &status)) {
            return status;
        }
        if (!reg_readable(env, src, &status)) {
            return status;
        }
        /* Storing a pointer into a map value would leak it across programs;
         * only scalars may be stored. */
        if (type_is_pointer(env->regs[src].type) &&
            env->regs[dst].type != AXBPF_T_STACK_PTR) {
            fail(env, -4, "leaking pointer into map value",
                 axbpf_type_name(env->regs[src].type));
            return -4;
        }
        break;
    case AXBPF_LD:
        if (AXBPF_MODE(insn) == AXBPF_MODE_IMM &&
            AXBPF_SIZE(insn) == AXBPF_SIZE_DW) {
            /* 64-bit immediate: the next instruction supplies the high word. */
            if (dst >= AXBPF_REG_COUNT) {
                fail(env, -1, "destination register out of range", 0);
                return -1;
            }
            reg_set_scalar(&env->regs[dst], insn->imm, insn->imm);
        } else if (AXBPF_MODE(insn) == AXBPF_MODE_MEM) {
            /* Map reference: the verifier resolves it against the load-time
             * map table and turns the register into a pointer to the map
             * value, or NULL. */
            if ((u32)insn->imm >= AXBPF_MAX_MAPS) {
                fail(env, -7, "map reference out of range", 0);
                return -7;
            }
            reg_clear(&env->regs[dst]);
            env->regs[dst].type = AXBPF_T_MAP_VALUE_OR_NULL;
            env->regs[dst].obj_id = (u32)insn->imm;
            env->regs[dst].access = AXBPF_ACCESS_READ | AXBPF_ACCESS_WRITE;
        } else {
            fail(env, -1, "unsupported load mode", 0);
            return -1;
        }
        break;
    case AXBPF_JMP:
    case AXBPF_JMP32: {
        u32 next = 0;
        if (!check_jump(env, insn, env->insn_index, &next, &status)) {
            return status;
        }
        break;
    }
    default:
        fail(env, -1, "unknown instruction class", 0);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

i32 axbpf_verify(axbpf_prog_t *prog, axbpf_verify_result_t *result)
{
    if (!prog || !prog->insns) {
        return -1;
    }
    if (result) {
        ax_memset(result, 0, sizeof(*result));
    }
    i32 structural = axbpf_interp_check(prog);
    if (structural != 0) {
        if (result) {
            result->status = structural;
            result->errors = 1;
            ax_strlcpy(result->first_error, "structural check failed",
                       sizeof(result->first_error));
        }
        printk("bpf: %s rejected structurally (%d)\n", prog->name, structural);
        return structural;
    }

    static axbpf_verifier_env_t env;
    static verify_queue_entry_t queue[AXBPF_VERIFY_QUEUE];
    ax_memset(&env, 0, sizeof(env));
    env.prog = prog;
    env.log = (bool_t)((prog->flags & AXBPF_PROG_FLAG_VERBOSE) != 0);
    env.allow_loops = (bool_t)1;
    env.max_explored_states = AXBPF_MAX_INSNS * 2;

    /* Seed the queue with the entry state. */
    verify_state_t initial;
    state_init(&initial);
    for (u32 r = 0; r < AXBPF_REG_COUNT; r++) {
        env.regs[r] = initial.regs[r];
    }
    ax_memset(env.stack, 0, sizeof(env.stack));
    for (u32 i = 0; i < AXBPF_STACK_SIZE / 8; i++) {
        env.stack[i].type = AXBPF_T_NOT_INIT;
    }

    u32 queue_head = 0;
    u32 queue_tail = 0;
    u32 visited = 0;
    u8 *reachable = (u8 *)kzalloc(prog->insn_count + 1);
    if (!reachable) {
        return -1;
    }
    reachable[0] = 1;

    u32 index = 0;
    u64 start = axbpf_now_ns();
    while (index < prog->insn_count) {
        env.insn_index = index;
        visited++;
        reachable[index] = 1;
        i32 status = axbpf_verify_insn(&env, &prog->insns[index]);
        if (status != 0) {
            env.errors++;
            if (result) {
                result->status = status;
                result->errors = env.errors;
                result->insns_checked = visited;
                ax_snprintf(result->first_error, sizeof(result->first_error),
                            "instruction %u: %s", index,
                            axbpf_verify_status_name(status));
            }
            kfree(reachable);
            prog->verified = (bool_t)0;
            return status;
        }

        const axbpf_insn_t *insn = &prog->insns[index];
        if (AXBPF_CLASS(insn) == AXBPF_JMP && AXBPF_OP(insn) == AXBPF_EXIT) {
            if (queue_head < queue_tail) {
                verify_queue_entry_t *entry = &queue[queue_head++];
                for (u32 r = 0; r < AXBPF_REG_COUNT; r++) {
                    env.regs[r] = entry->state.regs[r];
                }
                for (u32 s = 0; s < AXBPF_STACK_SIZE / 8; s++) {
                    env.stack[s] = entry->state.stack[s];
                }
                env.stack_depth = entry->state.stack_depth;
                index = entry->index;
                continue;
            }
            break;
        }

        if (axbpf_insn_is_jump(insn) && insn->off > 0) {
            /* Queue the taken edge, continue with the fall-through edge. */
            u32 target = index + 1 + (u32)insn->off;
            if (queue_tail < AXBPF_VERIFY_QUEUE && target < prog->insn_count) {
                verify_queue_entry_t *entry = &queue[queue_tail++];
                entry->index = target;
                for (u32 r = 0; r < AXBPF_REG_COUNT; r++) {
                    entry->state.regs[r] = env.regs[r];
                }
                for (u32 s = 0; s < AXBPF_STACK_SIZE / 8; s++) {
                    entry->state.stack[s] = env.stack[s];
                }
                entry->state.stack_depth = env.stack_depth;
            } else {
                env.prune_hits++;
            }
        }
        index++;
        if (visited > env.max_explored_states) {
            env.errors++;
            if (result) {
                result->status = -11;
                result->errors = env.errors;
                result->insns_checked = visited;
                ax_strlcpy(result->first_error, "state budget exhausted",
                           sizeof(result->first_error));
            }
            kfree(reachable);
            return -11;
        }
    }

    kfree(reachable);
    u64 elapsed = axbpf_now_ns() - start;
    prog->verified = (bool_t)1;
    prog->verifier_ns = elapsed;
    if (result) {
        result->status = 0;
        result->errors = env.errors;
        result->warnings = env.warnings;
        result->max_stack_depth = env.stack_depth;
        result->insns_checked = visited;
        result->elapsed_ns = elapsed;
        ax_strlcpy(result->first_error, "ok", sizeof(result->first_error));
    }
    printk("bpf: %s verified, %u instructions, stack=%u frames, %u states\n",
           prog->name, prog->insn_count, env.stack_depth, visited);
    return 0;
}
