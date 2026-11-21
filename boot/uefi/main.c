/*
 * AxiomOS UEFI boot loader - main entry point.
 *
 * Boot sequence
 *   1. initialise firmware console, print the banner
 *   2. acquire the firmware memory map (informational pass)
 *   3. bring up GOP and render the splash screen
 *   4. locate the ACPI RSDP
 *   5. open the ESP, read kernel.elf, place its segments
 *   6. build the Axiom Boot Protocol block and a kernel stack
 *   7. normalise the memory map and call ExitBootServices
 *   8. transfer control to the kernel entry point
 *
 * Every failure is reported through the firmware console *and* written to
 * \axiomos\bootlog.txt so that a failed boot inside a VM can be diagnosed
 * without a serial port.
 */

#include "loader.h"

#define LOADER_NAME     "AxiomOS UEFI Loader"
#define LOADER_VERSION  "1.0.0"
#define KERNEL_PATH     "/axiomos/kernel.elf"

static efi_system_table *g_st;
static efi_boot_services *g_bs;
static efi_handle g_image;
static struct root_volume g_vol;
static char g_log[4096];
static int g_log_used;

static void log_line(const char *fmt, ...)
{
    __builtin_va_list ap;
    char tmp[256];
    int n = 0;
    const char *p = fmt;

    __builtin_va_start(ap, fmt);
    while (*p && n < 250) {
        if (*p == '%') {
            p++;
            while (*p == '-' || *p == '+' || *p == '0' || *p == '.' ||
                   *p == ' ' || (*p >= '1' && *p <= '9')) {
                p++;        /* skip width/precision, as con_printf does */
            }
            switch (*p) {
            case 's': {
                const char *s = __builtin_va_arg(ap, const char *);
                while (s && *s && n < 250)
                    tmp[n++] = *s++;
                break;
            }
            case 'd': {
                char num[24];
                i64 v = (i64)__builtin_va_arg(ap, long long);
                u_to_dec((u64)(v < 0 ? -v : v), num);
                if (v < 0 && n < 250)
                    tmp[n++] = '-';
                for (char *q = num; *q && n < 250; q++)
                    tmp[n++] = *q;
                break;
            }
            case 'u': {
                char num[24];
                u64 v = (u64)__builtin_va_arg(ap, unsigned long long);
                u_to_dec(v, num);
                for (char *q = num; *q && n < 250; q++)
                    tmp[n++] = *q;
                break;
            }
            case 'x': {
                char hex[24];
                u64 v = (u64)__builtin_va_arg(ap, unsigned long long);
                u_to_hex(v, hex, 1);
                for (char *q = hex; *q && n < 250; q++)
                    tmp[n++] = *q;
                break;
            }
            default:
                if (n < 250)
                    tmp[n++] = '%';
                break;
            }
        } else {
            tmp[n++] = *p;
        }
        p++;
    }
    __builtin_va_end(ap);
    tmp[n++] = '\n';
    tmp[n] = 0;

    con_puts(tmp);
    /* Mirror into the boot log buffer. */
    for (int i = 0; i < n && g_log_used < (int)sizeof(g_log) - 1; i++)
        g_log[g_log_used++] = tmp[i];
}

static void save_log(void)
{
    if (g_vol.root && g_log_used > 0) {
        char header[64];
        int n = 0;
        const char *p = "AxiomOS loader log\n\n";
        while (*p)
            header[n++] = *p++;
        header[n] = 0;
        /* Prepend the header by shifting the buffer.  Small enough to bother. */
        for (int i = g_log_used - 1; i >= 0; i--)
            g_log[i + n] = g_log[i];
        for (int i = 0; i < n; i++)
            g_log[i] = header[i];
        g_log_used += n;
        fs_write_file(&g_vol, "/axiomos/bootlog.txt", g_log, (u64)g_log_used);
    }
}

static void fatal(const char *message)
{
    log_line("FATAL: %s", message);
    con_printf("\r\n*** AxiomOS loader halted: %s ***\r\n", message);
    save_log();
    g_st->boot_services->set_watchdog_timer(0, 0, 0, 0);
    g_st->runtime_services->reset_system(2 /* EfiResetShutdown */,
                                         EFI_SUCCESS, 0, 0);
    halt_forever();
}

efi_status EFIAPI efi_main(efi_handle image_handle, efi_system_table *system_table)
{
    struct memmap_state mm;
    struct loaded_elf kernel;
    struct gop_state gop;
    struct abp_boot_info *bi;
    u64 stack_base;
    u64 kernel_stack_top;
    efi_uint32 banner_percent = 0;
    u64 rsdp;

    g_st = system_table;
    g_bs = system_table->boot_services;
    g_image = image_handle;

    g_bs->set_watchdog_timer(0, 0, 0, 0);
    con_init(system_table);

    log_line("%s %s starting", LOADER_NAME, LOADER_VERSION);
    log_line("firmware: %s", (const char *)"UEFI");
    log_line("boot services at %x, runtime services at %x",
             (u64)g_bs, (u64)system_table->runtime_services);

    /* ---------------- memory map ------------------------------------ */
    if (memmap_acquire(g_bs, &mm) != 0)
        fatal("cannot acquire the firmware memory map");

    /* ---------------- graphics -------------------------------------- */
    if (gop_init(g_bs, &gop) != 0) {
        log_line("warning: no linear framebuffer, kernel will use serial only");
        u_memset(&gop, 0, sizeof(gop));
    } else {
        gop_banner(&gop);
        banner_percent = 10;
        gop_progress(&gop, banner_percent);
    }

    /* ---------------- ACPI ------------------------------------------ */
    rsdp = acpi_find_rsdp(system_table);
    if (rsdp)
        log_line("ACPI RSDP at %x", rsdp);
    banner_percent = 35;
    if (gop.proto)
        gop_progress(&gop, banner_percent);

    /* ---------------- file system ----------------------------------- */
    if (fs_open_root(g_bs, image_handle, &g_vol) != 0)
        fatal("no readable EFI system partition");
    banner_percent = 50;
    if (gop.proto)
        gop_progress(&gop, banner_percent);

    log_line("loading kernel image '%s'", KERNEL_PATH);
    if (elf_load(&g_vol, g_bs, KERNEL_PATH, &kernel) != 0)
        fatal("kernel.elf is missing or malformed (see /axiomos/kernel.elf)");
    log_line("kernel entry %x base %x size %x", kernel.entry, kernel.base,
             kernel.size);
    banner_percent = 65;
    if (gop.proto)
        gop_progress(&gop, banner_percent);

    /* ---------------- kernel stack ---------------------------------- */
    stack_base = memmap_alloc_any_raw(64 * 1024, EfiLoaderData);
    if (!stack_base)
        fatal("cannot allocate the kernel stack");
    kernel_stack_top = stack_base + 64 * 1024 - 16;
    log_line("kernel stack %x..%x", stack_base, kernel_stack_top);

    /* ---------------- boot info block -------------------------------- */
    bi = (struct abp_boot_info *)memmap_alloc_any_raw(sizeof(struct abp_boot_info),
                                                      EfiLoaderData);
    if (!bi)
        fatal("cannot allocate the boot information block");
    u_memset(bi, 0, sizeof(*bi));
    bi->magic = ABP_MAGIC;
    bi->version = ABP_VERSION;
    bi->size = sizeof(struct abp_boot_info);

    bi->kernel_base = kernel.base;
    bi->kernel_size = kernel.size;
    bi->kernel_entry = kernel.entry;
    bi->stack_top = kernel_stack_top;
    bi->stack_size = 64 * 1024;
    bi->acpi_rsdp = rsdp;
    bi->efi_system_table = (u64)system_table;
    bi->efi_image_handle = (u64)image_handle;

    bi->fb.base = gop.fb_base;
    bi->fb.width = gop.width;
    bi->fb.height = gop.height;
    bi->fb.stride = gop.stride;
    bi->fb.bpp = gop.bpp;
    bi->fb.red_mask = gop.red_mask;
    bi->fb.green_mask = gop.green_mask;
    bi->fb.blue_mask = gop.blue_mask;
    bi->fb.reserved_mask = gop.reserved_mask;
    bi->fb.pixel_format = gop.pixel_format;
    bi->fb.rotate = 0;

    u_strcpy(bi->loader_name, LOADER_NAME, 64);
    u_strcpy(bi->loader_version, LOADER_VERSION, 32);
    u_strcpy(bi->cmdline, "console=serial,fb root=fat32 init=/bin/init hvci=auto",
             1024);

    /* ---------------- normalise + handoff ---------------------------- */
    if (memmap_normalize(&mm, kernel.base, kernel.size, (u64)bi,
                         sizeof(struct abp_boot_info), gop.fb_base,
                         gop.fb_size) != 0)
        fatal("cannot normalise the memory map");
    bi->memmap_entries = (u64)mm.entries;
    bi->memmap_entry_count = mm.entry_count;
    bi->memmap_entry_size = sizeof(struct abp_memory_entry);
    bi->modules = 0;
    bi->module_count = 0;
    bi->boot_unix_time = 0;

    banner_percent = 85;
    if (gop.proto) {
        gop_progress(&gop, banner_percent);
        gop_progress(&gop, 100);
    }
    log_line("handing off: %u MiB usable RAM", mm.total_ram / (1024 * 1024));
    save_log();

    if (memmap_exit_from_map(g_bs, image_handle, &mm) != 0)
        fatal("ExitBootServices failed");

    /* Firmware is now out of the picture.  Interrupts are disabled here to
     * avoid taking a firmware-injected interrupt on the old IDT. */
    __asm__ __volatile__("cli");

    /* Reset the FPU/SSE state and mask exceptions so the kernel starts from a
     * known state even if firmware left CR0.TS or MXCSR dirty. */
    __asm__ __volatile__("fninit");

    axi_jump_to_kernel(kernel.entry, (u64)bi, kernel_stack_top);
    return EFI_SUCCESS; /* not reached */
}


