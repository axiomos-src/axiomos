/* AxiomOS - Interrupt Descriptor Table setup and exception reporting.
 *
 * Hardware IRQs are delivered through vectors 32..47 after the legacy PIC is
 * remapped; the APIC is brought up later and reuses the same handler slots.
 * The exception handlers below translate raw vector numbers into readable
 * diagnostics, because "triple fault" is a useless error message.
 */

#include <axiom/kernel.h>
#include <axiom/idt.h>

/* The stubs' addresses live in a table of 64-bit entries, so the table is
 * treated as an array of quadwords; reading a single byte would install a
 * garbage offset and turn every interrupt into a jump into low memory. */
extern u64 isr_table[];
extern void isr_stub_128(void);

static struct idt_entry g_idt[256];
static struct idt_pointer g_idt_ptr;
static bool_t g_idt_ready;

/* Human readable names for the architectural exceptions. */
static const char *exception_names[32] = {
    "divide error",              /* 0  */
    "debug",                     /* 1  */
    "non-maskable interrupt",    /* 2  */
    "breakpoint",                /* 3  */
    "overflow",                  /* 4  */
    "bound range exceeded",      /* 5  */
    "invalid opcode",            /* 6  */
    "device not available",      /* 7  */
    "double fault",              /* 8  */
    "coprocessor segment overrun",/* 9 */
    "invalid TSS",               /* 10 */
    "segment not present",       /* 11 */
    "stack-segment fault",       /* 12 */
    "general protection fault",  /* 13 */
    "page fault",                /* 14 */
    "reserved (15)",             /* 15 */
    "x87 floating point",        /* 16 */
    "alignment check",           /* 17 */
    "machine check",             /* 18 */
    "SIMD floating point",       /* 19 */
    "virtualisation",            /* 20 */
    "control protection",        /* 21 */
    "reserved (22)",             /* 22 */
    "reserved (23)",             /* 23 */
    "reserved (24)",             /* 24 */
    "reserved (25)",             /* 25 */
    "reserved (26)",             /* 26 */
    "reserved (27)",             /* 27 */
    "hypervisor injection",      /* 28 */
    "VMM communication",         /* 29 */
    "security",                  /* 30 */
    "reserved (31)"              /* 31 */
};

void idt_set_gate(u8 vector, void *handler, u16 selector, u8 ist, u8 type)
{
    u64 addr = (u64)handler;
    g_idt[vector].offset_low = (u16)(addr & 0xFFFF);
    g_idt[vector].selector = selector;
    g_idt[vector].ist = ist & 0x7;
    g_idt[vector].type_attr = type;
    g_idt[vector].offset_mid = (u16)((addr >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (u32)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vector].reserved = 0;
}

void idt_init(void)
{
    for (u32 i = 0; i < 256; i++) {
        g_idt[i].offset_low = 0;
        g_idt[i].selector = 0;
        g_idt[i].ist = 0;
        g_idt[i].type_attr = 0;
        g_idt[i].offset_mid = 0;
        g_idt[i].offset_high = 0;
        g_idt[i].reserved = 0;
    }

    for (u32 vector = 0; vector < 256; vector++) {
        u8 ist = 0;
        if (vector == VEC_DF)
            ist = 1;
        else if (vector == VEC_NMI)
            ist = 2;
        else if (vector == VEC_MC)
            ist = 3;
        /* 0x8E = present, DPL0, 64-bit interrupt gate (interrupts masked). */
        idt_set_gate((u8)vector, (void *)isr_table[vector], 0x08, ist, 0x8E);
    }

    /* The syscall gate is reachable from ring 3 (DPL3). */
    idt_set_gate(VEC_SYSCALL, (void *)&isr_stub_128, 0x08, 0, 0xEE);

    g_idt_ptr.limit = (u16)(sizeof(g_idt) - 1);
    g_idt_ptr.base = (u64)&g_idt;
    idt_load((u64)&g_idt_ptr);
    g_idt_ready = true;

    printk("idt: 256 gates installed at %p, syscall vector 0x80 (dpl3)\n",
           (void *)&g_idt);
}

bool_t idt_ready(void)
{
    return g_idt_ready;
}

/* ------------------------------------------------------------------ */
/* Exception diagnostics                                              */
/* ------------------------------------------------------------------ */

static u64 read_cr2_value(void)
{
    return read_cr2();
}

static const char *pf_reason(u64 error)
{
    if (error & 1) {
        /* Protection violation: page present but access denied. */
        return (error & 2) ? "write to read-only page" : "instruction fetch denied";
    }
    return (error & 2) ? "write to absent page" : "read from absent page";
}

void exception_handler(u8 vector, struct interrupt_frame *frame)
{
    const char *name = (vector < 32) ? exception_names[vector] : "unknown";

    if (vector == VEC_BP) {
        /* Breakpoints are recoverable: report and continue. */
        printk("breakpoint at rip=%p\n", (void *)frame->rip);
        return;
    }

    printk("\n*** exception %u: %s ***\n", (u64)vector, name);
    printk("  rip=%p cs=%x rflags=%p\n", (void *)frame->rip, frame->cs,
           (void *)frame->rflags);
    printk("  rsp=%p ss=%x error=%p\n", (void *)frame->rsp, frame->ss,
           (void *)frame->error_code);
    printk("  rax=%p rbx=%p rcx=%p rdx=%p\n", (void *)frame->rax,
           (void *)frame->rbx, (void *)frame->rcx, (void *)frame->rdx);
    printk("  rsi=%p rdi=%p rbp=%p\n", (void *)frame->rsi,
           (void *)frame->rdi, (void *)frame->rbp);
    printk("  r8=%p r9=%p r10=%p r11=%p\n", (void *)frame->r8,
           (void *)frame->r9, (void *)frame->r10, (void *)frame->r11);
    printk("  r12=%p r13=%p r14=%p r15=%p\n", (void *)frame->r12,
           (void *)frame->r13, (void *)frame->r14, (void *)frame->r15);

    if (vector == VEC_PF) {
        u64 cr2 = read_cr2_value();
        printk("  page fault at %p: %s (error=%p)\n", (void *)cr2,
               pf_reason(frame->error_code), (void *)frame->error_code);
        printk("  pml4=%p\n", (void *)read_cr3());
    }
    if (vector == VEC_DF)
        printk("  double fault: the exception handler itself faulted\n");
    if (vector == VEC_GP)
        printk("  general protection: selector=%x\n", frame->error_code);

    panic("unhandled CPU exception %u (%s)", (u64)vector, name);
}
