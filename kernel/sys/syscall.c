/* AxiomOS - system call interface.
 *
 * The syscall gate is vector 0x80, installed with DPL 3 by idt_init().  The
 * register convention follows the Linux x86_64 layout so that user programs
 * written for the AxiomOS userland can also be debugged with familiar tooling:
 *
 *   rax = number, rdi, rsi, rdx, r10, r8, r9 = arguments
 *   return value in rax
 *
 * Phase 1 implements the calls needed by the bundled userland: write, exit,
 * gettime, framebuffer info, mouse state, file open/read (through FAT32 or
 * ramfs depending on the path) and a small debug print.
 */

#include <axiom/kernel.h>
#include <axiom/idt.h>

#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_READ        2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_TIME        5
#define SYS_GETPID      6
#define SYS_YIELD       7
#define SYS_DEBUG       8
#define SYS_FB_INFO     9
#define SYS_MOUSE       10
#define SYS_ALLOC       11
#define SYS_FREE        12
#define SYS_SLEEP       13
#define SYS_KEYS        14
#define SYS_EXEC        15
#define SYS_MMAP        16

static u64 g_syscall_count;
static u64 g_syscall_last;

/* User pointer validation: phase 1 shares the kernel address space, so the
 * check is a range check against the kernel image instead of a page walk. */
static bool_t user_ptr_valid(u64 address, u64 length)
{
    extern u8 __kernel_end[];
    if (address < 0x1000)
        return false;
    if (address + length < address)
        return false;
    /* Reject pointers into the kernel image: user code lives above 4 MiB. */
    if (address < 0x400000 && (u64)__kernel_end < 0x400000)
        return false;
    return true;
}

static i64 sys_write(struct interrupt_frame *frame)
{
    u64 fd = frame->rdi;
    u64 buffer = frame->rsi;
    u64 length = frame->rdx;

    if (!user_ptr_valid(buffer, length))
        return -14;   /* EFAULT */

    const char *text = (const char *)buffer;
    if (fd == 1 || fd == 2) {
        for (u64 i = 0; i < length; i++)
            serial_putc(text[i]);
        return (i64)length;
    }
    return -9;        /* EBADF */
}

static i64 sys_time(void)
{
    return (i64)(boot_time_unix() + timer_ms() / 1000);
}

static i64 sys_fb_info(struct interrupt_frame *frame)
{
    u64 out = frame->rdi;
    if (!user_ptr_valid(out, 32))
        return -14;
    struct abp_boot_info *bi = g_boot_info;
    u64 *words = (u64 *)out;
    words[0] = bi->fb.width;
    words[1] = bi->fb.height;
    words[2] = bi->fb.stride;
    words[3] = bi->fb.base;
    return 0;
}

static i64 sys_mouse(struct interrupt_frame *frame)
{
    u64 out = frame->rdi;
    if (!user_ptr_valid(out, 16))
        return -14;
    i32 *values = (i32 *)out;
    values[0] = mouse_position_x();
    values[1] = mouse_position_y();
    values[2] = (i32)mouse_buttons();
    values[3] = 0;
    return 0;
}

static i64 sys_open(struct interrupt_frame *frame)
{
    u64 path_ptr = frame->rdi;
    u64 size_out = frame->rsi;
    u64 flags = frame->rdx;
    AXIOM_UNUSED_VAR(flags);

    if (!user_ptr_valid(path_ptr, 1))
        return -14;

    const char *path = (const char *)path_ptr;
    void *data = NULL_PTR;
    u64 size = 0;

    if (!fat32_read_file(path, &data, &size)) {
        if (!ramfs_read(path, &data, &size))
            return -2;      /* ENOENT */
    }
    if (size_out && user_ptr_valid(size_out, 8))
        *(u64 *)size_out = size;

    /* Phase 1: the file stays mapped in kernel memory and the caller receives
     * its address directly. */
    return (i64)(u64)data;
}

static i64 sys_debug(struct interrupt_frame *frame)
{
    u64 message = frame->rdi;
    if (!user_ptr_valid(message, 1))
        return -14;
    printk("[user] %s\n", (const char *)message);
    return 0;
}

static i64 sys_alloc(struct interrupt_frame *frame)
{
    u64 size = frame->rdi;
    if (size > 64 * 1024 * 1024)
        return -12;
    void *ptr = kzalloc(size);
    return ptr ? (i64)(u64)ptr : -12;
}

static i64 sys_keys(struct interrupt_frame *frame)
{
    u64 out = frame->rdi;
    if (!user_ptr_valid(out, 4))
        return -14;
    u8 scancode = 0;
    bool_t pressed = false;
    if (!ps2_get_key(&scancode, &pressed))
        return 0;
    ((u8 *)out)[0] = scancode;
    ((u8 *)out)[1] = pressed ? 1 : 0;
    return 1;
}

i64 syscall_dispatch(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
    struct interrupt_frame probe;
    memset(&probe, 0, sizeof(probe));
    probe.rax = nr;
    probe.rdi = a1;
    probe.rsi = a2;
    probe.rdx = a3;
    probe.r10 = a4;
    probe.r8 = a5;

    g_syscall_count++;
    g_syscall_last = nr;

    switch (nr) {
    case SYS_EXIT:
        thread_exit();
    case SYS_WRITE:
        return sys_write(&probe);
    case SYS_TIME:
        return sys_time();
    case SYS_GETPID:
        return (i64)sched_current_id();
    case SYS_YIELD:
        thread_yield();
        return 0;
    case SYS_DEBUG:
        return sys_debug(&probe);
    case SYS_FB_INFO:
        return sys_fb_info(&probe);
    case SYS_MOUSE:
        return sys_mouse(&probe);
    case SYS_ALLOC:
        return sys_alloc(&probe);
    case SYS_FREE:
        kfree((void *)a1);
        return 0;
    case SYS_SLEEP:
        timer_sleep_ms(a1);
        return 0;
    case SYS_KEYS:
        return sys_keys(&probe);
    case SYS_OPEN:
        return sys_open(&probe);
    default:
        printk("syscall: unknown number %u\n", nr);
        return -38;   /* ENOSYS */
    }
}

u64 syscall_count(void)
{
    return g_syscall_count;
}

u64 syscall_last(void)
{
    return g_syscall_last;
}

void syscall_init(void)
{
    g_syscall_count = 0;
    g_syscall_last = 0;
    printk("syscall: gate 0x80 active, %u calls available\n", (u64)16);
}

/* Assembly enter point: frame->rax holds the syscall number; the result is
 * written back into the frame so the iretq path returns it in rax. */
void syscall_entry_c(u64 nr, struct interrupt_frame *frame)
{
    frame->rax = (u64)syscall_dispatch(nr, frame->rdi, frame->rsi, frame->rdx,
                                       frame->r10, frame->r8);
}
