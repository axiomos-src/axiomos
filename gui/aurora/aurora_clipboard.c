/* AxiomOS Aurora - clipboard manager.
 *
 * The clipboard keeps a bounded history of entries with type tags, a pin flag
 * and a search index built from a simple substring scan (the history is small
 * enough that an index would cost more than it saves).  Images are stored by
 * handle only; this module never owns pixel buffers.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include "gui.h"

#define AXCB_MAX_ENTRIES     32
#define AXCB_TEXT_LEN        192
#define AXCB_LABEL_LEN       24
#define AXCB_MIME_LEN        16
#define AXCB_PINNED_MAX      8

#define AXCB_KIND_EMPTY       0
#define AXCB_KIND_TEXT        1
#define AXCB_KIND_RICH        2
#define AXCB_KIND_IMAGE       3
#define AXCB_KIND_FILES       4
#define AXCB_KIND_COLOUR      5
#define AXCB_KIND_LINK        6

#define AXCB_COLOUR_BG        GUI_RGB(0x14, 0x16, 0x1C)
#define AXCB_COLOUR_ROW       GUI_RGB(0x1C, 0x20, 0x28)
#define AXCB_COLOUR_ROW_SEL   GUI_RGB(0x1F, 0x2A, 0x44)
#define AXCB_COLOUR_TEXT      GUI_RGB(0xE6, 0xE8, 0xEE)
#define AXCB_COLOUR_MUTED     GUI_RGB(0x8E, 0x95, 0xA4)
#define AXCB_COLOUR_ACCENT    GUI_RGB(0x63, 0x8F, 0xFF)

typedef struct axcb_entry
{
    char text[AXCB_TEXT_LEN];
    char label[AXCB_LABEL_LEN];
    char mime[AXCB_MIME_LEN];
    u32  kind;
    u32  length;
    u32  handle;
    u32  bytes;
    u32  sequence;
    bool_t pinned;
    bool_t used;
} axcb_entry_t;

typedef struct axcb_state
{
    axcb_entry_t entries[AXCB_MAX_ENTRIES];
    u32  count;
    u32  pinned;
    u32  sequence;
    u32  copies;
    u32  pastes;
    u32  evictions;
    u32  search_hits;
    u32  selected;
    u32  filter_kind;
    char query[AXCB_MIME_LEN + 8];
    bool_t panel_open;
    bool_t ready;
} axcb_state_t;

static axcb_state_t axcb_state;

static void axcb_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axcb_copy_string(char *dst, const char *src, u32 limit)
{
    u32 i = 0;
    if (dst == NULL_PTR || limit == 0)
        return;
    while (src != NULL_PTR && src[i] != 0 && i + 1 < limit)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool_t axcb_match(const axcb_entry_t *entry)
{
    u32 i;
    u32 j;
    u32 query_length;
    if (axcb_state.query[0] == 0)
        return true;
    if (axcb_state.filter_kind != 0 && entry->kind != axcb_state.filter_kind)
        return false;
    query_length = 0;
    while (axcb_state.query[query_length] != 0)
        query_length++;
    for (i = 0; i + query_length < entry->length; i++)
    {
        for (j = 0; j < query_length; j++)
        {
            if (entry->text[i + j] != axcb_state.query[j])
                break;
        }
        if (j == query_length)
            return true;
    }
    return false;
}

static u32 axcb_evict(void)
{
    u32 i;
    u32 oldest = 0xFFFFFFFFu;
    u32 sequence = 0xFFFFFFFFu;
    for (i = 0; i < AXCB_MAX_ENTRIES; i++)
    {
        const axcb_entry_t *entry = &axcb_state.entries[i];
        if (!entry->used || entry->pinned)
            continue;
        if (entry->sequence < sequence)
        {
            sequence = entry->sequence;
            oldest = i;
        }
    }
    if (oldest == 0xFFFFFFFFu)
        return oldest;
    axcb_zero(&axcb_state.entries[oldest], (u32)sizeof(axcb_entry_t));
    axcb_state.count--;
    axcb_state.evictions++;
    return oldest;
}

static u32 axcb_alloc_slot(void)
{
    u32 i;
    for (i = 0; i < AXCB_MAX_ENTRIES; i++)
    {
        if (!axcb_state.entries[i].used)
            return i;
    }
    return axcb_evict();
}

/* -------------------------------------------------------------- public API */

void axcb_init(void)
{
    axcb_zero(&axcb_state, (u32)sizeof(axcb_state));
    axcb_state.ready = true;
}

u32 axcb_push(u32 kind, const char *text, const char *label)
{
    u32 slot;
    axcb_entry_t *entry;
    u32 i;
    if (!axcb_state.ready)
        return 0xFFFFFFFFu;
    slot = axcb_alloc_slot();
    if (slot == 0xFFFFFFFFu)
        return slot;
    entry = &axcb_state.entries[slot];
    axcb_zero(entry, (u32)sizeof(*entry));
    entry->kind = kind;
    axcb_copy_string(entry->text, text, AXCB_TEXT_LEN);
    axcb_copy_string(entry->label, label == NULL_PTR ? "clipboard" : label, AXCB_LABEL_LEN);
    axcb_copy_string(entry->mime, kind == AXCB_KIND_TEXT ? "text/plain" : "application/ax",
                     AXCB_MIME_LEN);
    entry->sequence = axcb_state.sequence++;
    entry->used = true;
    for (i = 0; i < AXCB_TEXT_LEN && entry->text[i] != 0; i++)
        ;
    entry->length = i;
    axcb_state.count++;
    axcb_state.copies++;
    axcb_state.selected = slot;
    return slot;
}

u32 axcb_push_bytes(u32 kind, const void *data, u32 size, const char *label)
{
    u32 index;
    if (data == NULL_PTR || size == 0)
        return 0xFFFFFFFFu;
    index = axcb_push(kind, label, label);
    if (index == 0xFFFFFFFFu)
        return index;
    axcb_state.entries[index].bytes = size;
    axcb_state.entries[index].handle = 0x1000 + index;
    axcb_state.entries[index].mime[0] = 'b';
    axcb_state.entries[index].mime[1] = 'i';
    axcb_state.entries[index].mime[2] = 'n';
    axcb_state.entries[index].mime[3] = 0;
    return index;
}

bool_t axcb_pin(u32 index, bool_t pinned)
{
    axcb_entry_t *entry;
    if (index >= AXCB_MAX_ENTRIES)
        return false;
    entry = &axcb_state.entries[index];
    if (!entry->used)
        return false;
    if (pinned && !entry->pinned)
    {
        if (axcb_state.pinned >= AXCB_PINNED_MAX)
            return false;
        axcb_state.pinned++;
    }
    else if (!pinned && entry->pinned)
    {
        if (axcb_state.pinned > 0)
            axcb_state.pinned--;
    }
    entry->pinned = pinned;
    return true;
}

bool_t axcb_remove(u32 index)
{
    axcb_entry_t *entry;
    if (index >= AXCB_MAX_ENTRIES)
        return false;
    entry = &axcb_state.entries[index];
    if (!entry->used)
        return false;
    if (entry->pinned && axcb_state.pinned > 0)
        axcb_state.pinned--;
    axcb_zero(entry, (u32)sizeof(*entry));
    if (axcb_state.count > 0)
        axcb_state.count--;
    return true;
}

u32 axcb_count(void)
{
    return axcb_state.count;
}

const char *axcb_text(u32 index)
{
    if (index >= AXCB_MAX_ENTRIES)
        return NULL_PTR;
    if (!axcb_state.entries[index].used)
        return NULL_PTR;
    return axcb_state.entries[index].text;
}

u32 axcb_kind(u32 index)
{
    if (index >= AXCB_MAX_ENTRIES)
        return AXCB_KIND_EMPTY;
    return axcb_state.entries[index].kind;
}

bool_t axcb_set_query(const char *query)
{
    axcb_copy_string(axcb_state.query, query, (u32)sizeof(axcb_state.query));
    axcb_state.search_hits = 0;
    return true;
}

bool_t axcb_set_filter(u32 kind)
{
    if (kind > AXCB_KIND_LINK)
        return false;
    axcb_state.filter_kind = kind;
    return true;
}

u32 axcb_visible_count(void)
{
    u32 i;
    u32 visible = 0;
    for (i = 0; i < AXCB_MAX_ENTRIES; i++)
    {
        if (!axcb_state.entries[i].used)
            continue;
        if (axcb_match(&axcb_state.entries[i]))
            visible++;
    }
    return visible;
}

u32 axcb_select(u32 index)
{
    if (index >= AXCB_MAX_ENTRIES)
        return axcb_state.selected;
    if (!axcb_state.entries[index].used)
        return axcb_state.selected;
    axcb_state.selected = index;
    axcb_state.pastes++;
    return index;
}

u32 axcb_selected(void)
{
    return axcb_state.selected;
}

bool_t axcb_toggle_panel(void)
{
    axcb_state.panel_open = !axcb_state.panel_open;
    return axcb_state.panel_open;
}

u32 axcb_clear_unpinned(void)
{
    u32 i;
    u32 removed = 0;
    for (i = 0; i < AXCB_MAX_ENTRIES; i++)
    {
        axcb_entry_t *entry = &axcb_state.entries[i];
        if (!entry->used || entry->pinned)
            continue;
        axcb_zero(entry, (u32)sizeof(*entry));
        removed++;
    }
    if (axcb_state.count > removed)
        axcb_state.count -= removed;
    else
        axcb_state.count = 0;
    return removed;
}

u32 axcb_bytes(void)
{
    u32 i;
    u32 total = 0;
    for (i = 0; i < AXCB_MAX_ENTRIES; i++)
    {
        if (!axcb_state.entries[i].used)
            continue;
        total += axcb_state.entries[i].bytes;
        total += axcb_state.entries[i].length;
    }
    return total;
}

/* ----------------------------------------------------------------- painting */

static const char *axcb_kind_name(u32 kind)
{
    switch (kind)
    {
    case AXCB_KIND_TEXT:   return "text";
    case AXCB_KIND_RICH:   return "rich";
    case AXCB_KIND_IMAGE:  return "image";
    case AXCB_KIND_FILES:  return "files";
    case AXCB_KIND_COLOUR: return "colour";
    case AXCB_KIND_LINK:   return "link";
    default:               return "empty";
    }
}

static void axcb_paint_row(gui_surface_t *surface, gui_rect_t row, u32 index)
{
    const axcb_entry_t *entry = &axcb_state.entries[index];
    u32 background = (index == axcb_state.selected) ? AXCB_COLOUR_ROW_SEL : AXCB_COLOUR_ROW;
    gui_fill_round_rect(surface, row, 6, background);
    gui_text(surface, row.x + 10, row.y + 6, entry->label, AXCB_COLOUR_TEXT, 1);
    gui_text(surface, row.x + 10, row.y + 22, entry->text, AXCB_COLOUR_MUTED, 1);
    if (entry->pinned)
    {
        gui_rect_t pin = gui_rect(row.x + row.w - 22, row.y + 8, 12, 12);
        gui_fill_round_rect(surface, pin, 3, AXCB_COLOUR_ACCENT);
    }
    {
        gui_rect_t tag = gui_rect(row.x + row.w - 76, row.y + 8, 44, 16);
        gui_fill_round_rect(surface, tag, 4, GUI_RGB(0x25, 0x2C, 0x3A));
        gui_text(surface, tag.x + 5, tag.y + 1, axcb_kind_name(entry->kind),
                 AXCB_COLOUR_MUTED, 1);
    }
}

void axcb_paint(gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t header;
    gui_rect_t list;
    u32 i;
    u32 shown = 0;
    gui_fill_rect(surface, bounds, AXCB_COLOUR_BG);
    header = gui_rect(bounds.x, bounds.y, bounds.w, 40);
    gui_fill_rect(surface, header, GUI_RGB(0x1A, 0x1E, 0x26));
    gui_text(surface, header.x + 12, header.y + 12, "Clipboard", AXCB_COLOUR_TEXT, 1);
    {
        char summary[24];
        u32 n = axcb_state.count;
        u32 digits = 0;
        char digits_buf[8];
        while (n != 0 && digits < 7)
        {
            digits_buf[digits++] = (char)('0' + (n % 10));
            n /= 10;
        }
        if (digits == 0)
            digits_buf[digits++] = '0';
        for (u32 k = 0; k < digits; k++)
            summary[k] = digits_buf[digits - 1 - k];
        summary[digits] = 0;
        gui_text(surface, header.x + 96, header.y + 12, summary, AXCB_COLOUR_MUTED, 1);
    }
    gui_text(surface, header.x + 128, header.y + 12, "items", AXCB_COLOUR_MUTED, 1);
    if (axcb_state.query[0] != 0)
    {
        gui_text(surface, header.x + 176, header.y + 12, axcb_state.query,
                 AXCB_COLOUR_ACCENT, 1);
    }
    list = gui_rect(bounds.x + 8, bounds.y + 48, bounds.w - 16, bounds.h - 56);
    for (i = 0; i < AXCB_MAX_ENTRIES && shown < 8; i++)
    {
        gui_rect_t row;
        if (!axcb_state.entries[i].used)
            continue;
        if (!axcb_match(&axcb_state.entries[i]))
            continue;
        row = gui_rect(list.x, list.y + (i32)shown * 52, list.w, 46);
        axcb_paint_row(surface, row, i);
        shown++;
    }
    if (shown == 0)
    {
        gui_text(surface, list.x + 12, list.y + 12, "Nothing matches the filter",
                 AXCB_COLOUR_MUTED, 1);
    }
    {
        gui_rect_t footer = gui_rect(bounds.x, bounds.y + bounds.h - 28, bounds.w, 28);
        gui_fill_rect(surface, footer, GUI_RGB(0x18, 0x1C, 0x23));
        gui_text(surface, footer.x + 12, footer.y + 6, "Enter to paste  Del to clear  Space to pin",
                 AXCB_COLOUR_MUTED, 1);
    }
}

bool_t axcb_click(i32 x, i32 y, gui_rect_t bounds)
{
    gui_rect_t list = gui_rect(bounds.x + 8, bounds.y + 48, bounds.w - 16, bounds.h - 56);
    u32 shown = 0;
    u32 i;
    if (!gui_rect_contains(list, x, y))
        return false;
    for (i = 0; i < AXCB_MAX_ENTRIES; i++)
    {
        if (!axcb_state.entries[i].used || !axcb_match(&axcb_state.entries[i]))
            continue;
        {
            gui_rect_t row = gui_rect(list.x, list.y + (i32)shown * 52, list.w, 46);
            if (gui_rect_contains(row, x, y))
            {
                axcb_select(i);
                return true;
            }
        }
        shown++;
    }
    return false;
}

void axcb_report(void)
{
    u32 i;
    printk("axcb: count=%u pinned=%u copies=%u pastes=%u evictions=%u bytes=%u\n",
           (unsigned)axcb_state.count, (unsigned)axcb_state.pinned,
           (unsigned)axcb_state.copies, (unsigned)axcb_state.pastes,
           (unsigned)axcb_state.evictions, (unsigned)axcb_bytes());
    for (i = 0; i < AXCB_MAX_ENTRIES; i++)
    {
        const axcb_entry_t *entry = &axcb_state.entries[i];
        if (!entry->used)
            continue;
        printk("  entry %u kind=%s label=%s len=%u pinned=%u\n", (unsigned)i,
               axcb_kind_name(entry->kind), entry->label, (unsigned)entry->length,
               (unsigned)entry->pinned);
    }
}

bool_t axcb_selftest(void)
{
    u32 first;
    u32 second;
    axcb_init();
    first = axcb_push(AXCB_KIND_TEXT, "hello world", "greeting");
    if (first != 0)
        return false;
    second = axcb_push(AXCB_KIND_LINK, "https://axiomos.local/docs", "link");
    if (second != 1)
        return false;
    if (axcb_count() != 2)
        return false;
    if (!axcb_pin(first, true))
        return false;
    if (axcb_state.pinned != 1)
        return false;
    if (!axcb_set_query("axiom"))
        return false;
    if (axcb_visible_count() != 1)
        return false;
    if (!axcb_set_query(""))
        return false;
    if (axcb_visible_count() != 2)
        return false;
    if (!axcb_set_filter(AXCB_KIND_LINK))
        return false;
    if (axcb_visible_count() != 1)
        return false;
    if (!axcb_set_filter(0))
        return false;
    if (axcb_select(second) != second)
        return false;
    if (axcb_push_bytes(AXCB_KIND_IMAGE, "pixels", 64, "screenshot") != 2)
        return false;
    if (axcb_clear_unpinned() != 2)
        return false;
    if (axcb_count() != 1)
        return false;
    if (!axcb_remove(first))
        return false;
    if (axcb_count() != 0)
        return false;
    if (!axcb_toggle_panel())
        return false;
    return true;
}
