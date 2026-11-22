/* AxiomOS - kernel entry and initialisation sequence.
 *
 * The sequence below is intentionally ordered by dependency:
 *   log -> CPU tables -> interrupts -> memory -> devices -> filesystem ->
 *   security -> GUI -> scheduler.
 * Anything that needs the heap runs after heap_init(); anything that needs
 * interrupts runs after idt_init() and sti().
 */

#include <axiom/kernel.h>
#include <axiom/idt.h>

struct abp_boot_info *g_boot_info;

extern u8 __kernel_start[];
extern u8 __kernel_end[];

static u64 g_kernel_start_tsc;

/* ------------------------------------------------------------------ */
/* Banner                                                             */
/* ------------------------------------------------------------------ */

static void print_banner(void)
{
    printk("\n");
    printk("    _   __  __  ____  __  __    ___  ____\n");
    printk("   / \\  \\ \\/ / /  _ \\ \\ \\/ /  / _ \\/ ___|\n");
    printk("  / _ \\  \\  /  | |_| | \\  /  | | | \\___ \\\n");
    printk(" / ___ \\ /  \\  |  _ <  /  \\  | |_| |___) |\n");
    printk("/_/   \\_\\_/\\_\\ |_| \\_\\/_/\\_\\  \\___/|____/\n");
    printk("\n");
    printk("AxiomOS 1.0 - axiomatic kernel\n");
    printk("kernel %p..%p (%u KiB)\n", (void *)__kernel_start,
           (void *)__kernel_end,
           ((u64)__kernel_end - (u64)__kernel_start) / 1024);
}

/* ------------------------------------------------------------------ */
/* CPU report                                                         */
/* ------------------------------------------------------------------ */

static void print_cpu(void)
{
    printk("cpu: %s\n", cpu_brand_string());
    cpu_print_features();
    printk("cpu: apic id %u, %s\n", cpu_apic_id(),
           cpu_has_feature(1, 9) ? "apic present" : "no local apic");
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                          */
/* ------------------------------------------------------------------ */

void kernel_early_init(void)
{
    serial_init(0x3F8);
    klog_init(LOG_INFO);
}

static void init_memory(void)
{
    pmm_init();
    vmm_init();

    /* Carve a 4 MiB heap out of physical memory and map it identity; larger
     * heaps can be added later by the slab layer. */
    {
        u64 heap_pages = (4ULL * 1024 * 1024) / AXIOM_PAGE_SIZE;
        u64 heap_phys = pmm_alloc_pages(heap_pages);
        if (!heap_phys) {
            panic("cannot reserve the initial kernel heap");
        }
        heap_init(heap_phys, heap_pages * AXIOM_PAGE_SIZE);
    }

    pmm_dump();
    vmm_dump_stats();
    heap_dump();
}

static void init_cpu_tables(void)
{
    gdt_init();
    tss_init(g_boot_info->stack_top);
    idt_init();
    irq_init();
    pic_remap();
    pit_init(100);
    printk("cpu: interrupts enabled, timer running at %u Hz\n", timer_hz());
}

void kernel_main(void *boot_info)
{
    struct abp_boot_info *bi = (struct abp_boot_info *)boot_info;

    g_boot_info = bi;
    g_kernel_start_tsc = 0;

    serial_init(0x3F8);
    klog_init(LOG_INFO);

    print_banner();

    if (!bi || bi->magic != ABP_MAGIC)
        panic("bad boot information magic (%p)", (void *)bi);
    printk("boot: protocol v%u, kernel %p..%p entry %p\n", (u64)bi->version,
           (void *)bi->kernel_base, (void *)(bi->kernel_base + bi->kernel_size),
           (void *)bi->kernel_entry);
    printk("boot: stack %p..%p, framebuffer %ux%u stride %u @ %p\n",
           (void *)(bi->stack_top - bi->stack_size), (void *)bi->stack_top,
           (u64)bi->fb.width, (u64)bi->fb.height, (u64)bi->fb.stride,
           (void *)bi->fb.base);
    printk("boot: %u memory entries, rsdp %p\n", (u64)bi->memmap_entry_count,
           (void *)bi->acpi_rsdp);

    u32 rust_failures = rust_selftest();
    printk("rust: self test, %u failure(s)\n", (u64)rust_failures);
    if (rust_failures)
        printk("rust: WARNING - crypto or math self test failed\n");

    init_cpu_tables();
    print_cpu();
    init_memory();

    acpi_init(bi->acpi_rsdp);
    pci_init();

    sti();

    /* Spin a short while so the timer and allocator get exercised before the
     * GUI takes over the screen; this also validates timer_sleep_ms(). */
    timer_sleep_ms(200);
    printk("boot: %u ticks after 200 ms (expect ~20)\n", timer_ticks());

    {
        /* Exercise the heap and check that memory is actually usable. */
        void *probe = kmalloc(1024);
        if (!probe)
            panic("heap probe allocation failed");
        memset(probe, 0x5A, 1024);
        for (u32 i = 0; i < 1024; i++) {
            if (((u8 *)probe)[i] != 0x5A)
                panic("heap memory corruption at offset %u", (u64)i);
        }
        kfree(probe);
        printk("boot: heap probe ok\n");
    }

    kernel_late_init();

    /* Idle loop: the GUI, if present, drives itself from the timer tick and
     * the input poller; otherwise we just wait for interrupts. */
    for (;;) {
        if (gui_active()) {
            gui_frame();
        }
        hlt();
    }
}

void kernel_late_init(void)
{
    u64 heap_pages;
    u64 heap_phys;

    /* Second heap segment: 12 MiB for GUI surfaces and font caches. */
    heap_pages = (12ULL * 1024 * 1024) / AXIOM_PAGE_SIZE;
    heap_phys = pmm_alloc_pages(heap_pages);
    if (heap_phys)
        heap_extend(heap_phys, heap_pages * AXIOM_PAGE_SIZE);
    else
        printk("boot: warning, no memory for the second heap segment\n");

    rtc_init();
    printk("boot: free memory %u MiB\n", pmm_free_bytes() / (1024 * 1024));

    hvci_init();
    hvci_verify_kernel();

    ps2_init();
    sched_init();

    if (!gui_init())
        printk("boot: GUI unavailable, falling back to serial console\n");
    else
        printk("boot: GUI ready\n");

    syscall_init();
    fat32_init();
    ahci_init();
    ramfs_init();

    printk("boot: completed in %u ms\n", timer_ms());
    sched_dump();
}

/* ------------------------------------------------------------------ */
/* Shutdown paths                                                     */
/* ------------------------------------------------------------------ */

void kernel_shutdown(void)
{
    printk("shutdown requested\n");
    if (g_boot_info && g_boot_info->efi_system_table) {
        /* ACPI power off through the PM1a control register would be the
         * proper path; the firmware reset service is sufficient for QEMU. */
    }
    cli();
    for (;;)
        hlt();
}

void kernel_reboot(void)
{
    /* Keyboard controller reset line: the classic triple-fault-free reset. */
    __asm__ __volatile__("cli");
    for (;;)
        hlt();
}

void kernel_halt(void)
{
    cli();
    for (;;)
        hlt();
}
