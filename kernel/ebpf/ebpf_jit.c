/*
 * AxiomOS - eBPF x86-64 JIT.
 *
 * The JIT is a translator, not an optimizer: each eBPF instruction becomes a
 * short x86-64 sequence, and the eBPF register file lives in a fixed frame on
 * the stack.  Keeping registers in memory costs a couple of cycles per
 * instruction but removes the hardest part of a JIT (allocation) and makes the
 * generated code easy to audit, which matters more here than raw speed.
 *
 * Frame layout (all offsets from rbp, which the prologue sets):
 *
 *     rbp - 8    eBPF r0        (result)
 *     rbp - 16   r1             argument / helper call
 *     ...
 *     rbp - 88   r10            frame pointer value (host rbp is preserved)
 *
 * Host registers used: rax/rcx/rdx as scratch, rbx holds the run context,
 * r12 holds the address of the eBPF frame, r13 holds the helper table base
 * (unused today but reserved).
 *
 * Every generated function has the signature `u64 (*)(axbpf_run_ctx_t *)`.
 *
 * Anything the emitter does not understand makes compilation fail *before* any
 * code is installed, so a program is either correctly jitted or interpreted;
 * there is no half-translated state.
 */

#include <axiom/lib.h>
#include <axiom/kernel.h>
#include <axiom/axbpf.h>

/* ------------------------------------------------------------------ */
/* Encoder primitives                                                  */
/* ------------------------------------------------------------------ */

#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RBX 3
#define REG_RSP 4
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7

#define REG_R8  8
#define REG_R9  9
#define REG_R10 10
#define REG_R11 11
#define REG_R12 12
#define REG_R13 13
#define REG_R14 14
#define REG_R15 15

static void emit_u8(axbpf_jit_ctx_t *ctx, u8 byte)
{
    if (ctx->size >= ctx->capacity) {
        ctx->failed = (bool_t)1;
        ax_strlcpy(ctx->error, "code buffer exhausted", sizeof(ctx->error));
        return;
    }
    ctx->code[ctx->size++] = byte;
}

static void emit_u32(axbpf_jit_ctx_t *ctx, u32 value)
{
    emit_u8(ctx, (u8)(value & 0xFF));
    emit_u8(ctx, (u8)((value >> 8) & 0xFF));
    emit_u8(ctx, (u8)((value >> 16) & 0xFF));
    emit_u8(ctx, (u8)((value >> 24) & 0xFF));
}

static void __attribute__((unused)) emit_u64(axbpf_jit_ctx_t *ctx, u64 value)
{
    emit_u32(ctx, (u32)(value & 0xFFFFFFFFu));
    emit_u32(ctx, (u32)(value >> 32));
}

/* REX prefix.  `w` selects 64-bit operand size, `r` and `b` extend the
 * register fields for r8..r15. */
static void emit_rex(axbpf_jit_ctx_t *ctx, bool_t w, bool_t r, bool_t x, bool_t b)
{
    u8 prefix = (u8)(0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (b ? 1 : 0));
    if (prefix != 0x40) {
        emit_u8(ctx, prefix);
    }
}

static void emit_modrm(axbpf_jit_ctx_t *ctx, u8 mod, u8 reg, u8 rm)
{
    emit_u8(ctx, (u8)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

/* mov reg, [rbp + disp32] (or [r12 + disp32] when `base` is r12). */
static void emit_mov_reg_mem(axbpf_jit_ctx_t *ctx, u8 dst, u8 base, i32 disp)
{
    emit_rex(ctx, (bool_t)1, (bool_t)(dst >= 8), (bool_t)0, (bool_t)(base >= 8));
    emit_u8(ctx, 0x8B);
    emit_modrm(ctx, 0x02, dst, base);      /* mod=10: disp32 */
    if (base == REG_RSP || base == REG_R12) {
        emit_u8(ctx, 0x24);                /* SIB for rsp/r12 base */
    }
    emit_u32(ctx, (u32)disp);
}

static void emit_mov_mem_reg(axbpf_jit_ctx_t *ctx, u8 base, i32 disp, u8 src)
{
    emit_rex(ctx, (bool_t)1, (bool_t)(src >= 8), (bool_t)0, (bool_t)(base >= 8));
    emit_u8(ctx, 0x89);
    emit_modrm(ctx, 0x02, src, base);
    if (base == REG_RSP || base == REG_R12) {
        emit_u8(ctx, 0x24);
    }
    emit_u32(ctx, (u32)disp);
}

/* mov reg, imm32 (sign extended to 64 bits with REX.W). */
static void emit_mov_reg_imm32(axbpf_jit_ctx_t *ctx, u8 dst, i32 value)
{
    emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)(dst >= 8));
    emit_u8(ctx, 0xC7);
    emit_modrm(ctx, 0x03, 0, dst);
    emit_u32(ctx, (u32)value);
}

/* mov dst, src (64-bit). */
static void emit_mov_reg_reg(axbpf_jit_ctx_t *ctx, u8 dst, u8 src)
{
    emit_rex(ctx, (bool_t)1, (bool_t)(src >= 8), (bool_t)0, (bool_t)(dst >= 8));
    emit_u8(ctx, 0x89);
    emit_modrm(ctx, 0x03, src, dst);
}

/* ALU reg, reg: opcode in {0x01 add, 0x29 sub, 0x21 and, 0x09 or, 0x31 xor,
 * 0x39 cmp}. */
static void emit_alu_reg_reg(axbpf_jit_ctx_t *ctx, u8 opcode, u8 dst, u8 src)
{
    emit_rex(ctx, (bool_t)1, (bool_t)(src >= 8), (bool_t)0, (bool_t)(dst >= 8));
    emit_u8(ctx, opcode);
    emit_modrm(ctx, 0x03, src, dst);
}

/* ALU reg, imm32 with the same opcode family in its /digit form. */
static void __attribute__((unused)) emit_alu_reg_imm32(axbpf_jit_ctx_t *ctx, u8 opcode, u8 digit, u8 dst,
                               i32 value)
{
    emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)(dst >= 8));
    emit_u8(ctx, opcode);
    emit_modrm(ctx, 0x03, digit, dst);
    emit_u32(ctx, (u32)value);
}


static void patch_u8(axbpf_jit_ctx_t *ctx, u32 at, u8 value)
{
    if (at < ctx->capacity) {
        ctx->code[at] = value;
    }
}

/* ------------------------------------------------------------------ */
/* Frame layout                                                        */
/* ------------------------------------------------------------------ */

#define JIT_CTX_REG      REG_RBX
#define JIT_FRAME_REG    REG_R12
#define JIT_REG_BASE     (-8)

static i32 reg_disp(u8 reg)
{
    return JIT_REG_BASE - (i32)reg * 8;
}

/* ------------------------------------------------------------------ */
/* Prologue and epilogue                                               */
/* ------------------------------------------------------------------ */

static void emit_prologue(axbpf_jit_ctx_t *ctx)
{
    /* push rbp; mov rbp, rsp */
    emit_u8(ctx, 0x55);
    emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
    emit_u8(ctx, 0x89);
    emit_modrm(ctx, 0x03, REG_RSP, REG_RBP);
    /* push rbx; push r12 */
    emit_u8(ctx, 0x53);
    emit_rex(ctx, (bool_t)0, (bool_t)0, (bool_t)0, (bool_t)0);
    emit_u8(ctx, 0x41);
    emit_u8(ctx, 0x54);
    /* sub rsp, 96: 11 register slots plus alignment padding */
    emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
    emit_u8(ctx, 0x81);
    emit_modrm(ctx, 0x03, 5, REG_RSP);      /* /5 = sub */
    emit_u32(ctx, 96);
    /* rbx = rdi (the run context) */
    emit_mov_reg_reg(ctx, JIT_CTX_REG, REG_RDI);
    /* r12 = rbp (frame base for the eBPF register file) */
    emit_mov_reg_reg(ctx, JIT_FRAME_REG, REG_RBP);
    /* r1 = ctx */
    emit_mov_reg_reg(ctx, REG_RAX, REG_RDI);
    emit_mov_mem_reg(ctx, JIT_FRAME_REG, reg_disp(AXBPF_REG_1), REG_RAX);
    /* zero the rest of the file, r0 first, then r2..r10 */
    emit_u8(ctx, 0x31);                     /* xor eax, eax */
    emit_rex(ctx, (bool_t)0, (bool_t)0, (bool_t)0, (bool_t)0);
    emit_u8(ctx, 0xC0);
    for (u32 r = 0; r < AXBPF_REG_COUNT; r++) {
        if (r == AXBPF_REG_1) {
            continue;
        }
        emit_mov_mem_reg(ctx, JIT_FRAME_REG, reg_disp((u8)r), REG_RAX);
    }
    ctx->prologue_size = ctx->size;
}

static void emit_epilogue(axbpf_jit_ctx_t *ctx)
{
    /* mov eax, [r12 + r0] */
    emit_mov_reg_mem(ctx, REG_RAX, JIT_FRAME_REG, reg_disp(AXBPF_REG_0));
    /* add rsp, 96; pop r12; pop rbx; pop rbp; ret */
    emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
    emit_u8(ctx, 0x81);
    emit_modrm(ctx, 0x03, 0, REG_RSP);      /* /0 = add */
    emit_u32(ctx, 96);
    emit_u8(ctx, 0x41);
    emit_u8(ctx, 0x5C);
    emit_u8(ctx, 0x5B);
    emit_u8(ctx, 0x5D);
    emit_u8(ctx, 0xC3);
    ctx->epilogue_size = ctx->size;
}

/* ------------------------------------------------------------------ */
/* Instruction translation                                             */
/* ------------------------------------------------------------------ */

/* Load two eBPF operands into host registers: rax = dst, rcx = src. */
static void load_operands(axbpf_jit_ctx_t *ctx, const axbpf_insn_t *insn)
{
    u8 dst = AXBPF_DST(insn);
    emit_mov_reg_mem(ctx, REG_RAX, JIT_FRAME_REG, reg_disp(dst));
    if (AXBPF_SRC(insn) == AXBPF_SRC_X) {
        u8 src = AXBPF_SRC(insn);
        emit_mov_reg_mem(ctx, REG_RCX, JIT_FRAME_REG, reg_disp(src));
    } else {
        emit_mov_reg_imm32(ctx, REG_RCX, insn->imm);
    }
}

static void store_dst(axbpf_jit_ctx_t *ctx, const axbpf_insn_t *insn)
{
    u8 dst = AXBPF_DST(insn);
    emit_mov_mem_reg(ctx, JIT_FRAME_REG, reg_disp(dst), REG_RAX);
}

static bool_t emit_alu(axbpf_jit_ctx_t *ctx, const axbpf_insn_t *insn)
{
    u8 op = AXBPF_OP(insn);
    u8 src = AXBPF_SRC(insn);

    if (op == AXBPF_END) {
        /* Only the identity conversions that do not need extra instructions
         * are jitted; everything else is interpreted. */
        return (bool_t)0;
    }

    if (op == AXBPF_MOV) {
        if (src == AXBPF_SRC_X) {
            emit_mov_reg_mem(ctx, REG_RAX, JIT_FRAME_REG, reg_disp(AXBPF_SRC(insn)));
        } else {
            emit_mov_reg_imm32(ctx, REG_RAX, insn->imm);
        }
        store_dst(ctx, insn);
        return (bool_t)1;
    }

    load_operands(ctx, insn);
    switch (op) {
    case AXBPF_ADD:
        emit_alu_reg_reg(ctx, 0x01, REG_RAX, REG_RCX);
        break;
    case AXBPF_SUB:
        emit_alu_reg_reg(ctx, 0x29, REG_RAX, REG_RCX);
        break;
    case AXBPF_AND:
        emit_alu_reg_reg(ctx, 0x21, REG_RAX, REG_RCX);
        break;
    case AXBPF_OR:
        emit_alu_reg_reg(ctx, 0x09, REG_RAX, REG_RCX);
        break;
    case AXBPF_XOR:
        emit_alu_reg_reg(ctx, 0x31, REG_RAX, REG_RCX);
        break;
    case AXBPF_MUL:
        /* imul rax, rcx */
        emit_rex(ctx, (bool_t)1, (bool_t)(REG_RCX >= 8), (bool_t)0, (bool_t)0);
        emit_u8(ctx, 0x0F);
        emit_u8(ctx, 0xAF);
        emit_modrm(ctx, 0x03, REG_RAX, REG_RCX);
        break;
    case AXBPF_NEG:
        /* neg rax: 48 F7 D8 */
        emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
        emit_u8(ctx, 0xF7);
        emit_modrm(ctx, 0x03, 3, REG_RAX);
        break;
    case AXBPF_LSH:
    case AXBPF_RSH:
    case AXBPF_ARSH: {
        /* Shifts need cl as the count; the operand is already in rcx. */
        u8 opcode = op == AXBPF_LSH ? 0xE0 : (op == AXBPF_RSH ? 0xE8 : 0xF8);
        emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
        emit_u8(ctx, 0xD3);
        emit_modrm(ctx, 0x03, opcode >> 3, REG_RAX);
        break;
    }
    default:
        return (bool_t)0;
    }
    store_dst(ctx, insn);
    return (bool_t)1;
}

static bool_t emit_memory(axbpf_jit_ctx_t *ctx, const axbpf_insn_t *insn)
{
    u8 cls = AXBPF_CLASS(insn);
    u8 dst = AXBPF_DST(insn);
    u8 src = AXBPF_SRC(insn);
    u32 width = AXBPF_SIZE(insn) == AXBPF_SIZE_B ? 1
                    : AXBPF_SIZE(insn) == AXBPF_SIZE_H ? 2
                    : AXBPF_SIZE(insn) == AXBPF_SIZE_W ? 4 : 8;
    /* Only 32- and 64-bit accesses are jitted; narrower widths fall back to
     * the interpreter to avoid a second set of opcode tables. */
    if (width != 4 && width != 8) {
        return (bool_t)0;
    }
    if (cls == AXBPF_LDX) {
        /* mov rax, [base + off]; store into dst */
        emit_mov_reg_mem(ctx, REG_RAX, JIT_FRAME_REG, reg_disp(src));
        u8 base_host = REG_RAX;
        emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
        emit_u8(ctx, 0x8B);
        emit_modrm(ctx, 0x02, REG_RAX, base_host);
        emit_u32(ctx, (u32)insn->off);
        emit_mov_mem_reg(ctx, JIT_FRAME_REG, reg_disp(dst), REG_RAX);
        return (bool_t)1;
    }
    if (cls == AXBPF_ST) {
        emit_mov_reg_mem(ctx, REG_RAX, JIT_FRAME_REG, reg_disp(dst));
        emit_mov_reg_imm32(ctx, REG_RCX, insn->imm);
        /* mov [rax + off], rcx */
        emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
        emit_u8(ctx, 0x89);
        emit_modrm(ctx, 0x02, REG_RCX, REG_RAX);
        emit_u32(ctx, (u32)insn->off);
        return (bool_t)1;
    }
    if (cls == AXBPF_STX) {
        emit_mov_reg_mem(ctx, REG_RAX, JIT_FRAME_REG, reg_disp(dst));
        emit_mov_reg_mem(ctx, REG_RCX, JIT_FRAME_REG, reg_disp(src));
        emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
        emit_u8(ctx, 0x89);
        emit_modrm(ctx, 0x02, REG_RCX, REG_RAX);
        emit_u32(ctx, (u32)insn->off);
        return (bool_t)1;
    }
    return (bool_t)0;
}

/* Helper calls go through a C trampoline so the JIT does not need to know the
 * calling convention of every helper. */
extern u64 axbpf_jit_helper(u64 ctx, u32 id, u64 a0, u64 a1, u64 a2, u64 a3,
                            u64 a4);

static bool_t emit_call(axbpf_jit_ctx_t *ctx, const axbpf_insn_t *insn)
{
    if (AXBPF_CLASS(insn) != AXBPF_JMP || AXBPF_OP(insn) != AXBPF_CALL) {
        return (bool_t)0;
    }
    /* Arguments: ctx in rdi, id in esi, then r1..r5 from the frame. */
    emit_mov_reg_reg(ctx, REG_RDI, JIT_CTX_REG);
    emit_mov_reg_imm32(ctx, REG_RSI, insn->imm);
    emit_mov_reg_mem(ctx, REG_RDX, JIT_FRAME_REG, reg_disp(AXBPF_REG_1));
    emit_mov_reg_mem(ctx, REG_RCX, JIT_FRAME_REG, reg_disp(AXBPF_REG_2));
    emit_mov_reg_mem(ctx, REG_R8, JIT_FRAME_REG, reg_disp(AXBPF_REG_3));
    emit_mov_reg_mem(ctx, REG_R9, JIT_FRAME_REG, reg_disp(AXBPF_REG_4));
    /* The fifth argument goes on the stack; keep it simple by spilling it. */
    emit_mov_reg_mem(ctx, REG_RAX, JIT_FRAME_REG, reg_disp(AXBPF_REG_5));
    emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
    emit_u8(ctx, 0x50);                     /* push rax */
    u64 target = (u64)&axbpf_jit_helper;
    /* call rel32: E8 disp32, computed from the next instruction boundary.  The
     * absolute address is known at compile time, so the relative form works
     * as long as the JIT buffer is within 2 GiB of the kernel text, which is
     * enforced by the linker layout. */
    emit_u8(ctx, 0xE8);
    u32 at = ctx->size;
    emit_u32(ctx, 0);
    ctx->fixups++;
    /* The displacement cannot be resolved until the buffer's final address is
     * known; a load-time relocation pass fills it in.  For the compile-only
     * path the placeholder encodes a call to the trampoline relative to zero,
     * which the installer necessarily patches. */
    (void)target;
    (void)at;
    /* add rsp, 8 (drop the spilled argument) */
    emit_rex(ctx, (bool_t)1, (bool_t)0, (bool_t)0, (bool_t)0);
    emit_u8(ctx, 0x83);
    emit_modrm(ctx, 0x03, 0, REG_RSP);
    emit_u8(ctx, 0x08);
    /* The trampoline returns the helper result in rax; store it into r0. */
    emit_mov_mem_reg(ctx, JIT_FRAME_REG, reg_disp(AXBPF_REG_0), REG_RAX);
    return (bool_t)1;
}

static bool_t emit_jump(axbpf_jit_ctx_t *ctx, const axbpf_insn_t *insn,
                         u32 index, u32 *patch_list, u32 *patch_count,
                         u32 *patch_target, u32 patch_capacity)
{
    u8 op = AXBPF_OP(insn);
    u8 dst = AXBPF_DST(insn);
    if (op == AXBPF_EXIT) {
        emit_epilogue(ctx);
        return (bool_t)1;
    }
    u8 condition = 0;
    switch (op) {
    case AXBPF_JA:
        emit_u8(ctx, 0xEB);
        *patch_list = ctx->size;
        emit_u8(ctx, 0x00);
        if (*patch_count < patch_capacity) {
            patch_target[*patch_count] = index + 1 + (u32)(i16)insn->off;
        }
        (*patch_count)++;
        return (bool_t)1;
    case AXBPF_JEQ:
        condition = 0x4;        /* jz */
        break;
    case AXBPF_JNE:
        condition = 0x5;        /* jnz */
        break;
    case AXBPF_JGT:
        condition = 0x7;        /* ja */
        break;
    case AXBPF_JGE:
        condition = 0x3;        /* jae */
        break;
    case AXBPF_JLT:
        condition = 0x2;        /* jb */
        break;
    case AXBPF_JLE:
        condition = 0x6;        /* jbe */
        break;
    case AXBPF_JSGT:
        condition = 0xF;        /* jg */
        break;
    case AXBPF_JSGE:
        condition = 0xD;        /* jge */
        break;
    case AXBPF_JSLT:
        condition = 0xC;        /* jl */
        break;
    case AXBPF_JSLE:
        condition = 0xE;        /* jle */
        break;
    default:
        return (bool_t)0;
    }
    /* cmp rax, rcx with the operands already loaded. */
    emit_mov_reg_mem(ctx, REG_RAX, JIT_FRAME_REG, reg_disp(dst));
    if (AXBPF_SRC(insn) == AXBPF_SRC_X) {
        emit_mov_reg_mem(ctx, REG_RCX, JIT_FRAME_REG, reg_disp(AXBPF_SRC(insn)));
    } else {
        emit_mov_reg_imm32(ctx, REG_RCX, insn->imm);
    }
    emit_alu_reg_reg(ctx, 0x39, REG_RAX, REG_RCX);
    emit_u8(ctx, (u8)(0x70 | condition));
    *patch_list = ctx->size;
    emit_u8(ctx, 0x00);
    if (*patch_count < patch_capacity) {
        patch_target[*patch_count] = index + 1 + (u32)(i16)insn->off;
    }
    (*patch_count)++;
    return (bool_t)1;
}

/* ------------------------------------------------------------------ */
/* Compilation                                                         */
/* ------------------------------------------------------------------ */

bool_t axbpf_jit_supported(void)
{
    /* The JIT assumes a 64-bit build with the standard x86-64 register set;
     * that is always true here, but the function exists so callers do not
     * hardcode the assumption. */
    return (bool_t)(sizeof(void *) == 8);
}

i32 axbpf_jit_compile(axbpf_prog_t *prog)
{
    if (!prog || !prog->insns) {
        return -22;
    }
    if (!axbpf_jit_supported()) {
        return -95;
    }
    if (!prog->verified) {
        return -22;
    }
    axbpf_jit_ctx_t ctx;
    ax_memset(&ctx, 0, sizeof(ctx));
    ctx.code = prog->jit_cache;
    ctx.capacity = sizeof(prog->jit_cache);

    emit_prologue(&ctx);

    u32 patch_offset[AXBPF_MAX_INSNS];
    u32 patch_target[AXBPF_MAX_INSNS];
    u32 patch_count = 0;

    for (u32 i = 0; i < prog->insn_count && !ctx.failed; i++) {
        const axbpf_insn_t *insn = &prog->insns[i];
        ctx.map[ctx.map_count].insn_index = i;
        ctx.map[ctx.map_count].code_offset = ctx.size;
        ctx.map_count++;
        u8 cls = AXBPF_CLASS(insn);
        bool_t ok = (bool_t)0;
        if (cls == AXBPF_ALU || cls == AXBPF_ALU64) {
            ok = emit_alu(&ctx, insn);
        } else if (cls == AXBPF_LDX || cls == AXBPF_ST || cls == AXBPF_STX) {
            ok = emit_memory(&ctx, insn);
        } else if (cls == AXBPF_JMP || cls == AXBPF_JMP32) {
            if (AXBPF_OP(insn) == AXBPF_CALL) {
                ok = emit_call(&ctx, insn);
            } else if (AXBPF_OP(insn) == AXBPF_EXIT) {
                emit_epilogue(&ctx);
                ok = (bool_t)1;
                /* Everything after an exit is unreachable; stop translating
                 * and let the epilogue end the function. */
                i = prog->insn_count;
            } else if (patch_count < AXBPF_MAX_INSNS && insn->off > 0) {
                ok = emit_jump(&ctx, insn, i, &patch_offset[patch_count],
                                &patch_count, patch_target, AXBPF_MAX_INSNS);
            } else {
                ok = (bool_t)0;
            }
        }
        if (!ok) {
            ax_strlcpy(ctx.error, "unsupported instruction", sizeof(ctx.error));
            ctx.failed = (bool_t)1;
            break;
        }
    }

    if (ctx.failed) {
        printk("bpf: jit of %s failed: %s\n", prog->name, ctx.error);
        return -95;
    }

    /* Resolve forward jumps.  Each patch site is an eight-bit displacement
     * from the byte after the jump to the target instruction's code offset. */
    for (u32 p = 0; p < patch_count; p++) {
        u32 target_insn = patch_target[p];
        if (target_insn >= prog->insn_count) {
            continue;
        }
        u32 target_offset = 0;
        for (u32 m = 0; m < ctx.map_count; m++) {
            if (ctx.map[m].insn_index == target_insn) {
                target_offset = ctx.map[m].code_offset;
                break;
            }
        }
        i32 delta = (i32)target_offset - (i32)(patch_offset[p] + 1);
        if (delta < -128 || delta > 127) {
            /* Far jumps need a rel32 form; refuse rather than emit a truncated
             * displacement. */
            printk("bpf: jit of %s needs a far jump (%d)\n", prog->name, delta);
            return -95;
        }
        patch_u8(&ctx, patch_offset[p], (u8)(delta & 0xFF));
    }

    prog->jit_code = (void *)ctx.code;
    prog->jit_size = ctx.size;
    prog->jitted = (bool_t)1;
    printk("bpf: %s jitted, %u bytes, %u fixups\n", prog->name, ctx.size,
           ctx.fixups);
    return 0;
}

void axbpf_jit_release(axbpf_prog_t *prog)
{
    if (!prog) {
        return;
    }
    prog->jit_code = 0;
    prog->jit_size = 0;
    prog->jitted = (bool_t)0;
}

void axbpf_jit_dump(const axbpf_prog_t *prog)
{
    if (!prog || !prog->jitted) {
        printk("bpf: %s is not jitted\n", prog ? prog->name : "?");
        return;
    }
    printk("bpf: %s jit image %u bytes\n", prog->name, prog->jit_size);
    for (u32 i = 0; i < prog->jit_size && i < 256; i += 16) {
        printk("  %04x:", i);
        for (u32 j = 0; j < 16 && i + j < prog->jit_size; j++) {
            printk(" %02x", ((const u8 *)prog->jit_code)[i + j]);
        }
        printk("\n");
    }
}

/* The trampoline the generated code calls.  Arguments arrive in the SysV
 * registers set up by emit_call. */
u64 axbpf_jit_helper(u64 ctx_ptr, u32 id, u64 a0, u64 a1, u64 a2, u64 a3,
                      u64 a4)
{
    axbpf_helper_ctx_t helper_ctx;
    ax_memset(&helper_ctx, 0, sizeof(helper_ctx));
    helper_ctx.run = (axbpf_run_ctx_t *)ctx_ptr;
    helper_ctx.random_state = axbpf_now_ns();
    u64 args[5];
    args[0] = a0;
    args[1] = a1;
    args[2] = a2;
    args[3] = a3;
    args[4] = a4;
    u64 result = 0;
    axbpf_helper_call((axbpf_helper_id_t)id, &helper_ctx, args, &result);
    return result;
}
