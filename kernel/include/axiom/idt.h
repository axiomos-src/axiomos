/* AxiomOS - interrupt frame layout shared with arch/x86_64/isr.asm.
 *
 * The assembly stubs push the general purpose registers in the order below,
 * then the vector and error code.  A C handler receives a pointer to this
 * structure as its second argument.
 */
#ifndef AXIOM_IDT_H
#define AXIOM_IDT_H

#include <axiom/types.h>

struct interrupt_frame {
    /* pushed by isr_common, lowest address first */
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* pushed by the individual stub */
    u64 vector;
    u64 error_code;
    /* pushed by the CPU */
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
} AXIOM_PACKED;

struct idt_pointer {
    u16 limit;
    u64 base;
} AXIOM_PACKED;

/* Exception vector numbers */
#define VEC_DE      0
#define VEC_DB      1
#define VEC_NMI     2
#define VEC_BP      3
#define VEC_OF      4
#define VEC_BR      5
#define VEC_UD      6
#define VEC_NM      7
#define VEC_DF      8
#define VEC_TS      10
#define VEC_NP      11
#define VEC_SS      12
#define VEC_GP      13
#define VEC_PF      14
#define VEC_MF      16
#define VEC_AC      17
#define VEC_MC      18
#define VEC_XM      19
#define VEC_VE      20
#define VEC_CP      21
#define VEC_IRQ0    32
#define VEC_SYSCALL 0x80
#define VEC_APIC_SPURIOUS 0xFF

#define IRQ_BASE    VEC_IRQ0

#endif /* AXIOM_IDT_H */
