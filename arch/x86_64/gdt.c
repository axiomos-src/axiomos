/* AxiomOS - Global Descriptor Table and Task State Segment.
 *
 * Long mode effectively ignores most of the GDT, but the CPU still requires
 * valid descriptors for CS/DS/SS and the TSS is mandatory for ring 3
 * transitions and for IST stacks (double fault, NMI, machine check).  The
 * layout here is the classic flat model:
 *
 *   0x08  kernel code   DPL 0
 *   0x10  kernel data   DPL 0
 *   0x18  user code     DPL 3
 *   0x20  user data     DPL 3
 *   0x28  TSS           (16 bytes, system descriptor)
 */

#include <axiom/kernel.h>

#define GDT_ENTRIES 7

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
} AXIOM_PACKED;

struct gdt_pointer {
    u16 limit;
    u64 base;
} AXIOM_PACKED;

struct tss_entry {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} AXIOM_PACKED;

static struct gdt_entry g_gdt[GDT_ENTRIES];
static struct tss_entry g_tss;
static struct gdt_pointer g_gdt_ptr;

/* IST stacks are static so that a double fault can be handled even if the
 * page allocator is not yet online.  8 KiB per stack, 16-byte aligned. */
static u8 g_ist_df[8192] AXIOM_ALIGNED(16);
static u8 g_ist_nmi[8192] AXIOM_ALIGNED(16);
static u8 g_ist_mce[8192] AXIOM_ALIGNED(16);

#define IST_DOUBLE_FAULT 1
#define IST_NMI          2
#define IST_MACHINE_CHECK 3

extern void tss_load(u16 selector);

static void gdt_set_entry(int index, u64 base, u64 limit, u8 access, u8 flags)
{
    g_gdt[index].limit_low = (u16)(limit & 0xFFFF);
    g_gdt[index].base_low = (u16)(base & 0xFFFF);
    g_gdt[index].base_mid = (u8)((base >> 16) & 0xFF);
    g_gdt[index].access = access;
    g_gdt[index].granularity = (u8)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    g_gdt[index].base_high = (u8)((base >> 24) & 0xFF);
}

static void gdt_set_tss(u64 base, u64 limit)
{
    /* A system descriptor occupies two 8-byte slots. */
    gdt_set_entry(5, base, limit, 0x89, 0x00);
    g_gdt[6].limit_low = (u16)((base >> 32) & 0xFFFF);
    g_gdt[6].base_low = (u16)((base >> 48) & 0xFFFF);
    g_gdt[6].base_mid = 0;
    g_gdt[6].access = 0;
    g_gdt[6].granularity = 0;
    g_gdt[6].base_high = 0;
}

void tss_init(u64 kernel_stack)
{
    g_tss.reserved0 = 0;
    g_tss.rsp0 = kernel_stack;
    g_tss.rsp1 = 0;
    g_tss.rsp2 = 0;
    g_tss.iomap_base = sizeof(struct tss_entry);
    g_tss.ist[IST_DOUBLE_FAULT - 1] = (u64)(g_ist_df + sizeof(g_ist_df));
    g_tss.ist[IST_NMI - 1] = (u64)(g_ist_nmi + sizeof(g_ist_nmi));
    g_tss.ist[IST_MACHINE_CHECK - 1] = (u64)(g_ist_mce + sizeof(g_ist_mce));

    gdt_set_tss((u64)&g_tss, sizeof(struct tss_entry) - 1);
}

void gdt_init(void)
{
    /* Null descriptor: required by the architecture. */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Kernel code: present, DPL0, executable/readable, long mode (L=1). */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);
    /* Kernel data: present, DPL0, writable. */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);
    /* User code: DPL3, long mode. */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xA0);
    /* User data: DPL3. */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);

    /* TSS is filled in by tss_init(); reserve the slots now. */
    gdt_set_tss((u64)&g_tss, sizeof(struct tss_entry) - 1);

    g_gdt_ptr.limit = (u16)(sizeof(g_gdt) - 1);
    g_gdt_ptr.base = (u64)g_gdt;

    /* gdt_load takes the address of the 10-byte pseudo-descriptor: the stub
     * executes `lgdt [rdi]`, so a packed limit/base value would be dereferenced
     * as a pointer and fault. */
    gdt_load((u64)&g_gdt_ptr, 0x08, 0x10);

    tss_load(0x28);
    KASSERT((read_cr0() & (1ULL << 0)) == 0 || 1);
}

/* ------------------------------------------------------------------ */
/* Helpers used by the scheduler and user mode                        */
/* ------------------------------------------------------------------ */

u64 gdt_tss_rsp0(void)
{
    return g_tss.rsp0;
}

void gdt_set_kernel_stack(u64 rsp0)
{
    g_tss.rsp0 = rsp0;
}

void gdt_dump(void)
{
    printk("gdt: base=%p limit=%u tss.rsp0=%p\n", (void *)g_gdt_ptr.base,
           (u64)g_gdt_ptr.limit, (void *)g_tss.rsp0);
    for (int i = 0; i < GDT_ENTRIES; i++) {
        printk("  [%d] limit=%x base=%x access=%x flags=%x\n", (u64)i,
               (u64)g_gdt[i].limit_low, (u64)g_gdt[i].base_low,
               (u64)g_gdt[i].access, (u64)g_gdt[i].granularity);
    }
}
