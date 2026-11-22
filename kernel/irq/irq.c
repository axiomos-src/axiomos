/* AxiomOS - interrupt dispatch and vector registry.
 *
 * All 256 vectors funnel through irq_dispatch().  Exceptions (0..31) go to the
 * architectural exception reporter; the legacy IRQ range is forwarded to
 * drivers registered through irq_register(); vector 0x80 is the syscall gate.
 * End-of-interrupt handling is abstracted so the same dispatch code works with
 * the 8259 PIC and with the local APIC.
 */

#include <axiom/kernel.h>
#include <axiom/idt.h>

struct irq_entry {
    irq_handler_t handler;
    const char   *name;
    u64           count;
};

static struct irq_entry g_irq_table[256];
static u64 g_spurious_irqs;
static u64 g_total_interrupts;

extern void exception_handler(u8 vector, struct interrupt_frame *frame);

/* Provided by timer.c (PIC EOI) and apic.c (APIC EOI). */
extern void pic_eoi(u8 irq);
extern bool_t apic_enabled(void);
extern void apic_eoi(void);
extern void syscall_entry_c(u64 nr, struct interrupt_frame *frame);

void irq_init(void)
{
    for (u32 i = 0; i < 256; i++) {
        g_irq_table[i].handler = NULL_PTR;
        g_irq_table[i].name = "unclaimed";
        g_irq_table[i].count = 0;
    }
    for (u32 vector = 0; vector < 32; vector++)
        irq_register((u8)vector, (irq_handler_t)exception_handler,
                     vector < 32 ? "cpu-exception" : "unknown");
    g_total_interrupts = 0;
    g_spurious_irqs = 0;
    printk("irq: dispatch table initialised\n");
}

void irq_register(u8 vector, irq_handler_t handler, const char *name)
{
    g_irq_table[vector].handler = handler;
    g_irq_table[vector].name = name ? name : "anonymous";
}

void irq_unregister(u8 vector)
{
    g_irq_table[vector].handler = NULL_PTR;
    g_irq_table[vector].name = "unclaimed";
}

u64 irq_count(u8 irq)
{
    return g_irq_table[irq].count;
}

void irq_dispatch(u8 vector, void *frame_ptr)
{
    struct interrupt_frame *frame = (struct interrupt_frame *)frame_ptr;

    g_total_interrupts++;
    g_irq_table[vector].count++;

    if (vector == VEC_SYSCALL) {
        /* Syscall ABI: rax = number, rdi/rsi/rdx/r10/r8/r9 = args. */
        syscall_entry_c(frame->rax, frame);
        return;
    }

    if (vector < 32) {
        exception_handler(vector, frame);
        return;
    }

    if (vector >= IRQ_BASE && vector < IRQ_BASE + 16) {
        u8 irq = (u8)(vector - IRQ_BASE);
        if (g_irq_table[vector].handler)
            g_irq_table[vector].handler(vector, frame);
        else
            g_spurious_irqs++;

        if (apic_enabled())
            apic_eoi();
        else
            pic_eoi(irq);
        return;
    }

    if (vector == VEC_APIC_SPURIOUS) {
        g_spurious_irqs++;
        if (apic_enabled())
            apic_eoi();
        return;
    }

    if (g_irq_table[vector].handler) {
        g_irq_table[vector].handler(vector, frame);
        if (apic_enabled())
            apic_eoi();
        return;
    }

    /* Unknown vector: log once per vector rather than flooding the serial
     * port, then continue.  Masking the source would be better, but we do not
     * know where it comes from. */
    if (g_irq_table[vector].count == 1)
        printk("irq: unhandled vector %u (ignored)\n", (u64)vector);
    if (apic_enabled())
        apic_eoi();
    else
        pic_eoi((u8)((vector >= IRQ_BASE) ? vector - IRQ_BASE : vector));
}

u64 irq_total_count(void)
{
    return g_total_interrupts;
}

u64 irq_spurious_count(void)
{
    return g_spurious_irqs;
}

void irq_dump(void)
{
    printk("irq: %u interrupts, %u spurious\n", g_total_interrupts,
           g_spurious_irqs);
    for (u32 i = 32; i < 256; i++) {
        if (g_irq_table[i].count)
            printk("  vector %u (%s): %u\n", (u64)i, g_irq_table[i].name,
                   g_irq_table[i].count);
    }
}
