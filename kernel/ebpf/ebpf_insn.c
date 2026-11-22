/*
 * AxiomOS - eBPF instruction utilities: naming, disassembly, analysis.
 *
 * Everything here works on a single instruction and has no side effects, which
 * makes it usable from the verifier, the JIT, the tracer's log path and the
 * shell's `bpf dis` command.
 *
 * The decoder's job is to answer precise questions about one instruction:
 * which class is it, does it write a register, how much stack does it touch,
 * can it branch.  Guessing here would make the verifier unsound, so every
 * helper returns "unknown" rather than a plausible answer when the opcode does
 * not fit a known shape.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>

/* ------------------------------------------------------------------ */
/* Name tables                                                         */
/* ------------------------------------------------------------------ */

static const char *const g_class_names[] = {
    "ld", "ldx", "st", "stx", "alu", "jmp", "jmp32", "alu64"
};

static const char *const g_alu_names[] = {
    "add", "sub", "mul", "div", "or", "and", "lsh", "rsh",
    "neg", "mod", "xor", "mov", "arsh", "end"
};

static const char *const g_jmp_names[] = {
    "ja", "jeq", "jgt", "jge", "jset", "jne", "jsgt", "jsge",
    "call", "exit", "jlt", "jle", "jslt", "jsle"
};

const char *axbpf_class_name(u8 code)
{
    u8 cls = (u8)(code & 0x07);
    if (cls >= sizeof(g_class_names) / sizeof(g_class_names[0])) {
        return "?";
    }
    return g_class_names[cls];
}

const char *axbpf_op_name(u8 code)
{
    u8 cls = (u8)(code & 0x07);
    u8 op = (u8)(code & 0xF0);
    u8 index = (u8)(op >> 4);
    switch (cls) {
    case AXBPF_ALU:
    case AXBPF_ALU64:
        if (index < sizeof(g_alu_names) / sizeof(g_alu_names[0])) {
            return g_alu_names[index];
        }
        return "?alu";
    case AXBPF_JMP:
    case AXBPF_JMP32:
        if (index < sizeof(g_jmp_names) / sizeof(g_jmp_names[0])) {
            return g_jmp_names[index];
        }
        return "?jmp";
    case AXBPF_LD:
    case AXBPF_LDX:
        return "load";
    case AXBPF_ST:
    case AXBPF_STX:
        return "store";
    default:
        return "?";
    }
}

const char *axbpf_type_name(u8 type)
{
    switch (type) {
    case AXBPF_T_NOT_INIT:
        return "not_init";
    case AXBPF_T_MAP_VALUE:
        return "map_value";
    case AXBPF_T_MAP_VALUE_OR_NULL:
        return "map_value_or_null";
    case AXBPF_T_STACK_PTR:
        return "stack_ptr";
    case AXBPF_T_PACKET_PTR:
        return "packet_ptr";
    case AXBPF_T_PACKET_META:
        return "packet_meta";
    case AXBPF_T_CONTEXT_PTR:
        return "context_ptr";
    case AXBPF_T_SCALAR:
        return "scalar";
    case AXBPF_T_SCALAR_OR_NULL:
        return "scalar_or_null";
    case AXBPF_T_PTR_TO_MAP:
        return "ptr_to_map";
    case AXBPF_T_CONST_PTR:
        return "const_ptr";
    case AXBPF_T_MEM_ALLOC:
        return "mem_alloc";
    default:
        return "unknown";
    }
}

const char *axbpf_verify_status_name(i32 status)
{
    switch (status) {
    case 0:
        return "ok";
    case -1:
        return "invalid instruction";
    case -2:
        return "uninitialised register";
    case -3:
        return "invalid stack access";
    case -4:
        return "pointer arithmetic";
    case -5:
        return "unbounded loop";
    case -6:
        return "invalid helper call";
    case -7:
        return "invalid map access";
    case -8:
        return "jump out of range";
    case -9:
        return "missing exit";
    case -10:
        return "too many instructions";
    case -11:
        return "verification budget exhausted";
    default:
        return "unknown failure";
    }
}

/* ------------------------------------------------------------------ */
/* Per-instruction analysis                                            */
/* ------------------------------------------------------------------ */

bool_t axbpf_insn_is_jump(const axbpf_insn_t *insn)
{
    if (!insn) {
        return (bool_t)0;
    }
    u8 cls = AXBPF_CLASS(insn);
    if (cls != AXBPF_JMP && cls != AXBPF_JMP32) {
        return (bool_t)0;
    }
    u8 op = AXBPF_OP(insn);
    return (bool_t)(op != AXBPF_CALL && op != AXBPF_EXIT);
}

bool_t axbpf_insn_writes_reg(const axbpf_insn_t *insn, u8 reg)
{
    if (!insn) {
        return (bool_t)0;
    }
    u8 dst = AXBPF_DST(insn);
    u8 cls = AXBPF_CLASS(insn);
    switch (cls) {
    case AXBPF_ALU:
    case AXBPF_ALU64:
    case AXBPF_LDX:
        return (bool_t)(dst == reg);
    case AXBPF_LD:
        /* The immediate form of LD writes dst; the abs/ind forms write r0. */
        if (AXBPF_MODE(insn) == AXBPF_MODE_IMM) {
            return (bool_t)(dst == reg);
        }
        return (bool_t)(reg == AXBPF_REG_0);
    case AXBPF_ST:
    case AXBPF_STX:
        return (bool_t)0;
    case AXBPF_JMP:
    case AXBPF_JMP32:
        if (AXBPF_OP(insn) == AXBPF_CALL) {
            /* Helpers clobber r0..r5 by the calling convention. */
            return (bool_t)(reg <= AXBPF_REG_5);
        }
        return (bool_t)0;
    default:
        return (bool_t)0;
    }
}

u32 axbpf_insn_stack_delta(const axbpf_insn_t *insn, u32 *depth)
{
    if (!insn || !depth) {
        return 0;
    }
    u8 cls = AXBPF_CLASS(insn);
    if ((cls == AXBPF_ST || cls == AXBPF_STX) &&
        AXBPF_DST(insn) == AXBPF_REG_10) {
        /* The offset is negative from the frame base.  Writing below the
         * current depth grows the frame. */
        i32 off = (i32)insn->off;
        if (off < 0) {
            u32 used = (u32)(-off);
            if (used > *depth) {
                *depth = used;
            }
        }
        return *depth;
    }
    return *depth;
}

u64 axbpf_hash_insns(const axbpf_insn_t *insns, u32 count)
{
    if (!insns) {
        return 0;
    }
    /* FNV-1a over the canonical eight-byte encoding, which gives programs a
     * stable identity independent of name and load order. */
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i < count; i++) {
        const axbpf_insn_t *insn = &insns[i];
        u8 bytes[8];
        bytes[0] = insn->code;
        bytes[1] = insn->dst_src;
        bytes[2] = (u8)(insn->off & 0xFF);
        bytes[3] = (u8)((insn->off >> 8) & 0xFF);
        bytes[4] = (u8)(insn->imm & 0xFF);
        bytes[5] = (u8)((insn->imm >> 8) & 0xFF);
        bytes[6] = (u8)((insn->imm >> 16) & 0xFF);
        bytes[7] = (u8)((insn->imm >> 24) & 0xFF);
        for (u32 b = 0; b < 8; b++) {
            hash ^= bytes[b];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

/* ------------------------------------------------------------------ */
/* Disassembly                                                         */
/* ------------------------------------------------------------------ */

static char *append(char *out, usize cap, const char *text)
{
    usize used = ax_strlen(out);
    if (used >= cap - 1) {
        return out;
    }
    usize room = cap - 1 - used;
    usize len = ax_strlen(text);
    if (len > room) {
        len = room;
    }
    for (usize i = 0; i < len; i++) {
        out[used + i] = text[i];
    }
    out[used + len] = 0;
    return out;
}

static void format_reg(char *out, usize cap, u8 reg)
{
    if (reg <= AXBPF_REG_10) {
        out[0] = 'r';
        if (reg == 10) {
            out[1] = '1';
            out[2] = '0';
            out[3] = 0;
        } else {
            out[1] = (char)('0' + reg);
            out[2] = 0;
        }
        return;
    }
    ax_strlcpy(out, "r?", cap);
}

static void format_imm(char *out, usize cap, i32 value)
{
    ax_snprintf(out, cap, "%d", value);
}

i32 axbpf_disasm(const axbpf_insn_t *insn, char *out, usize cap)
{
    if (!insn || !out || cap < 8) {
        return -1;
    }
    out[0] = 0;
    u8 cls = AXBPF_CLASS(insn);
    u8 op = AXBPF_OP(insn);
    u8 dst = AXBPF_DST(insn);
    u8 src = AXBPF_SRC(insn);

    char reg_dst[8];
    char reg_src[8];
    char imm[16];
    format_reg(reg_dst, sizeof(reg_dst), dst);
    format_reg(reg_src, sizeof(reg_src), src);

    switch (cls) {
    case AXBPF_ALU:
    case AXBPF_ALU64: {
        const char *name = axbpf_op_name(insn->code);
        append(out, cap, name);
        append(out, cap, " ");
        append(out, cap, reg_dst);
        append(out, cap, ", ");
        if (src == AXBPF_SRC_X) {
            append(out, cap, reg_src);
        } else {
            format_imm(imm, sizeof(imm), insn->imm);
            append(out, cap, imm);
        }
        /* MOV with a 64-bit immediate spans two instructions; note it so a
         * reader is not confused by the filler instruction that follows. */
        if (op == AXBPF_MOV && src == AXBPF_SRC_K && insn->imm == 0 &&
            cls == AXBPF_ALU64) {
            append(out, cap, " (ldimm64)");
        }
        break;
    }
    case AXBPF_JMP:
    case AXBPF_JMP32: {
        if (op == AXBPF_CALL) {
            append(out, cap, "call ");
            format_imm(imm, sizeof(imm), insn->imm);
            if (src == AXBPF_PSEUDO_CALL) {
                append(out, cap, " (bpf-to-bpf)");
            }
            break;
        }
        if (op == AXBPF_EXIT) {
            append(out, cap, "exit");
            break;
        }
        const char *name = axbpf_op_name(insn->code);
        append(out, cap, name);
        if (op != AXBPF_JA) {
            append(out, cap, " ");
            append(out, cap, reg_dst);
            append(out, cap, ", ");
            if (src == AXBPF_SRC_X) {
                append(out, cap, reg_src);
            } else {
                format_imm(imm, sizeof(imm), insn->imm);
                append(out, cap, imm);
            }
        }
        append(out, cap, ", +");
        format_imm(imm, sizeof(imm), (i32)insn->off);
        append(out, cap, imm);
        break;
    }
    case AXBPF_LD:
    case AXBPF_LDX: {
        u8 size = AXBPF_SIZE(insn);
        const char *width = size == AXBPF_SIZE_B ? "b"
                             : size == AXBPF_SIZE_H ? "h"
                             : size == AXBPF_SIZE_W ? "w" : "dw";
        append(out, cap, cls == AXBPF_LD ? "ld" : "ldx");
        append(out, cap, width);
        append(out, cap, " ");
        append(out, cap, reg_dst);
        append(out, cap, ", [");
        append(out, cap, reg_src);
        if (insn->off) {
            append(out, cap, insn->off > 0 ? "+" : "-");
            format_imm(imm, sizeof(imm), insn->off > 0 ? insn->off : -insn->off);
            append(out, cap, imm);
        }
        append(out, cap, "]");
        break;
    }
    case AXBPF_ST:
    case AXBPF_STX: {
        append(out, cap, cls == AXBPF_ST ? "st" : "stx");
        u8 size = AXBPF_SIZE(insn);
        append(out, cap, size == AXBPF_SIZE_B ? "b"
                          : size == AXBPF_SIZE_H ? "h"
                          : size == AXBPF_SIZE_W ? "w" : "dw");
        append(out, cap, " [");
        append(out, cap, reg_dst);
        if (insn->off) {
            append(out, cap, insn->off > 0 ? "+" : "-");
            format_imm(imm, sizeof(imm), insn->off > 0 ? insn->off : -insn->off);
            append(out, cap, imm);
        }
        append(out, cap, "], ");
        if (cls == AXBPF_STX) {
            append(out, cap, reg_src);
        } else {
            format_imm(imm, sizeof(imm), insn->imm);
            append(out, cap, imm);
        }
        break;
    }
    default:
        ax_strlcpy(out, "invalid", cap);
        return -1;
    }
    return 0;
}

void axbpf_disasm_prog(const axbpf_prog_t *prog, void (*emit)(const char *line))
{
    if (!prog || !prog->insns || !emit) {
        return;
    }
    char line[128];
    for (u32 i = 0; i < prog->insn_count; i++) {
        char text[96];
        if (axbpf_disasm(&prog->insns[i], text, sizeof(text)) != 0) {
            ax_strlcpy(text, "<invalid>", sizeof(text));
        }
        ax_snprintf(line, sizeof(line), "%4u: %02x %02x %04x %08x  %s", i,
                    prog->insns[i].code, prog->insns[i].dst_src,
                    (u32)(u16)prog->insns[i].off, (u32)prog->insns[i].imm,
                    text);
        emit(line);
    }
}

/* ------------------------------------------------------------------ */
/* Structural checks used before verification                          */
/* ------------------------------------------------------------------ */

i32 axbpf_interp_check(const axbpf_prog_t *prog)
{
    if (!prog || !prog->insns) {
        return -1;
    }
    if (prog->insn_count == 0 || prog->insn_count > AXBPF_MAX_INSNS) {
        return -10;
    }
    /* A program must end in EXIT, otherwise execution would fall off the end
     * into whatever follows in memory. */
    const axbpf_insn_t *last = &prog->insns[prog->insn_count - 1];
    if (AXBPF_CLASS(last) != AXBPF_JMP || AXBPF_OP(last) != AXBPF_EXIT) {
        return -9;
    }
    /* Every jump target must land inside the program. */
    for (u32 i = 0; i < prog->insn_count; i++) {
        const axbpf_insn_t *insn = &prog->insns[i];
        if (!axbpf_insn_is_jump(insn)) {
            continue;
        }
        i32 target = (i32)i + 1 + (i32)insn->off;
        if (target < 0 || (u32)target >= prog->insn_count) {
            return -8;
        }
    }
    return 0;
}

/* Count the instructions of each class, reported by `bpf stat`. */
void axbpf_prog_class_stats(const axbpf_prog_t *prog, u32 counts[8])
{
    if (!counts) {
        return;
    }
    for (u32 i = 0; i < 8; i++) {
        counts[i] = 0;
    }
    if (!prog || !prog->insns) {
        return;
    }
    for (u32 i = 0; i < prog->insn_count; i++) {
        u32 cls = (u32)AXBPF_CLASS(&prog->insns[i]);
        if (cls < 8) {
            counts[cls]++;
        }
    }
}

/* Return the index of the first instruction that touches register `reg`
 * without initialising it first; used by the verifier's diagnostics and by
 * static analysis in the shell. */
i32 axbpf_first_touch(const axbpf_prog_t *prog, u8 reg)
{
    if (!prog || !prog->insns) {
        return -1;
    }
    for (u32 i = 0; i < prog->insn_count; i++) {
        const axbpf_insn_t *insn = &prog->insns[i];
        if (axbpf_insn_writes_reg(insn, reg)) {
            return (i32)i;
        }
    }
    return -1;
}
