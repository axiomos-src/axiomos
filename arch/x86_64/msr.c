/* AxiomOS - model specific register access and feature policy.
 *
 * Reading and writing MSRs is trivial; the interesting part here is the policy
 * layer that decides which registers the kernel allows itself to touch based on
 * what the CPU advertises.  Every write records the previous value so that a
 * panic dump can show what the kernel changed.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXMSR_EFER               0xC0000080u
#define AXMSR_STAR               0xC0000081u
#define AXMSR_LSTAR              0xC0000082u
#define AXMSR_CSTAR              0xC0000083u
#define AXMSR_SFMASK             0xC0000084u
#define AXMSR_FS_BASE            0xC0000100u
#define AXMSR_GS_BASE            0xC0000101u
#define AXMSR_KERNEL_GS_BASE     0xC0000102u
#define AXMSR_TSC                0x00000010u
#define AXMSR_APIC_BASE          0x0000001Bu
#define AXMSR_MTRR_CAP           0x000000FEu
#define AXMSR_MTRR_DEF_TYPE      0x000002FFu
#define AXMSR_PAT                0x00000277u
#define AXMSR_SYSENTER_CS        0x00000174u
#define AXMSR_SYSENTER_ESP       0x00000175u
#define AXMSR_SYSENTER_EIP       0x00000176u
#define AXMSR_DEBUG_CTL          0x000001D9u
#define AXMSR_PERF_CAP           0x00000345u
#define AXMSR_SPEC_CTRL          0x00000048u
#define AXMSR_PRED_CMD           0x00000049u
#define AXMSR_FLUSH_CMD          0x0000010Bu
#define AXMSR_ARCH_CAP           0x0000010Au
#define AXMSR_GS_SWAP            0x0000010Cu

#define AXMSR_EFER_SCE           0x00000001ull
#define AXMSR_EFER_LME           0x00000100ull
#define AXMSR_EFER_LMA           0x00000400ull
#define AXMSR_EFER_NXE           0x00000800ull
#define AXMSR_EFER_SVME          0x00001000ull
#define AXMSR_EFER_SVME_ENABLE   0x00001000ull

#define AXMSR_HISTORY_SLOTS      32
#define AXMSR_WRITE_LIMIT        4096

typedef struct axmsr_record
{
    u32  msr;
    u64  old_value;
    u64  new_value;
    u32  sequence;
    bool_t valid;
} axmsr_record_t;

typedef struct axmsr_state
{
    axmsr_record_t history[AXMSR_HISTORY_SLOTS];
    u32  history_head;
    u32  reads;
    u32  writes;
    u32  rejects;
    u32  faults;
    u64  efer;
    u64  star;
    u64  lstar;
    u64  kernel_gs_base;
    u64  pat;
    u64  spec_ctrl;
    u64  arch_caps;
    u32  apic_base;
    bool_t nxe_enabled;
    bool_t syscall_enabled;
    bool_t svm_enabled;
    bool_t ready;
} axmsr_state_t;

static axmsr_state_t axmsr_state;

static void axmsr_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static bool_t axmsr_allowed(u32 msr)
{
    switch (msr)
    {
    case AXMSR_EFER:
    case AXMSR_STAR:
    case AXMSR_LSTAR:
    case AXMSR_SFMASK:
    case AXMSR_FS_BASE:
    case AXMSR_GS_BASE:
    case AXMSR_KERNEL_GS_BASE:
    case AXMSR_TSC:
    case AXMSR_APIC_BASE:
    case AXMSR_PAT:
    case AXMSR_SYSENTER_CS:
    case AXMSR_SYSENTER_ESP:
    case AXMSR_SYSENTER_EIP:
    case AXMSR_SPEC_CTRL:
    case AXMSR_ARCH_CAP:
    case AXMSR_GS_SWAP:
        return true;
    default:
        return false;
    }
}

/* Real MSR instructions are emitted by the assembly helpers; the freestanding
 * unit test build substitutes the shadow storage below. */
static u64 axmsr_hw_read(u32 msr)
{
    u32 low = 0;
    u32 high = 0;
    AXIOM_UNUSED_VAR(msr);
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((u64)high << 32) | (u64)low;
}

static void axmsr_hw_write(u32 msr, u64 value)
{
    u32 low = (u32)(value & 0xFFFFFFFFu);
    u32 high = (u32)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static void axmsr_record(u32 msr, u64 old_value, u64 new_value)
{
    axmsr_record_t *record = &axmsr_state.history[axmsr_state.history_head];
    record->msr = msr;
    record->old_value = old_value;
    record->new_value = new_value;
    record->sequence = axmsr_state.writes;
    record->valid = true;
    axmsr_state.history_head = (axmsr_state.history_head + 1) % AXMSR_HISTORY_SLOTS;
}

/* -------------------------------------------------------------- public API */

void axmsr_init(void)
{
    axmsr_zero(&axmsr_state, (u32)sizeof(axmsr_state));
    axmsr_state.ready = true;
}

u64 axmsr_read(u32 msr)
{
    if (!axmsr_state.ready || !axmsr_allowed(msr))
    {
        axmsr_state.rejects++;
        return 0;
    }
    axmsr_state.reads++;
    return axmsr_hw_read(msr);
}

bool_t axmsr_write(u32 msr, u64 value)
{
    u64 old_value;
    if (!axmsr_state.ready || !axmsr_allowed(msr))
    {
        axmsr_state.rejects++;
        return false;
    }
    if (axmsr_state.writes >= AXMSR_WRITE_LIMIT)
        return false;
    old_value = axmsr_hw_read(msr);
    axmsr_hw_write(msr, value);
    axmsr_state.writes++;
    axmsr_record(msr, old_value, value);
    switch (msr)
    {
    case AXMSR_EFER:
        axmsr_state.efer = value;
        axmsr_state.nxe_enabled = (value & AXMSR_EFER_NXE) != 0;
        axmsr_state.syscall_enabled = (value & AXMSR_EFER_SCE) != 0;
        axmsr_state.svm_enabled = (value & AXMSR_EFER_SVME) != 0;
        break;
    case AXMSR_STAR:
        axmsr_state.star = value;
        break;
    case AXMSR_LSTAR:
        axmsr_state.lstar = value;
        break;
    case AXMSR_KERNEL_GS_BASE:
        axmsr_state.kernel_gs_base = value;
        break;
    case AXMSR_PAT:
        axmsr_state.pat = value;
        break;
    case AXMSR_SPEC_CTRL:
        axmsr_state.spec_ctrl = value;
        break;
    case AXMSR_ARCH_CAP:
        axmsr_state.arch_caps = value;
        break;
    case AXMSR_APIC_BASE:
        axmsr_state.apic_base = (u32)(value & 0xFFFFF000u);
        break;
    default:
        break;
    }
    return true;
}

bool_t axmsr_enable_nxe(void)
{
    u64 efer = axmsr_read(AXMSR_EFER);
    efer |= AXMSR_EFER_NXE;
    return axmsr_write(AXMSR_EFER, efer);
}

bool_t axmsr_enable_syscall(u64 star, u64 lstar, u64 sfmask)
{
    u64 efer;
    if (!axmsr_write(AXMSR_STAR, star))
        return false;
    if (!axmsr_write(AXMSR_LSTAR, lstar))
        return false;
    if (!axmsr_write(AXMSR_SFMASK, sfmask))
        return false;
    efer = axmsr_read(AXMSR_EFER);
    efer |= AXMSR_EFER_SCE;
    return axmsr_write(AXMSR_EFER, efer);
}

bool_t axmsr_set_kernel_stack(u64 address)
{
    return axmsr_write(AXMSR_KERNEL_GS_BASE, address);
}

bool_t axmsr_enable_svm(void)
{
    u64 efer = axmsr_read(AXMSR_EFER);
    efer |= AXMSR_EFER_SVME_ENABLE;
    return axmsr_write(AXMSR_EFER, efer);
}

bool_t axmsr_enable_apic(u32 base, bool_t bsp)
{
    u64 value = (u64)(base & 0xFFFFF000u) | 0x800u;
    if (bsp)
        value |= 0x100u;
    return axmsr_write(AXMSR_APIC_BASE, value);
}

bool_t axmsr_set_pat(u64 value)
{
    return axmsr_write(AXMSR_PAT, value);
}

u32 axmsr_history_count(void)
{
    u32 i;
    u32 count = 0;
    for (i = 0; i < AXMSR_HISTORY_SLOTS; i++)
    {
        if (axmsr_state.history[i].valid)
            count++;
    }
    return count;
}

bool_t axmsr_get_history(u32 index, u32 *msr, u64 *old_value, u64 *new_value)
{
    u32 cursor;
    if (index >= AXMSR_HISTORY_SLOTS)
        return false;
    cursor = (axmsr_state.history_head + AXMSR_HISTORY_SLOTS - 1 - index) % AXMSR_HISTORY_SLOTS;
    if (!axmsr_state.history[cursor].valid)
        return false;
    if (msr != NULL_PTR)
        *msr = axmsr_state.history[cursor].msr;
    if (old_value != NULL_PTR)
        *old_value = axmsr_state.history[cursor].old_value;
    if (new_value != NULL_PTR)
        *new_value = axmsr_state.history[cursor].new_value;
    return true;
}

bool_t axmsr_features(void)
{
    return axmsr_state.nxe_enabled && axmsr_state.syscall_enabled;
}

u64 axmsr_arch_capabilities(void)
{
    return axmsr_state.arch_caps;
}

bool_t axmsr_spectre_mitigated(void)
{
    return (axmsr_state.spec_ctrl & 0x3ull) != 0;
}

void axmsr_report(void)
{
    u32 i;
    printk("axmsr: reads=%u writes=%u rejects=%u nxe=%u syscall=%u svm=%u\n",
           (unsigned)axmsr_state.reads, (unsigned)axmsr_state.writes,
           (unsigned)axmsr_state.rejects, (unsigned)axmsr_state.nxe_enabled,
           (unsigned)axmsr_state.syscall_enabled, (unsigned)axmsr_state.svm_enabled);
    printk("axmsr: efer=0x%lx lstar=0x%lx kgs=0x%lx apic=0x%x\n",
           (unsigned long)axmsr_state.efer, (unsigned long)axmsr_state.lstar,
           (unsigned long)axmsr_state.kernel_gs_base, (unsigned)axmsr_state.apic_base);
    for (i = 0; i < AXMSR_HISTORY_SLOTS; i++)
    {
        const axmsr_record_t *record = &axmsr_state.history[i];
        if (!record->valid)
            continue;
        printk("  msr 0x%x 0x%lx -> 0x%lx\n", (unsigned)record->msr,
               (unsigned long)record->old_value, (unsigned long)record->new_value);
    }
}

bool_t axmsr_selftest(void)
{
    axmsr_init();
    if (!axmsr_enable_nxe())
        return false;
    if (!axmsr_state.nxe_enabled)
        return false;
    if (!axmsr_enable_syscall(0x0020001000000000ull, 0xFFFFFFFF80100000ull, 0x200))
        return false;
    if (!axmsr_state.syscall_enabled)
        return false;
    if (!axmsr_set_kernel_stack(0xFFFF800000001000ull))
        return false;
    if (axmsr_read(0x12345678u) != 0)
        return false;
    if (axmsr_state.rejects != 1)
        return false;
    if (!axmsr_enable_svm())
        return false;
    if (!axmsr_state.svm_enabled)
        return false;
    if (!axmsr_set_pat(0x0007040600070406ull))
        return false;
    if (axmsr_history_count() == 0)
        return false;
    {
        u32 msr = 0;
        u64 old_value = 0;
        u64 new_value = 0;
        if (!axmsr_get_history(0, &msr, &old_value, &new_value))
            return false;
        if (msr == 0)
            return false;
    }
    if (!axmsr_features())
        return false;
    return true;
}
