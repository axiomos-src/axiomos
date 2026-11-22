/* AxiomOS - eBPF disassembler.
 *
 * Used by the trace printer and by the verifier diagnostics: when a program is
 * rejected the kernel dumps the offending instructions in readable form so the
 * user can see which register or map was implicated.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXBD_MAX_LINES   64
#define AXBD_LINE_LEN    72
#define AXBD_MAX_OPS     96

typedef struct axbd_opcode
{
    u8          code;
    const char *mnemonic;
    const char *operands;
} axbd_opcode_t;

typedef struct axbd_line
{
    char text[AXBD_LINE_LEN];
    u16  instruction;
} axbd_line_t;

typedef struct axbd_output
{
    axbd_line_t lines[AXBD_MAX_LINES];
    u16         count;
    u16         truncated;
    u32         render_failures;
    bool_t      ready;
} axbd_output_t;

static const axbd_opcode_t axbd_ops[AXBD_MAX_OPS] = {
    { 0x07, "add64 imm", "r%u += %d" },
    { 0x0F, "add64 reg", "r%u += r%u" },
    { 0x17, "sub64 imm", "r%u -= %d" },
    { 0x1F, "sub64 reg", "r%u -= r%u" },
    { 0x27, "mul64 imm", "r%u *= %d" },
    { 0x2F, "mul64 reg", "r%u *= r%u" },
    { 0x37, "div64 imm", "r%u /= %d" },
    { 0x3F, "div64 reg", "r%u /= r%u" },
    { 0x47, "or64 imm", "r%u |= %d" },
    { 0x4F, "or64 reg", "r%u |= r%u" },
    { 0x57, "and64 imm", "r%u &= %d" },
    { 0x5F, "and64 reg", "r%u &= r%u" },
    { 0x67, "lsh64 imm", "r%u <<= %d" },
    { 0x6F, "lsh64 reg", "r%u <<= r%u" },
    { 0x77, "rsh64 imm", "r%u >>= %d" },
    { 0x7F, "rsh64 reg", "r%u >>= r%u" },
    { 0x87, "neg64", "r%u = -r%u" },
    { 0x97, "mod64 imm", "r%u %%= %d" },
    { 0x9F, "mod64 reg", "r%u %%= r%u" },
    { 0xA7, "xor64 imm", "r%u ^= %d" },
    { 0xAF, "xor64 reg", "r%u ^= r%u" },
    { 0xB7, "mov64 imm", "r%u = %d" },
    { 0xBF, "mov64 reg", "r%u = r%u" },
    { 0xC7, "arsh64 imm", "r%u s>>= %d" },
    { 0xCF, "arsh64 reg", "r%u s>>= r%u" },
    { 0x18, "lddw", "r%u = %llx" },
    { 0x61, "ldxw", "r%u = *(u32 *)(r%u + %d)" },
    { 0x69, "ldxh", "r%u = *(u16 *)(r%u + %d)" },
    { 0x71, "ldxb", "r%u = *(u8 *)(r%u + %d)" },
    { 0x79, "ldxdw", "r%u = *(u64 *)(r%u + %d)" },
    { 0x62, "stxw", "*(u32 *)(r%u + %d) = r%u" },
    { 0x63, "stxdw", "*(u64 *)(r%u + %d) = r%u" },
    { 0x6A, "stxh", "*(u16 *)(r%u + %d) = r%u" },
    { 0x72, "stxb", "*(u8 *)(r%u + %d) = r%u" },
    { 0x05, "ja", "goto %+d" },
    { 0x15, "jeq imm", "if r%u == %d goto %+d" },
    { 0x1D, "jeq reg", "if r%u == r%u goto %+d" },
    { 0x25, "jgt imm", "if r%u > %d goto %+d" },
    { 0x2D, "jgt reg", "if r%u > r%u goto %+d" },
    { 0x35, "jge imm", "if r%u >= %d goto %+d" },
    { 0x3D, "jge reg", "if r%u >= r%u goto %+d" },
    { 0x45, "jset imm", "if r%u & %d goto %+d" },
    { 0x4D, "jset reg", "if r%u & r%u goto %+d" },
    { 0x55, "jne imm", "if r%u != %d goto %+d" },
    { 0x5D, "jne reg", "if r%u != r%u goto %+d" },
    { 0x85, "call", "call %d" },
    { 0x95, "exit", "exit" },
    { 0xB5, "jslt imm", "if r%u < %d goto %+d" },
    { 0xAD, "jslt reg", "if r%u < r%u goto %+d" },
    { 0xB6, "st imm", "*(u32 *)(r%u + %d) = %d" },
    { 0xD4, "be16", "r%u = be16(r%u)" },
    { 0xDC, "be32", "r%u = be32(r%u)" },
    { 0x05 + 0x00, "nop", "nop" }
    /* The remaining opcodes are decoded through the generic table below. */
};

static void axbd_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axbd_append(axbd_output_t *out, const char *text, u16 index)
{
    axbd_line_t *line;
    u32 i = 0;
    if (out->count >= AXBD_MAX_LINES)
    {
        out->truncated++;
        return;
    }
    line = &out->lines[out->count];
    while (text[i] != 0 && i + 1 < AXBD_LINE_LEN)
    {
        line->text[i] = text[i];
        i++;
    }
    line->text[i] = 0;
    line->instruction = index;
    out->count++;
}

static const axbd_opcode_t *axbd_lookup(u8 code)
{
    u32 i;
    for (i = 0; i < AXBD_MAX_OPS; i++)
    {
        if (axbd_ops[i].code == code && axbd_ops[i].mnemonic != NULL_PTR)
        {
            if (i > 0 && axbd_ops[i].code == 0x05 && i != 33)
                continue;
            return &axbd_ops[i];
        }
    }
    return NULL_PTR;
}

static const char *axbd_fallback(u8 code)
{
    switch (code & 0x07)
    {
    case 0: return "alu";
    case 1: return "ldx";
    case 2: return "stx";
    case 3: return "st";
    case 4: return "alu32";
    case 5: return "jmp";
    case 6: return "jmp32";
    default: return "unknown";
    }
}

static void axbd_format_number(char *dst, u32 value, u32 base)
{
    u32 i = 0;
    u32 digits = 0;
    u32 tmp = value;
    if (tmp == 0)
    {
        dst[0] = '0';
        dst[1] = 0;
        return;
    }
    while (tmp > 0)
    {
        tmp /= base;
        digits++;
    }
    for (i = 0; i < digits; i++)
    {
        u32 digit = value % base;
        value /= base;
        dst[digits - i - 1] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    dst[digits] = 0;
}

static void axbd_render_alu(char *out, u32 capacity, const axbd_opcode_t *op,
                            u8 dst_reg, u8 src_reg, i32 imm)
{
    u32 cursor = 0;
    const char *text = op->mnemonic;
    u32 i = 0;
    char number[24];
    while (text[i] != 0 && cursor + 1 < capacity)
    {
        out[cursor++] = text[i++];
    }
    if (cursor + 1 < capacity)
        out[cursor++] = ' ';
    axbd_format_number(number, (u32)dst_reg, 10);
    for (i = 0; number[i] != 0 && cursor + 1 < capacity; i++)
        out[cursor++] = number[i];
    if (imm != 0)
    {
        if (cursor + 2 < capacity)
        {
            out[cursor++] = ' ';
            out[cursor++] = ',';
        }
        axbd_format_number(number, (u32)(imm < 0 ? -imm : imm), 10);
        if (imm < 0 && cursor + 1 < capacity)
            out[cursor++] = '-';
        for (i = 0; number[i] != 0 && cursor + 1 < capacity; i++)
            out[cursor++] = number[i];
    }
    AXIOM_UNUSED_VAR(src_reg);
    out[cursor] = 0;
}

/* -------------------------------------------------------------- public API */

void axbd_init(axbd_output_t *out)
{
    axbd_zero(out, (u32)sizeof(*out));
    out->ready = true;
}

u32 axbd_disassemble(axbd_output_t *out, const void *instructions, u32 count,
                     u32 base_index)
{
    const u8 *cursor = (const u8 *)instructions;
    u32 i;
    if (!out->ready || instructions == NULL_PTR)
        return 0;
    for (i = 0; i < count; i++)
    {
        char text[AXBD_LINE_LEN];
        const u8 *instruction = cursor + i * 8;
        u8 code = instruction[0];
        u8 dst = instruction[1] & 0x0F;
        u8 src = (instruction[1] >> 4) & 0x0F;
        i16 offset = *(const i16 *)(instruction + 2);
        i32 imm = *(const i32 *)(instruction + 4);
        const axbd_opcode_t *op = axbd_lookup(code);
        u32 j;
        u32 cursor_out = 0;
        char number[24];
        axbd_zero(text, AXBD_LINE_LEN);
        axbd_format_number(number, base_index + i, 10);
        text[0] = number[0];
        text[1] = number[1];
        text[2] = number[2];
        text[3] = ' ';
        text[4] = ' ';
        cursor_out = 5;
        if (op != NULL_PTR)
        {
            char body[AXBD_LINE_LEN];
            axbd_render_alu(body, AXBD_LINE_LEN, op, dst, src, imm);
            for (j = 0; body[j] != 0 && cursor_out + 1 < AXBD_LINE_LEN; j++)
                text[cursor_out++] = body[j];
            if (offset != 0 && cursor_out + 8 < AXBD_LINE_LEN)
            {
                text[cursor_out++] = ' ';
                text[cursor_out++] = 'o';
                text[cursor_out++] = 'f';
                text[cursor_out++] = 's';
                text[cursor_out++] = 'e';
                text[cursor_out++] = '=';
                axbd_format_number(number, (u32)(offset < 0 ? -offset : offset), 10);
                for (j = 0; number[j] != 0 && cursor_out + 1 < AXBD_LINE_LEN; j++)
                    text[cursor_out++] = number[j];
            }
        }
        else
        {
            const char *kind = axbd_fallback(code);
            for (j = 0; kind[j] != 0 && cursor_out + 1 < AXBD_LINE_LEN; j++)
                text[cursor_out++] = kind[j];
            out->render_failures++;
        }
        text[cursor_out] = 0;
        axbd_append(out, text, (u16)(base_index + i));
    }
    return out->count;
}

u32 axbd_dump(axbd_output_t *out)
{
    u32 i;
    for (i = 0; i < out->count; i++)
    {
        printk("  %04u  %s\n", (unsigned)out->lines[i].instruction,
               out->lines[i].text);
    }
    if (out->truncated > 0)
        printk("  ... %u lines truncated\n", (unsigned)out->truncated);
    return out->count;
}

const char *axbd_line(axbd_output_t *out, u32 index)
{
    if (index >= out->count)
        return "";
    return out->lines[index].text;
}

u16 axbd_opcode_count(void)
{
    return (u16)AXBD_MAX_OPS;
}

bool_t axbd_selftest(void)
{
    static const u8 program[] = {
        0xB7, 0x01, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
        0x07, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x95, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    axbd_output_t out;
    axbd_init(&out);
    if (axbd_disassemble(&out, program, 3, 0) != 3)
        return false;
    if (out.count != 3)
        return false;
    if (out.truncated != 0)
        return false;
    if (axbd_line(&out, 2)[0] == 0)
        return false;
    if (axbd_opcode_count() != AXBD_MAX_OPS)
        return false;
    return true;
}
