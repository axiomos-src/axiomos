/* AxiomOS - 8259 PIC, PIT and HPET.
 *
 * The legacy interrupt controller is programmed first because it needs no
 * ACPI tables:  the kernel must be able to take timer interrupts before the
 * ACPI parser has run.  Once the local APIC is enabled the PIC is masked and
 * the PIT keeps counting as a fallback time source.
 */

#include <axiom/kernel.h>
#include <axiom/idt.h>

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_FREQ  1193182ULL

static u64 g_ticks;
static u64 g_timer_hz = 100;
static u64 g_boot_tsc;
static bool_t g_timer_ready;

/* io helpers (declared here to keep the module self contained) */
extern u8  io_inb(u16 port);
extern void io_outb(u16 port, u8 value);
extern void io_wait(void);

/* ------------------------------------------------------------------ */
/* 8259 PIC                                                           */
/* ------------------------------------------------------------------ */

void pic_remap(void)
{
    /* ICW1: start initialisation, ICW4 needed */
    io_outb(PIC1_CMD, 0x11);
    io_wait();
    io_outb(PIC2_CMD, 0x11);
    io_wait();
    /* ICW2: vector offsets - master 32, slave 40 */
    io_outb(PIC1_DATA, 0x20);
    io_wait();
    io_outb(PIC2_DATA, 0x28);
    io_wait();
    /* ICW3: master has slave on IRQ2, slave is cascade identity 2 */
    io_outb(PIC1_DATA, 0x04);
    io_wait();
    io_outb(PIC2_DATA, 0x02);
    io_wait();
    /* ICW4: 8086 mode */
    io_outb(PIC1_DATA, 0x01);
    io_wait();
    io_outb(PIC2_DATA, 0x01);
    io_wait();

    /* Mask everything; drivers unmask what they need. */
    io_outb(PIC1_DATA, 0xFF);
    io_outb(PIC2_DATA, 0xFF);
    printk("pic: remapped to vectors 32..47, all sources masked\n");
}

void pic_mask_all(void)
{
    io_outb(PIC1_DATA, 0xFF);
    io_outb(PIC2_DATA, 0xFF);
}

void pic_eoi(u8 irq)
{
    if (irq >= 8)
        io_outb(PIC2_CMD, PIC_EOI);
    io_outb(PIC1_CMD, PIC_EOI);
}

void irq_unmask(u8 irq)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 bit = (u8)(1 << (irq & 7));
    u8 mask = io_inb(port);
    io_outb(port, (u8)(mask & ~bit));
    if (irq >= 8) {
        /* Cascade line must also be unmasked on the master. */
        u8 master = io_inb(PIC1_DATA);
        io_outb(PIC1_DATA, (u8)(master & ~(1 << 2)));
    }
}

void irq_mask(u8 irq)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 bit = (u8)(1 << (irq & 7));
    u8 mask = io_inb(port);
    io_outb(port, (u8)(mask | bit));
}

/* ------------------------------------------------------------------ */
/* Programmable Interval Timer                                        */
/* ------------------------------------------------------------------ */

static void timer_irq_handler(u8 vector, void *frame)
{
    AXIOM_UNUSED_VAR(frame);
    AXIOM_UNUSED_VAR(vector);
    g_ticks++;
    sched_tick();
}

void pit_init(u32 frequency_hz)
{
    if (!frequency_hz)
        frequency_hz = 100;
    g_timer_hz = frequency_hz;

    /* Command: channel 0, lobyte/hibyte access, mode 3 (square wave). */
    u64 divisor = PIT_FREQ / frequency_hz;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;
    if (divisor < 1)
        divisor = 1;

    io_outb(PIT_CMD, 0x36);
    io_outb(PIT_CH0, (u8)(divisor & 0xFF));
    io_outb(PIT_CH0, (u8)((divisor >> 8) & 0xFF));

    irq_register(VEC_IRQ0, timer_irq_handler, "pit");
    irq_unmask(0);
    g_boot_tsc = rdtsc();
    g_timer_ready = true;
    printk("pit: %u Hz (divisor %u), vector %u\n", g_timer_hz, divisor,
           (u64)VEC_IRQ0);
}

u64 timer_ticks(void)
{
    return g_ticks;
}

u64 timer_ms(void)
{
    if (!g_timer_ready)
        return 0;
    return (g_ticks * 1000) / g_timer_hz;
}

void timer_sleep_ticks(u64 ticks)
{
    u64 target = g_ticks + ticks;
    u64 spin_limit = 0x8000000ULL;

    while (g_ticks < target) {
        sti();
        hlt();
        /* If the timer source is not delivering interrupts this would block
         * the whole boot; bail out after a generous budget and report it
         * instead of hanging silently. */
        if (--spin_limit == 0) {
            printk("timer: warning, no tick within the wait budget "
                   "(%u ticks so far)\n", g_ticks);
            return;
        }
    }
}

void timer_sleep_ms(u64 ms)
{
    u64 deadlines = (ms * g_timer_hz + 999) / 1000;
    if (!deadlines)
        deadlines = 1;
    timer_sleep_ticks(deadlines);
}

u64 timer_hz(void)
{
    return g_timer_hz;
}

u64 tsc_elapsed(void)
{
    return rdtsc() - g_boot_tsc;
}

/* ------------------------------------------------------------------ */
/* High Precision Event Timer                                         */
/* ------------------------------------------------------------------ */

#define HPET_REG_CAP   0x00
#define HPET_REG_PERIOD 0x04
#define HPET_REG_CONFIG 0x10
#define HPET_REG_COUNT  0xF0

static u64 g_hpet_base;
static u32 g_hpet_period_fs;

void hpet_init(void)
{
    void *madt = acpi_madt();
    AXIOM_UNUSED_VAR(madt);
    /* The HPET MMIO base comes from the ACPI HPET table; if it is absent the
     * PIT remains the time source, so this is not fatal. */
    void *hpet_table = NULL_PTR;
    if (!acpi_find_table("HPET", &hpet_table) || !hpet_table) {
        printk("hpet: table not present, using the PIT only\n");
        return;
    }
    {
        u8 *table = (u8 *)hpet_table;
        u64 base = *(u64 *)(table + 44);
        u64 phys = (base & 0xFFFFFFFFFFFFF000ULL);
        g_hpet_base = vmm_map_mmio(phys, 4096);
        if (!g_hpet_base) {
            printk("hpet: cannot map %p\n", (void *)phys);
            return;
        }
        g_hpet_period_fs = *(u32 *)(g_hpet_base + HPET_REG_PERIOD);
        u64 caps = *(u64 *)(g_hpet_base + HPET_REG_CAP);
        printk("hpet: mapped at %p, %u fs period, %u comparators\n",
               (void *)g_hpet_base, (u64)g_hpet_period_fs,
               (u64)((caps >> 8) & 0x1F));
        /* Enable the main counter. */
        *(u64 *)(g_hpet_base + HPET_REG_CONFIG) |= 1;
    }
    printk("hpet: counter running\n");
}

u64 hpet_nanoseconds(void)
{
    if (!g_hpet_base)
        return timer_ms() * 1000000ULL;
    u64 count = *(u64 *)(g_hpet_base + HPET_REG_COUNT);
    return (count * g_hpet_period_fs) / 1000000ULL;
}
