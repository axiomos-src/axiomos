/* AxiomOS - page reclaim.
 *
 * When the free page pool drops under the watermark the allocator asks this
 * module to reclaim.  Pages are classified by how expensive they are to rebuild
 * (clean file cache, dirty file cache, anonymous) and reclaimed in that order.
 * The module keeps its own counters so the sysmon app can show reclaim pressure
 * without walking the page database.
 */

#include <axiom/kernel.h>
#include <axiom/types.h>

#define AXPR_MAX_PAGES     256
#define AXPR_WATERMARK_LOW 32
#define AXPR_WATERMARK_HIGH 96
#define AXPR_BATCH         8
#define AXPR_MAX_SWAP      2048

enum axprc_page_class
{
    AXPR_CLASS_FREE = 0,
    AXPR_CLASS_CLEAN_FILE,
    AXPR_CLASS_DIRTY_FILE,
    AXPR_CLASS_ANON,
    AXPR_CLASS_PINNED
};

enum axprc_ref_kind
{
    AXPR_REF_UNKNOWN = 0,
    AXPR_REF_MAPPED,
    AXPR_REF_CACHE,
    AXPR_REF_KERNEL
};

typedef struct axprc_page
{
    u64  physical;
    u32  class;
    u32  refs;
    u32  age;
    u32  last_used;
    u8   kind;
    bool_t dirty;
    bool_t reclaimable;
    bool_t swapped;
    bool_t pinned;
} axprc_page_t;

typedef struct axprc_swap_slot
{
    u64  physical;
    u32  slot;
    u32  usage;
    bool_t in_use;
} axprc_swap_slot_t;

typedef struct axprc_reclaimer
{
    axprc_page_t pages[AXPR_MAX_PAGES];
    axprc_swap_slot_t swap[AXPR_MAX_SWAP];
    u32  page_count;
    u32  swap_used;
    u32  reclaimed_clean;
    u32  reclaimed_dirty;
    u32  reclaimed_anon;
    u32  reclaimed_pinned_refused;
    u32  scans;
    u32  batches;
    u32  writebacks;
    u32  watermark_low;
    u32  watermark_high;
    u32  last_free;
    bool_t under_pressure;
    bool_t swap_enabled;
} axprc_reclaimer_t;

static axprc_reclaimer_t axprc_reclaimer;

static void axprc_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static axprc_page_t *axprc_find(u64 physical)
{
    u32 i;
    for (i = 0; i < axprc_reclaimer.page_count; i++)
    {
        if (axprc_reclaimer.pages[i].physical == physical)
            return &axprc_reclaimer.pages[i];
    }
    return NULL_PTR;
}

static u32 axprc_pick_victim(u32 *class_out)
{
    u32 i;
    u32 best = 0xFFFFFFFFu;
    u32 best_class = 0xFF;
    u32 best_age = 0;
    for (i = 0; i < axprc_reclaimer.page_count; i++)
    {
        axprc_page_t *page = &axprc_reclaimer.pages[i];
        if (!page->reclaimable || page->pinned || page->class == AXPR_CLASS_FREE)
            continue;
        if (page->class < best_class ||
            (page->class == best_class && (best == 0xFFFFFFFFu || page->age > best_age)))
        {
            best = i;
            best_class = page->class;
            best_age = page->age;
        }
    }
    *class_out = best_class;
    return best;
}

/* -------------------------------------------------------------- public API */

void axprc_init(void)
{
    axprc_zero(&axprc_reclaimer, (u32)sizeof(axprc_reclaimer));
    axprc_reclaimer.watermark_low = AXPR_WATERMARK_LOW;
    axprc_reclaimer.watermark_high = AXPR_WATERMARK_HIGH;
    axprc_reclaimer.swap_enabled = true;
}

u32 axprc_add_page(u64 physical, u32 page_class, bool_t dirty, bool_t pinned)
{
    axprc_page_t *page;
    if (axprc_reclaimer.page_count >= AXPR_MAX_PAGES)
        return 0xFFFFFFFFu;
    page = &axprc_reclaimer.pages[axprc_reclaimer.page_count];
    axprc_zero(page, (u32)sizeof(*page));
    page->physical = physical;
    page->class = page_class;
    page->dirty = dirty;
    page->pinned = pinned;
    page->reclaimable = (bool_t)(!pinned && page_class != AXPR_CLASS_PINNED);
    page->refs = 1;
    page->kind = AXPR_REF_UNKNOWN;
    axprc_reclaimer.page_count++;
    return axprc_reclaimer.page_count - 1;
}

bool_t axprc_touch(u64 physical)
{
    axprc_page_t *page = axprc_find(physical);
    if (page == NULL_PTR)
        return false;
    page->last_used++;
    page->age = 0;
    if (page->refs < 255)
        page->refs++;
    return true;
}

bool_t axprc_release(u64 physical)
{
    axprc_page_t *page = axprc_find(physical);
    if (page == NULL_PTR)
        return false;
    if (page->refs > 0)
        page->refs--;
    return true;
}

u32 axprc_notify_free(u32 free_pages)
{
    axprc_reclaimer.last_free = free_pages;
    axprc_reclaimer.under_pressure =
        (bool_t)(free_pages < axprc_reclaimer.watermark_low);
    return axprc_reclaimer.under_pressure ? 1u : 0u;
}

u32 axprc_reclaim(u32 target)
{
    u32 total = 0;
    u32 class_out = 0;
    u32 index;
    u32 batch = 0;
    axprc_reclaimer.batches++;
    while (total < target && axprc_reclaimer.under_pressure)
    {
        index = axprc_pick_victim(&class_out);
        if (index == 0xFFFFFFFFu)
            break;
        {
            axprc_page_t *page = &axprc_reclaimer.pages[index];
            axprc_reclaimer.scans++;
            if (page->pinned)
            {
                axprc_reclaimer.reclaimed_pinned_refused++;
                page->age++;
                batch++;
                if (batch >= AXPR_BATCH)
                    break;
                continue;
            }
            if (page->class == AXPR_CLASS_CLEAN_FILE)
            {
                page->class = AXPR_CLASS_FREE;
                page->reclaimable = false;
                axprc_reclaimer.reclaimed_clean++;
                total++;
            }
            else if (page->class == AXPR_CLASS_DIRTY_FILE)
            {
                page->dirty = false;
                page->class = AXPR_CLASS_FREE;
                page->reclaimable = false;
                axprc_reclaimer.reclaimed_dirty++;
                axprc_reclaimer.writebacks++;
                total++;
            }
            else if (page->class == AXPR_CLASS_ANON)
            {
                if (axprc_reclaimer.swap_enabled && axprc_reclaimer.swap_used < AXPR_MAX_SWAP)
                {
                    axprc_swap_slot_t *slot =
                        &axprc_reclaimer.swap[axprc_reclaimer.swap_used];
                    slot->physical = page->physical;
                    slot->slot = axprc_reclaimer.swap_used;
                    slot->usage = 1;
                    slot->in_use = true;
                    axprc_reclaimer.swap_used++;
                    page->swapped = true;
                    page->class = AXPR_CLASS_FREE;
                    page->reclaimable = false;
                    axprc_reclaimer.reclaimed_anon++;
                    total++;
                }
            }
            else
            {
                page->age++;
            }
        }
        batch++;
        if (batch >= AXPR_BATCH)
            break;
    }
    axprc_reclaimer.last_free += total;
    if (axprc_reclaimer.last_free >= axprc_reclaimer.watermark_high)
        axprc_reclaimer.under_pressure = false;
    return total;
}

bool_t axprc_swap_in(u64 *physical)
{
    u32 i;
    if (!axprc_reclaimer.swap_enabled)
        return false;
    for (i = 0; i < axprc_reclaimer.swap_used; i++)
    {
        if (axprc_reclaimer.swap[i].in_use)
        {
            *physical = axprc_reclaimer.swap[i].physical;
            axprc_reclaimer.swap[i].in_use = false;
            if (axprc_reclaimer.swap_used > 0)
                axprc_reclaimer.swap_used--;
            return true;
        }
    }
    return false;
}

void axprc_set_swap(bool_t enabled)
{
    axprc_reclaimer.swap_enabled = enabled;
}

void axprc_set_watermarks(u32 low, u32 high)
{
    axprc_reclaimer.watermark_low = low;
    axprc_reclaimer.watermark_high = high;
}

u32 axprc_reclaimed(void)
{
    return axprc_reclaimer.reclaimed_clean + axprc_reclaimer.reclaimed_dirty +
           axprc_reclaimer.reclaimed_anon;
}

u32 axprc_swap_used(void)
{
    return axprc_reclaimer.swap_used;
}

bool_t axprc_pressured(void)
{
    return axprc_reclaimer.under_pressure;
}

void axprc_aging_tick(void)
{
    u32 i;
    for (i = 0; i < axprc_reclaimer.page_count; i++)
    {
        axprc_page_t *page = &axprc_reclaimer.pages[i];
        if (page->class == AXPR_CLASS_FREE)
            continue;
        page->age++;
        if (page->class == AXPR_CLASS_ANON && page->age > 8)
            page->refs = page->refs > 1 ? 1 : page->refs;
    }
}

void axprc_report(void)
{
    printk("axpr: pages=%u free_hint=%u pressure=%u reclaimed=%u (clean=%u dirty=%u anon=%u)\n",
           (unsigned)axprc_reclaimer.page_count, (unsigned)axprc_reclaimer.last_free,
           (unsigned)axprc_reclaimer.under_pressure,
           (unsigned)axprc_reclaimed(), (unsigned)axprc_reclaimer.reclaimed_clean,
           (unsigned)axprc_reclaimer.reclaimed_dirty,
           (unsigned)axprc_reclaimer.reclaimed_anon);
    printk("axpr: swap=%u/%u writebacks=%u batches=%u scans=%u\n",
           (unsigned)axprc_reclaimer.swap_used, (unsigned)AXPR_MAX_SWAP,
           (unsigned)axprc_reclaimer.writebacks, (unsigned)axprc_reclaimer.batches,
           (unsigned)axprc_reclaimer.scans);
}

bool_t axprc_selftest(void)
{
    u64 physical = 0;
    axprc_init();
    if (axprc_add_page(0x1000, AXPR_CLASS_CLEAN_FILE, false, false) != 0)
        return false;
    axprc_add_page(0x2000, AXPR_CLASS_DIRTY_FILE, true, false);
    axprc_add_page(0x3000, AXPR_CLASS_ANON, false, false);
    if (axprc_reclaimer.page_count != 3)
        return false;
    axprc_notify_free(10);
    if (!axprc_pressured())
        return false;
    if (axprc_reclaim(1) != 1)
        return false;
    if (axprc_reclaimer.reclaimed_clean != 1)
        return false;
    axprc_notify_free(10);
    if (axprc_reclaim(2) < 1)
        return false;
    if (axprc_reclaimer.reclaimed_dirty < 1)
        return false;
    axprc_notify_free(10);
    axprc_reclaim(4);
    if (axprc_swap_used() == 0)
    {
        if (axprc_reclaimer.reclaimed_anon != 1)
            return false;
    }
    if (!axprc_swap_in(&physical))
    {
        if (axprc_swap_used() == 0)
            return true;
        return false;
    }
    if (physical == 0)
        return false;
    axprc_touch(0x1000);
    axprc_aging_tick();
    axprc_notify_free(200);
    if (axprc_pressured())
        return false;
    return true;
}
