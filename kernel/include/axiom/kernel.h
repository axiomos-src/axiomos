/* AxiomOS - kernel-wide interfaces.
 *
 * Everything that crosses module boundaries is declared here or in one of the
 * sibling headers (mm.h, vfs.h, gui.h ...).  Keeping the prototypes in one
 * place makes the assembly/C/Rust ABI reviewable in a single pass.
 */
#ifndef AXIOM_KERNEL_H
#define AXIOM_KERNEL_H

#include <axiom/types.h>
#include "../boot/bootinfo.h"

/* ------------------------------------------------------------------ */
/* Boot / core                                                        */
/* ------------------------------------------------------------------ */

extern struct abp_boot_info *g_boot_info;

void     kernel_early_init(void);
void     kernel_main(void *boot_info) AXIOM_NORETURN;
void     kernel_late_init(void);
void     kernel_shutdown(void) AXIOM_NORETURN;
void     kernel_reboot(void) AXIOM_NORETURN;
void     kernel_halt(void) AXIOM_NORETURN;

/* Architecture – GDT/IDT/CPU */
void     gdt_init(void);
void     gdt_load(u64 gdtr_value, u16 code_sel, u16 data_sel);
void     tss_init(u64 kernel_stack);
void     idt_init(void);
void     idt_load(u64 idtr);
void     idt_set_gate(u8 vector, void *handler, u16 selector, u8 ist, u8 type);
void     cpu_init(void);
u64      cpu_brand_string(void);
bool_t   cpu_has_feature(u32 leaf, u32 bit);
void     cpu_print_features(void);
u32      cpu_apic_id(void);

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

enum log_level {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO  = 2,
    LOG_WARN  = 3,
    LOG_ERROR = 4,
    LOG_FATAL = 5
};

void     klog_init(enum log_level level);
void     klog_set_level(enum log_level level);
enum log_level klog_level(void);
void     printk(const char *fmt, ...) AXIOM_PRINTF(1, 2);
void     klog(enum log_level level, const char *fmt, ...) AXIOM_PRINTF(2, 3);
void     kputs(const char *s);
void     kputhex(u64 value);
void     kputdec(u64 value);
void     kputchar(char c);
void     panic(const char *fmt, ...) AXIOM_NORETURN AXIOM_PRINTF(1, 2);
void     kassert_fail(const char *expr, const char *file, int line) AXIOM_NORETURN;

#define KASSERT(x) do { if (!(x)) kassert_fail(#x, __FILE__, __LINE__); } while (0)
#define BUG_ON(x)  KASSERT(!(x))

/* ------------------------------------------------------------------ */
/* Serial and console                                                 */
/* ------------------------------------------------------------------ */

void     serial_init(u16 port);
void     serial_putc(char c);
void     serial_puts(const char *s);
void     serial_puthex(u64 v);
bool_t   serial_ready(void);

/* ------------------------------------------------------------------ */
/* Memory management                                                  */
/* ------------------------------------------------------------------ */

void     pmm_init(void);
u64      pmm_alloc_page(void);
u64      pmm_alloc_pages(u64 count);
void     pmm_free_page(u64 addr);
void     pmm_free_pages(u64 addr, u64 count);
u64      pmm_total_bytes(void);
u64      pmm_usable_bytes(void);
u64      pmm_used_bytes(void);
void     pmm_dump(void);
void     pmm_reserve(u64 base, u64 length);
bool_t   pmm_is_usable(u64 addr);

void     vmm_init(void);
bool_t   vmm_map(u64 virt, u64 phys, u64 flags);
bool_t   vmm_unmap(u64 virt);
bool_t   vmm_map_range(u64 virt, u64 phys, u64 size, u64 flags);
u64      vmm_map_mmio(u64 phys, u64 size);
u64      vmm_translate(u64 virt);
u64      vmm_kernel_pml4(void);
void     vmm_switch(u64 pml4);
u64      vmm_new_address_space(void);
void     vmm_destroy_address_space(u64 pml4);
void     vmm_protect(u64 virt, u64 size, u64 flags);
void     vmm_dump_stats(void);

/* Page flags shared by the kernel and the MMU code. */
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_PWT        (1ULL << 3)
#define PTE_PCD        (1ULL << 4)
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_HUGE       (1ULL << 7)
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_NX         (1ULL << 63)
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_NO_EXEC    PTE_NX

void     heap_init(u64 base, u64 size);
void    *kmalloc(u64 size);
void    *kzalloc(u64 size);
void    *krealloc(void *ptr, u64 size);
void     kfree(void *ptr);
void    *kmalloc_aligned(u64 size, u64 align);
u64      heap_used_bytes(void);
u64      heap_free_bytes(void);
void     heap_dump(void);

/* ------------------------------------------------------------------ */
/* Interrupts, timers and scheduling                                  */
/* ------------------------------------------------------------------ */

typedef void (*irq_handler_t)(u8 vector, void *frame);

void     irq_init(void);
void     irq_register(u8 vector, irq_handler_t handler, const char *name);
void     irq_unregister(u8 vector);
void     irq_dispatch(u8 vector, void *frame);
void     irq_mask(u8 irq);
void     irq_unmask(u8 irq);
u64      irq_count(u8 irq);
void     pic_remap(void);
void     pic_mask_all(void);
void     pit_init(u32 frequency_hz);
u64      timer_ticks(void);
u64      timer_ms(void);
void     timer_sleep_ms(u64 ms);
void     timer_sleep_ticks(u64 ticks);
void     hpet_init(void);
bool_t   apic_init(void);
void     apic_eoi(void);
void     apic_timer_init(u32 hz);
bool_t   apic_enabled(void);
u32      apic_id(void);
u32      apic_timer_calibrate(void);
bool_t   apic_timer_start(u32 hz);
void     apic_timer_stop(void);
void     apic_route_irq(u8 irq, u8 vector, u8 destination);
void     apic_mask_irq(u8 irq);
void     apic_unmask_irq(u8 irq);
void     apic_dump(void);
u64      boot_time_unix(void);
void     rtc_init(void);
void     rtc_read(u16 *year, u8 *month, u8 *day, u8 *hour, u8 *minute, u8 *second);
void     rtc_format(char *buffer, u32 size);
u32      rust_selftest(void);

void     sched_init(void);
void     sched_start(void);
u64      thread_create(void (*entry)(void *), void *arg, const char *name);
void     thread_yield(void);
void     thread_exit(void) AXIOM_NORETURN;
u64      sched_current_id(void);
const char *sched_current_name(void);
u32      sched_thread_count(void);
void     sched_tick(void);
void     sched_dump(void);

/* ------------------------------------------------------------------ */
/* Devices                                                            */
/* ------------------------------------------------------------------ */

void     pci_init(void);
void     pci_dump(void);
u32      pci_read32(u8 bus, u8 dev, u8 func, u8 off);
void     pci_write32(u8 bus, u8 dev, u8 func, u8 off, u32 value);
u16      pci_read16(u8 bus, u8 dev, u8 func, u8 off);
u8       pci_read8(u8 bus, u8 dev, u8 func, u8 off);
u64      pci_bar(u8 bus, u8 dev, u8 func, u8 bar);
bool_t   pci_find_class(u8 class_code, u8 subclass, u8 *bus, u8 *dev, u8 *func,
                        u8 index);
u8       pci_find_capability(u8 bus, u8 dev, u8 func, u8 cap_id);
u32      pci_device_count(void);

void     ps2_init(void);
void     ps2_poll(void);
bool_t   ps2_get_key(u8 *scancode, bool_t *pressed);
bool_t   ps2_get_mouse(i32 *dx, i32 *dy, u8 *buttons);
u8       ps2_modifiers(void);
char     keyboard_translate(u8 scancode, bool_t shift, bool_t caps);
i32      mouse_position_x(void);
i32      mouse_position_y(void);
u8       mouse_buttons(void);
void     mouse_set_position(i32 x, i32 y);
void     keyboard_init(void);

void     ahci_init(void);
bool_t   ahci_present(void);
bool_t   ahci_read(u64 lba, u16 count, void *buffer);
bool_t   ahci_write(u64 lba, u16 count, const void *buffer);
u64      ahci_disk_size(void);
void     ahci_dump(void);
void     ata_init(void);
bool_t   ata_read(u64 lba, u8 count, void *buffer);
bool_t   ata_write(u64 lba, u8 count, const void *buffer);

/* ------------------------------------------------------------------ */
/* ACPI                                                               */
/* ------------------------------------------------------------------ */

void     acpi_init(u64 rsdp);
u64      acpi_rsdp(void);
void     acpi_dump_tables(void);
u32      acpi_cpu_count(void);
u32      acpi_ioapic_address(void);
bool_t   acpi_find_table(const char *signature, void **table);
void    *acpi_madt(void);

/* ------------------------------------------------------------------ */
/* File systems                                                       */
/* ------------------------------------------------------------------ */

void     vfs_init(void);
void     fat32_init(void);
bool_t   fat32_mount(u64 partition_lba);
bool_t   fat32_list_root(void);
bool_t   fat32_read_file(const char *path, void **data, u64 *size);
bool_t   fat32_write_file(const char *path, const void *data, u64 size);
bool_t   fat32_stat(const char *path, u64 *size, u8 *attr);
void     fat32_dump(void);

struct ramfs_node;
void     ramfs_init(void);
bool_t   ramfs_create(const char *path, u64 size);
bool_t   ramfs_write(const char *path, const void *data, u64 size);
bool_t   ramfs_read(const char *path, void **data, u64 *size);
bool_t   ramfs_list(const char *path);
void     ramfs_dump(void);

/* ------------------------------------------------------------------ */
/* GUI                                                                */
/* ------------------------------------------------------------------ */

bool_t   gui_init(void);
void     gui_frame(void);
void     gui_handle_input(void);
bool_t   gui_active(void);
void     gui_shutdown(void);

/* ------------------------------------------------------------------ */
/* HVCI (Rust) and crypto                                            */
/* ------------------------------------------------------------------ */

typedef struct hvci_status {
    u32 active;
    u32 mode;               /* 0 disabled, 1 software, 2 VMX            */
    u64 protected_pages;
    u64 verified_pages;
    u64 violations;
    u64 manifest_hash;
    char detail[96];
} hvci_status_t;

void     hvci_init(void);
void     hvci_get_status(hvci_status_t *out);
bool_t   hvci_verify_kernel(void);
void     hvci_register_region(u64 base, u64 size, const char *name);
u64      hvci_hash_region(u64 base, u64 size);
void     hvci_protect_readonly(u64 base, u64 size);

u64      sha256_hash(const void *data, u64 size, u8 out[32]);
u32      crc32(const void *data, u64 size);
u64      fnv1a64(const void *data, u64 size);

/* ------------------------------------------------------------------ */
/* Syscalls and user mode                                             */
/* ------------------------------------------------------------------ */

void     syscall_init(void);
i64      syscall_dispatch(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
i64      user_spawn(const char *path, const char *args);
i64      elf_exec(const void *image, u64 size, const char *args);
void     usermode_init(void);

#endif /* AXIOM_KERNEL_H */
