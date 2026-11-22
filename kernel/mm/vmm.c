/* AxiomOS - virtual memory manager (4-level paging).
 *
 * Boot strategy: firmware's page tables are replaced by a kernel-authored
 * hierarchy that identity-maps the first 4 GiB with 2 MiB pages.  Identity
 * mapping means the loader's physical placement of the kernel (1 MiB) remains
 * valid after CR3 is switched, and drivers can poke at MMIO below 4 GiB
 * without extra bookkeeping.
 *
 * Dynamic mappings allocate page tables from the physical allocator.  Devices
 * whose BARs live above 4 GiB are mapped through the MMIO window at
 * 0xFFFFFF0000000000 so the kernel never needs to extend the identity map.
 *
 * Phase 1 limitation (documented in docs/mm.md): a user address space shares
 * the kernel half of the PML4, so user pages must be placed above 4 GiB or in
 * a dedicated region carved out of the identity map.
 */

#include <axiom/kernel.h>

#define PML4_ENTRIES 512
#define PDPT_ENTRIES 512
#define PD_ENTRIES   512
#define PT_ENTRIES   512

#define MMIO_WINDOW_BASE 0xFFFFFF0000000000ULL
#define MMIO_WINDOW_END  0xFFFFFF8000000000ULL

/* Bootstrap tables: static so that paging can be enabled before the physical
 * allocator is initialised.  4 GiB identity map = 1 PML4 + 1 PDPT + 4 PD. */
static u64 g_boot_pml4[PML4_ENTRIES] AXIOM_ALIGNED(4096);
static u64 g_boot_pdpt[PDPT_ENTRIES] AXIOM_ALIGNED(4096);
static u64 g_boot_pd[4][PD_ENTRIES] AXIOM_ALIGNED(4096);

static u64 g_kernel_pml4;
static u64 g_mmio_next = MMIO_WINDOW_BASE + 0x100000;
static u64 g_mapped_pages;
static u64 g_split_pages;
static bool_t g_vmm_ready;

/* ------------------------------------------------------------------ */
/* Table walking                                                      */
/* ------------------------------------------------------------------ */

AXIOM_INLINE u64 *table_from_entry(u64 entry)
{
    return (u64 *)(entry & PTE_ADDR_MASK);
}

AXIOM_INLINE u64 make_entry(u64 phys, u64 flags)
{
    return (phys & PTE_ADDR_MASK) | flags;
}

static u64 *vmm_next_table(u64 *table, u64 index, bool_t create)
{
    u64 entry = table[index];
    if (entry & PTE_PRESENT)
        return table_from_entry(entry);
    if (!create)
        return NULL_PTR;

    u64 phys = pmm_alloc_page();
    if (!phys)
        return NULL_PTR;
    table[index] = make_entry(phys, PTE_PRESENT | PTE_WRITABLE);
    return (u64 *)phys;
}

/* Split a 2 MiB page into 512 4 KiB pages so a smaller mapping can be placed
 * inside it.  Returns the new page table, or NULL on failure. */
static u64 *vmm_split_huge(u64 *pd, u64 pd_index)
{
    u64 entry = pd[pd_index];
    u64 phys_base = entry & PTE_ADDR_MASK;
    u64 *pt;
    u64 flags;

    if (!(entry & PTE_HUGE))
        return table_from_entry(entry);

    pt = (u64 *)pmm_alloc_page();
    if (!pt)
        return NULL_PTR;

    flags = PTE_PRESENT | PTE_WRITABLE;
    if (entry & PTE_USER)
        flags |= PTE_USER;
    if (entry & PTE_NX)
        flags |= PTE_NX;

    for (u64 i = 0; i < PT_ENTRIES; i++)
        pt[i] = make_entry(phys_base + i * AXIOM_PAGE_SIZE, flags);

    pd[pd_index] = make_entry((u64)pt, PTE_PRESENT | PTE_WRITABLE |
                              (entry & PTE_USER ? PTE_USER : 0));
    g_split_pages++;
    invlpg(phys_base);
    return pt;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */

void vmm_init(void)
{
    /* Enable NX and global pages before installing our tables. */
    {
        u64 efer = read_msr(0xC0000080);        /* IA32_EFER */
        write_msr(0xC0000080, efer | (1ULL << 11));   /* NXE */
        write_cr4(read_cr4() | (1ULL << 7));    /* PGE */
    }

    for (u32 i = 0; i < PML4_ENTRIES; i++)
        g_boot_pml4[i] = 0;
    for (u32 i = 0; i < PDPT_ENTRIES; i++)
        g_boot_pdpt[i] = 0;

    for (u32 gb = 0; gb < 4; gb++) {
        g_boot_pdpt[gb] = make_entry((u64)g_boot_pd[gb], PTE_PRESENT | PTE_WRITABLE);
        for (u32 p = 0; p < PD_ENTRIES; p++) {
            u64 phys = (u64)gb * 0x40000000ULL + (u64)p * AXIOM_HUGE_SIZE;
            g_boot_pd[gb][p] = make_entry(phys, PTE_PRESENT | PTE_WRITABLE |
                                          PTE_HUGE | PTE_GLOBAL);
        }
    }
    g_boot_pml4[0] = make_entry((u64)g_boot_pdpt, PTE_PRESENT | PTE_WRITABLE);

    g_kernel_pml4 = (u64)g_boot_pml4;
    write_cr3(g_kernel_pml4);
    invlpg(0);

    g_vmm_ready = true;
    g_mapped_pages = 0;
    printk("vmm: identity map 0..4 GiB with 2 MiB pages, pml4 at %p\n",
           (void *)g_kernel_pml4);
}

u64 vmm_kernel_pml4(void)
{
    return g_kernel_pml4;
}

void vmm_switch(u64 pml4)
{
    if (pml4)
        write_cr3(pml4);
}

/* ------------------------------------------------------------------ */
/* Mapping                                                            */
/* ------------------------------------------------------------------ */

bool_t vmm_map(u64 virt, u64 phys, u64 flags)
{
    u64 *pml4 = (u64 *)g_kernel_pml4;
    u64 *pdpt, *pd, *pt;
    u64 i4 = (virt >> 39) & 0x1FF;
    u64 i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF;
    u64 i1 = (virt >> 12) & 0x1FF;

    if (!g_vmm_ready || (virt & 0xFFF) || (phys & 0xFFF))
        return false;

    pdpt = vmm_next_table(pml4, i4, true);
    if (!pdpt)
        return false;
    pd = vmm_next_table(pdpt, i3, true);
    if (!pd)
        return false;

    if (pd[i2] & PTE_PRESENT) {
        if (pd[i2] & PTE_HUGE) {
            pt = vmm_split_huge(pd, i2);
            if (!pt)
                return false;
        } else {
            pt = table_from_entry(pd[i2]);
        }
    } else {
        pt = (u64 *)pmm_alloc_page();
        if (!pt)
            return false;
        pd[i2] = make_entry((u64)pt, PTE_PRESENT | PTE_WRITABLE);
    }

    pt[i1] = make_entry(phys, flags | PTE_PRESENT);
    invlpg(virt);
    g_mapped_pages++;
    return true;
}

bool_t vmm_unmap(u64 virt)
{
    u64 *pml4 = (u64 *)g_kernel_pml4;
    u64 i4 = (virt >> 39) & 0x1FF, i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF, i1 = (virt >> 12) & 0x1FF;
    u64 *pdpt, *pd, *pt;

    if (!(pml4[i4] & PTE_PRESENT))
        return false;
    pdpt = table_from_entry(pml4[i4]);
    if (!(pdpt[i3] & PTE_PRESENT))
        return false;
    pd = table_from_entry(pdpt[i3]);
    if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE))
        return false;
    pt = table_from_entry(pd[i2]);
    pt[i1] = 0;
    invlpg(virt);
    return true;
}

bool_t vmm_map_range(u64 virt, u64 phys, u64 size, u64 flags)
{
    for (u64 off = 0; off < size; off += AXIOM_PAGE_SIZE) {
        if (!vmm_map(virt + off, phys + off, flags))
            return false;
    }
    return true;
}

u64 vmm_map_mmio(u64 phys, u64 size)
{
    u64 aligned_size = AXIOM_PAGE_ALIGN(size);
    u64 virt = g_mmio_next;
    if (virt + aligned_size >= MMIO_WINDOW_END) {
        printk("vmm: MMIO window exhausted\n");
        return 0;
    }
    for (u64 off = 0; off < aligned_size; off += AXIOM_PAGE_SIZE) {
        if (!vmm_map(virt + off, phys + off,
                     PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT |
                     PTE_NO_EXEC))
            return 0;
    }
    g_mmio_next += aligned_size;
    return virt;
}

u64 vmm_translate(u64 virt)
{
    u64 *pml4 = (u64 *)g_kernel_pml4;
    u64 i4 = (virt >> 39) & 0x1FF, i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF, i1 = (virt >> 12) & 0x1FF;
    u64 *pdpt, *pd, *pt;

    if (!(pml4[i4] & PTE_PRESENT))
        return 0;
    pdpt = table_from_entry(pml4[i4]);
    if (!(pdpt[i3] & PTE_PRESENT))
        return 0;
    pd = table_from_entry(pdpt[i3]);
    if (!(pd[i2] & PTE_PRESENT))
        return 0;
    if (pd[i2] & PTE_HUGE)
        return (pd[i2] & PTE_ADDR_MASK) + (virt & (AXIOM_HUGE_SIZE - 1));
    pt = table_from_entry(pd[i2]);
    if (!(pt[i1] & PTE_PRESENT))
        return 0;
    return (pt[i1] & PTE_ADDR_MASK) + (virt & 0xFFF);
}

void vmm_protect(u64 virt, u64 size, u64 flags)
{
    for (u64 off = 0; off < size; off += AXIOM_PAGE_SIZE) {
        u64 pa = vmm_translate(virt + off);
        if (!pa)
            continue;
        u64 page = (virt + off) & AXIOM_PAGE_MASK;
        vmm_map(page, pa & AXIOM_PAGE_MASK, flags);
    }
}

/* ------------------------------------------------------------------ */
/* Address spaces                                                     */
/* ------------------------------------------------------------------ */

u64 vmm_new_address_space(void)
{
    u64 phys = pmm_alloc_page();
    if (!phys)
        return 0;
    u64 *pml4 = (u64 *)phys;
    u64 *kernel = (u64 *)g_kernel_pml4;

    /* Share the kernel's mappings so that interrupts, the heap and driver
     * code stay reachable from every address space. */
    for (u32 i = 0; i < PML4_ENTRIES; i++)
        pml4[i] = kernel[i];
    return phys;
}

void vmm_destroy_address_space(u64 pml4)
{
    if (!pml4 || pml4 == g_kernel_pml4)
        return;
    pmm_free_page(pml4);
}

void vmm_dump_stats(void)
{
    printk("vmm: pml4=%p mapped=%u splits=%u mmio_next=%p\n",
           (void *)g_kernel_pml4, g_mapped_pages, g_split_pages,
           (void *)g_mmio_next);
}

/* ------------------------------------------------------------------ */
/* Kernel image protections                                           */
/* ------------------------------------------------------------------ */

extern u8 __text_end[];
extern u8 __rodata_start[];

/* Mark the kernel's text as read-only and non-writable, leaving NX clear so it
 * stays executable.  HVCI calls this after taking its integrity manifest. */
void vmm_protect_kernel_text(void)
{
    u64 start = (u64)__text_end - ((u64)__text_end - 0x100000);
    AXIOM_UNUSED_VAR(start);
    u64 end = (u64)__text_end;
    for (u64 addr = 0x100000; addr < end; addr += AXIOM_PAGE_SIZE) {
        u64 pa = vmm_translate(addr);
        if (pa)
            vmm_map(addr, pa & AXIOM_PAGE_MASK, PTE_PRESENT | PTE_GLOBAL);
    }
    printk("vmm: kernel text protected read-only (%p..%p)\n",
           (void *)0x100000ULL, (void *)end);
}
