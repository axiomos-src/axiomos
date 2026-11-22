/* AxiomOS - local APIC and IOAPIC support.
 *
 * The kernel boots with the legacy 8259 PIC (see timer.c) because that is the
 * only interrupt controller which is guaranteed to exist on every x86_64
 * machine.  This module adds the APIC path: it locates the local APIC through
 * MSR 0x1B (or through the MADT on machines which report it there), maps its
 * register window into the MMIO window, brings up the spurious interrupt
 * vector, and programs the IOAPIC redirection table so that IRQ0 (the PIT)
 * and IRQ1/IRQ12 (PS/2) keep working once the PIC is masked off.
 *
 * Everything degrades cleanly: `apic_init` returns false and leaves the PIC
 * in charge when no APIC is present, so irq.c's `apic_enabled()` check keeps
 * sending EOI to the PIC.
 */

#include <axiom/kernel.h>
#include <axiom/idt.h>

/* Local APIC register offsets. */
#define LAPIC_ID              0x020
#define LAPIC_VERSION         0x030
#define LAPIC_TPR             0x080
#define LAPIC_EOI             0x0B0
#define LAPIC_SVR             0x0F0
#define LAPIC_ESR             0x280
#define LAPIC_LVT_CMCI        0x2F0
#define LAPIC_ICR_LO          0x300
#define LAPIC_ICR_HI          0x310
#define LAPIC_LVT_TIMER       0x320
#define LAPIC_LVT_THERMAL     0x330
#define LAPIC_LVT_PERF        0x340
#define LAPIC_LVT_LINT0       0x350
#define LAPIC_LVT_LINT1       0x360
#define LAPIC_LVT_ERROR       0x370
#define LAPIC_TIMER_INIT      0x380
#define LAPIC_TIMER_CURRENT   0x390
#define LAPIC_TIMER_DIVIDE    0x3E0

/* IOAPIC registers, accessed through an index/data pair. */
#define IOAPIC_REGSEL         0x00
#define IOAPIC_WINDOW         0x10
#define IOAPIC_REG_ID         0x00
#define IOAPIC_REG_VERSION    0x01
#define IOAPIC_REG_ARB        0x02
#define IOAPIC_REG_REDIR_BASE 0x10

#define APIC_SPURIOUS_VECTOR  0xFF
#define APIC_TIMER_VECTOR     (IRQ_BASE + 0)   /* shares IRQ0 with the PIT */

#define APIC_LVT_MASKED       (1u << 16)

static u8 *g_lapic = 0;
static u8 *g_ioapic = 0;
static u32 g_ioapic_entries = 0;
static bool_t g_enabled = false;
static bool_t g_x2apic = false;
static u64 g_bus_hz = 0;

static u32 lapic_read(u32 reg)
{
    if (!g_lapic)
        return 0;
    return *(volatile u32 *)(g_lapic + reg);
}

static void lapic_write(u32 reg, u32 value)
{
    if (!g_lapic)
        return;
    *(volatile u32 *)(g_lapic + reg) = value;
    (void)lapic_read(LAPIC_ID);   /* serialise the write */
}

static u32 ioapic_read(u32 reg)
{
    if (!g_ioapic)
        return 0;
    *(volatile u32 *)(g_ioapic + IOAPIC_REGSEL) = reg;
    return *(volatile u32 *)(g_ioapic + IOAPIC_WINDOW);
}

static void ioapic_write(u32 reg, u32 value)
{
    if (!g_ioapic)
        return;
    *(volatile u32 *)(g_ioapic + IOAPIC_REGSEL) = reg;
    *(volatile u32 *)(g_ioapic + IOAPIC_WINDOW) = value;
}

bool_t apic_enabled(void)
{
    return g_enabled;
}

u32 apic_id(void)
{
    if (g_x2apic) {
        u32 lo, hi;
        __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x802));
        return lo;
    }
    return lapic_read(LAPIC_ID) >> 24;
}

void apic_eoi(void)
{
    if (!g_enabled)
        return;
    lapic_write(LAPIC_EOI, 0);
}

/* ---------------------------------------------------------------- */
/* MSR helpers for the x2APIC mode                                   */
/* ---------------------------------------------------------------- */

#define MSR_APIC_BASE   0x1B
#define MSR_X2APIC_EOI  0x80B
#define MSR_X2APIC_SIVR 0x80F
#define MSR_X2APIC_TPR  0x808

static u64 read_msr_local(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void write_msr_local(u32 msr, u64 value)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((u32)value),
                      "d"((u32)(value >> 32)) : "memory");
}

/* ---------------------------------------------------------------- */
/* IOAPIC programming                                               */
/* ---------------------------------------------------------------- */

/* Program one redirection entry.  `irq` is the ISA IRQ number, `vector` the
 * IDT vector, `destination` the target local APIC id. */
void apic_route_irq(u8 irq, u8 vector, u8 destination)
{
    if (!g_ioapic || irq >= g_ioapic_entries)
        return;

    u32 low = vector;                       /* fixed delivery, physical dest */
    u32 high = ((u32)destination) << 24;
    u32 reg = IOAPIC_REG_REDIR_BASE + (u32)irq * 2;

    /* Mask while rewriting, then unmask. */
    ioapic_write(reg, low | APIC_LVT_MASKED);
    ioapic_write(reg + 1, high);
    ioapic_write(reg, low);
}

void apic_mask_irq(u8 irq)
{
    if (!g_ioapic || irq >= g_ioapic_entries)
        return;
    u32 reg = IOAPIC_REG_REDIR_BASE + (u32)irq * 2;
    ioapic_write(reg, ioapic_read(reg) | APIC_LVT_MASKED);
}

void apic_unmask_irq(u8 irq)
{
    if (!g_ioapic || irq >= g_ioapic_entries)
        return;
    u32 reg = IOAPIC_REG_REDIR_BASE + (u32)irq * 2;
    ioapic_write(reg, ioapic_read(reg) & ~APIC_LVT_MASKED);
}

void apic_dump(void)
{
    printk("apic: %s mode=%s id=%u ioapic_entries=%u\n",
           g_enabled ? "enabled" : "disabled",
           g_x2apic ? "x2apic" : "xapic", apic_id(),
           g_ioapic ? g_ioapic_entries : 0);
}

/* ---------------------------------------------------------------- */
/* Timer                                                            */
/* ---------------------------------------------------------------- */

/* Calibrate the LAPIC timer against the PIT: count down a known number of
 * PIT ticks and read the APIC timer's decrement.  On TCG this works fine
 * because both are emulated and the ratio is exact. */
u32 apic_timer_calibrate(void)
{
    if (!g_enabled)
        return 0;

    u32 divide = 0x3;                            /* divide by 16 */
    lapic_write(LAPIC_TIMER_DIVIDE, divide);
    lapic_write(LAPIC_LVT_TIMER, APIC_TIMER_VECTOR | APIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);

    /* Wait for ~10 ms using the PIT based reference. */
    timer_sleep_ms(10);

    u32 remaining = lapic_read(LAPIC_TIMER_CURRENT);
    u32 elapsed = 0xFFFFFFFFu - remaining;
    g_bus_hz = (u64)elapsed * 100u;              /* ticks per second */
    lapic_write(LAPIC_LVT_TIMER, APIC_LVT_MASKED);
    return elapsed;
}

bool_t apic_timer_start(u32 hz)
{
    if (!g_enabled || g_bus_hz == 0)
        return false;
    u32 period = (u32)(g_bus_hz / (hz ? hz : 100));
    if (period == 0)
        period = 1;
    lapic_write(LAPIC_TIMER_DIVIDE, 0x3);
    lapic_write(LAPIC_LVT_TIMER, APIC_TIMER_VECTOR | (0 << 17));
    lapic_write(LAPIC_TIMER_INIT, period);
    return true;
}

void apic_timer_stop(void)
{
    if (g_enabled)
        lapic_write(LAPIC_LVT_TIMER, APIC_LVT_MASKED);
}

/* ---------------------------------------------------------------- */
/* Initialisation                                                   */
/* ---------------------------------------------------------------- */

void apic_timer_init(u32 hz)
{
    if (apic_timer_calibrate() != 0)
        apic_timer_start(hz);
}

bool_t apic_init(void)
{
    /* The CPU must have an APIC: CPUID.1:EDX[9]. */
    if (!cpu_has_feature(1, 9))
        return false;

    u64 base_msr = read_msr_local(MSR_APIC_BASE);
    if (!(base_msr & (1u << 11))) {
        /* Globally disabled by firmware; try to enable it. */
        write_msr_local(MSR_APIC_BASE, base_msr | (1u << 11));
        base_msr = read_msr_local(MSR_APIC_BASE);
    }

    u64 physical = base_msr & 0xFFFFF000ull;
    if (!physical)
        physical = 0xFEE00000ull;

    if (cpu_has_x2apic()) {
        /* Virtualise the MSR interface: EOI, TPR and SVR go through MSRs. */
        write_msr_local(MSR_X2APIC_TPR, 0);
        write_msr_local(MSR_X2APIC_SIVR, APIC_SPURIOUS_VECTOR | 0x100);
        g_x2apic = true;
    } else {
        g_lapic = (u8 *)vmm_map_mmio(physical, 0x1000);
        if (!g_lapic)
            return false;
        lapic_write(LAPIC_TPR, 0);
        lapic_write(LAPIC_LVT_TIMER, APIC_LVT_MASKED);
        lapic_write(LAPIC_LVT_LINT0, APIC_LVT_MASKED);
        lapic_write(LAPIC_LVT_LINT1, APIC_LVT_MASKED);
        lapic_write(LAPIC_LVT_ERROR, 0xFE);
        lapic_write(LAPIC_SVR, APIC_SPURIOUS_VECTOR | 0x100);
        /* Clear a pending error by writing the ESR twice. */
        lapic_write(LAPIC_ESR, 0);
        lapic_write(LAPIC_ESR, 0);
    }

    /* Locate the IOAPIC through the ACPI MADT. */
    u64 ioapic_phys = acpi_ioapic_address();
    if (!ioapic_phys)
        ioapic_phys = 0xFEC00000ull;      /* architectural default */
    g_ioapic = (u8 *)vmm_map_mmio(ioapic_phys, 0x1000);
    if (!g_ioapic) {
        g_enabled = (bool_t)!g_x2apic;
        return g_enabled;
    }
    u32 version = ioapic_read(IOAPIC_REG_VERSION);
    g_ioapic_entries = (version >> 16) & 0xFF;
    if (g_ioapic_entries > 24)
        g_ioapic_entries = 24;

    /* Mask every redirection entry before the PIC goes away. */
    for (u32 i = 0; i < g_ioapic_entries; i++)
        ioapic_write(IOAPIC_REG_REDIR_BASE + i * 2, APIC_LVT_MASKED);

    /* Route the legacy ISA interrupts which the kernel uses.  The PIC keeps
     * them masked, so the IOAPIC is the single owner. */
    apic_route_irq(0, IRQ_BASE + 0, (u8)apic_id());      /* PIT   */
    apic_route_irq(1, IRQ_BASE + 1, (u8)apic_id());      /* kbd   */
    apic_route_irq(2, IRQ_BASE + 2, (u8)apic_id());      /* casc. */
    apic_route_irq(4, IRQ_BASE + 4, (u8)apic_id());      /* COM1  */
    apic_route_irq(12, IRQ_BASE + 12, (u8)apic_id());    /* mouse */

    g_enabled = true;
    return true;
}
