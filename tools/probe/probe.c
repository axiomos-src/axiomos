/* Minimal UEFI probe application used to validate the VMware EFI boot path.
 * Writes directly to the firmware console and to COM1, then parks the CPU.
 */

typedef unsigned long long u64;
typedef unsigned short     u16;
typedef unsigned char      u8;

typedef struct { u64 a; u64 b; } efi_table_header_ish;

typedef struct {
    u16 year, month, day, hour, minute, second, pad1, ms, tz, tzday, pad2;
    unsigned int nanosecond;
} efi_time_dummy;

typedef struct {
    efi_table_header_ish hdr;
    void *notify_fn;
    efi_time_dummy *get_time;
    efi_time_dummy *set_time;
    efi_time_dummy *get_wakeup;
    efi_time_dummy *set_wakeup;
    efi_time_dummy *set_virtual;
    efi_time_dummy *get_var;
    efi_time_dummy *get_next_var;
    efi_time_dummy *set_var;
    efi_time_dummy *get_var_info;
    u64 *get_next_high_mono;
    void (*reset_system)(int, int, u64, void *);
} efi_runtime_services;

typedef struct {
    efi_table_header_ish hdr;
    char *reset;
    void (*output_string)(void *con, const char *text);
    void (*output_test_string)(void *con, const char *text);
} efi_simple_text_output;

typedef struct {
    efi_table_header_ish hdr;
    char *raise_tpl;
    char *restore_tpl;
    void *allocate_pages;
    void *free_pages;
    void *get_memory_map;
    void *allocate_pool;
    void *free_pool;
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol;
    void *reinstall_protocol;
    void *uninstall_protocol;
    void *handle_protocol;
    char *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    char *start_image;
    void *exit;
    void *unload_image;
    void *exit_boot_services;
    void *get_next_monotonic;
    void *stall;
    void (*set_watchdog_timer)(u64, u64, u64, void *);
} efi_boot_services;

typedef struct {
    efi_table_header_ish hdr;
    char *firmware_vendor;
    unsigned int firmware_revision;
    void *console_in_handle;
    void *con_in;
    void *console_out_handle;
    efi_simple_text_output *con_out;
    void *standard_error_handle;
    efi_simple_text_output *std_err;
    efi_runtime_services *runtime_services;
    efi_boot_services *boot_services;
    u64 number_of_table_entries;
    void *configuration_table;
} efi_system_table;

typedef u64 efi_status;
typedef void *efi_handle;

static void uart_write(const char *s)
{
    while (*s) {
        u8 c = (u8)*s++;
        int guard = 0;
        while (((*(volatile u8 *)0x3FD) & 0x20) == 0 && guard < 200000)
            guard++;
        *(volatile u8 *)0x3F8 = c;
    }
}

static void halt_forever(void)
{
    for (;;)
        __asm__ volatile("hlt");
}

efi_status efi_main(efi_handle image_handle, efi_system_table *st)
{
    (void)image_handle;

    st->boot_services->set_watchdog_timer(0, 0, 0, 0);

    uart_write("\r\nPROBE: efi_main entered\r\n");
    if (st->con_out && st->con_out->output_string)
        st->con_out->output_string(st->con_out, "PROBE: console alive\r\n");

    uart_write("PROBE: parking\r\n");
    halt_forever();
    return 0;
}
