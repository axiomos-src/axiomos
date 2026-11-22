/* AxiomOS - physical memory manager.
 *
 * A single bitmap describes every 4 KiB frame below 4 GiB (the phase 1 limit;
 * the loader normalises the firmware map so the kernel only sees six region
 * types).  The bitmap is 128 KiB, allocated statically in .bss, which means
 * the allocator works before the heap exists.
 *
 * Regions marked usable by firmware are freed first, then reservations are
 * applied for the kernel image, the boot information block, the framebuffer
 * and any driver MMIO hole.  Overlapping marks in the boot map are resolved by
 * applying reservations after frees, so a reservation always wins.
 */

#include <axiom/kernel.h>

#define PMM_MAX_PHYS      (4ULL * 1024 * 1024 * 1024)
#define PMM_FRAME_SIZE    AXIOM_PAGE_SIZE
#define PMM_FRAME_COUNT   (PMM_MAX_PHYS / PMM_FRAME_SIZE)   /* 1 048 576 */
#define PMM_BITMAP_WORDS  (PMM_FRAME_COUNT / 64)            /* 16 384     */

static u64 g_bitmap[PMM_BITMAP_WORDS] AXIOM_ALIGNED(4096);
static u64 g_total_frames;
static u64 g_used_frames;
static u64 g_reserved_frames;
static u64 g_alloc_hint;
static bool_t g_pmm_ready;

/* ------------------------------------------------------------------ */
/* Bit helpers                                                        */
/* ------------------------------------------------------------------ */

AXIOM_INLINE bool_t frame_is_free(u64 frame)
{
    return (g_bitmap[frame / 64] & (1ULL << (frame % 64))) != 0;
}

AXIOM_INLINE void frame_set_free(u64 frame)
{
    g_bitmap[frame / 64] |= (1ULL << (frame % 64));
}

AXIOM_INLINE void frame_set_used(u64 frame)
{
    g_bitmap[frame / 64] &= ~(1ULL << (frame % 64));
}

static void frame_mark_range(u64 base, u64 length, bool_t free)
{
    u64 first = base / PMM_FRAME_SIZE;
    u64 last = (base + length + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    if (first >= PMM_FRAME_COUNT)
        return;
    if (last > PMM_FRAME_COUNT)
        last = PMM_FRAME_COUNT;

    for (u64 f = first; f < last; f++) {
        if (free) {
            if (!frame_is_free(f)) {
                frame_set_free(f);
                g_total_frames++;
            }
        } else {
            if (frame_is_free(f)) {
                g_used_frames++;
                frame_set_used(f);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */

void pmm_reserve(u64 base, u64 length)
{
    frame_mark_range(base, length, false);
}

void pmm_init(void)
{
    struct abp_boot_info *bi = g_boot_info;

    for (u64 i = 0; i < PMM_BITMAP_WORDS; i++)
        g_bitmap[i] = 0;
    g_total_frames = 0;
    g_used_frames = 0;
    g_reserved_frames = 0;
    g_alloc_hint = 1;

    KASSERT(bi && bi->magic == ABP_MAGIC);

    /* Pass 1: free everything firmware calls usable. */
    struct abp_memory_entry *entries = (struct abp_memory_entry *)bi->memmap_entries;
    for (u32 i = 0; i < bi->memmap_entry_count; i++) {
        if (entries[i].type == ABP_MEM_USABLE)
            frame_mark_range(entries[i].base, entries[i].length, true);
    }

    /* Pass 2: reservations. */
    for (u32 i = 0; i < bi->memmap_entry_count; i++) {
        if (entries[i].type != ABP_MEM_USABLE)
            frame_mark_range(entries[i].base, entries[i].length, false);
    }

    /* The low 1 MiB holds real-mode structures, the EBDA and the IVT, and is
     * never handed out even if firmware claims it is usable. */
    for (u64 f = 0; f < (0x100000 / PMM_FRAME_SIZE); f++) {
        if (frame_is_free(f)) {
            frame_set_used(f);
            g_used_frames++;
        }
    }

    /* The bitmap itself lives in .bss (already reserved with the kernel). */
    g_pmm_ready = true;
    printk("pmm: %u MiB usable, %u MiB used, %u frames reserved\n",
           pmm_usable_bytes() / (1024 * 1024), pmm_used_bytes() / (1024 * 1024),
           g_reserved_frames);
}

/* ------------------------------------------------------------------ */
/* Allocation                                                         */
/* ------------------------------------------------------------------ */

static u64 find_free_frame(u64 start, u64 count)
{
    u64 scanned = 0;
    u64 frame = start;
    while (scanned < PMM_FRAME_COUNT) {
        /* Skip full words quickly. */
        if ((frame % 64) == 0 && g_bitmap[frame / 64] == 0) {
            frame += 64;
            scanned += 64;
            if (frame >= PMM_FRAME_COUNT)
                frame = 1;
            continue;
        }
        if (frame_is_free(frame)) {
            /* Check that the whole run is free before committing. */
            u64 run = 0;
            while (run < count && frame + run < PMM_FRAME_COUNT &&
                   frame_is_free(frame + run))
                run++;
            if (run == count)
                return frame;
            frame += run;
            scanned += run;
        } else {
            frame++;
            scanned++;
        }
        if (frame >= PMM_FRAME_COUNT)
            frame = 1;
    }
    return 0;
}

u64 pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

u64 pmm_alloc_pages(u64 count)
{
    if (!g_pmm_ready || count == 0)
        return 0;

    u64 frame = find_free_frame(g_alloc_hint, count);
    if (!frame) {
        g_alloc_hint = 1;
        frame = find_free_frame(1, count);
    }
    if (!frame)
        return 0;

    for (u64 i = 0; i < count; i++) {
        frame_set_used(frame + i);
        g_used_frames++;
    }
    g_alloc_hint = frame + count;
    if (g_alloc_hint >= PMM_FRAME_COUNT)
        g_alloc_hint = 1;

    u64 addr = frame * PMM_FRAME_SIZE;
    /* Hand out zeroed memory: every caller relies on this. */
    memset((void *)addr, 0, count * PMM_FRAME_SIZE);
    return addr;
}

void pmm_free_page(u64 addr)
{
    pmm_free_pages(addr, 1);
}

void pmm_free_pages(u64 addr, u64 count)
{
    u64 frame = addr / PMM_FRAME_SIZE;
    if (!addr || (addr % PMM_FRAME_SIZE) || !g_pmm_ready)
        return;
    for (u64 i = 0; i < count && frame + i < PMM_FRAME_COUNT; i++) {
        if (!frame_is_free(frame + i)) {
            frame_set_free(frame + i);
            if (g_used_frames)
                g_used_frames--;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Accounting                                                         */
/* ------------------------------------------------------------------ */

u64 pmm_total_bytes(void)
{
    return g_total_frames * PMM_FRAME_SIZE;
}

u64 pmm_usable_bytes(void)
{
    return g_total_frames * PMM_FRAME_SIZE;
}

u64 pmm_used_bytes(void)
{
    return g_used_frames * PMM_FRAME_SIZE;
}

u64 pmm_free_bytes(void)
{
    u64 free = (g_total_frames > g_used_frames) ? g_total_frames - g_used_frames : 0;
    return free * PMM_FRAME_SIZE;
}

bool_t pmm_is_usable(u64 addr)
{
    u64 frame = addr / PMM_FRAME_SIZE;
    if (frame >= PMM_FRAME_COUNT)
        return false;
    return frame_is_free(frame);
}

void pmm_dump(void)
{
    printk("pmm: %u frames total, %u used, %u free (%u KiB)\n",
           g_total_frames, g_used_frames,
           g_total_frames > g_used_frames ? g_total_frames - g_used_frames : 0,
           pmm_free_bytes() / 1024);

    /* Print a coarse occupancy map: 64 columns, each 1/64 of physical RAM. */
    {
        char line[80];
        u32 cols = 64;
        u64 step = PMM_FRAME_COUNT / cols;
        for (u32 c = 0; c < cols; c++) {
            u64 free_here = 0;
            for (u64 f = c * step; f < (c + 1) * step; f += step / 8 + 1) {
                if (frame_is_free(f))
                    free_here++;
            }
            line[c] = free_here > 4 ? ' ' : '#';
        }
        line[cols] = 0;
        printk("pmm: [%s]\n", line);
    }
}
