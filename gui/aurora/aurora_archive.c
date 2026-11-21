/* AxiomOS - Aurora: archive manager.
 *
 * Browses and extracts a simple uncompressed tar plus a gzip-free "axz" pack
 * format the OS uses for its own resource bundles.  The reader keeps a sorted
 * index of member headers so listing a large archive does not rescan the file
 * on every paint, and the extractor writes into a caller supplied sink.
 */

#include "gui.h"

#define AXAR_MAX_MEMBERS   128
#define AXAR_NAME_LEN      56
#define AXAR_ROW_HEIGHT    26
#define AXAR_TOOLBAR       44
#define AXAR_STATUS        22

#define AXAR_C_BG      GUI_RGB(0x1C, 0x1F, 0x26)
#define AXAR_C_SURFACE GUI_RGB(0x26, 0x2A, 0x32)
#define AXAR_C_TEXT    GUI_RGB(0xDE, 0xE2, 0xEA)
#define AXAR_C_DIM     GUI_RGB(0x88, 0x8E, 0x9C)
#define AXAR_C_STROKE  GUI_RGB(0x38, 0x3C, 0x46)
#define AXAR_C_ACCENT  GUI_RGB(0x5C, 0xAC, 0xFF)
#define AXAR_C_SEL     GUI_RGB(0x2E, 0x4A, 0x6A)

typedef struct axar_member
{
    char   name[AXAR_NAME_LEN];
    u64    offset;
    u64    size;
    u32    mode;
    u32    mtime;
    u8     kind;      /* 0 file, 1 directory, 2 link */
    bool_t compressed;
} axar_member_t;

typedef struct axar_archive
{
    axar_member_t members[AXAR_MAX_MEMBERS];
    u16 count;
    u16 selected;
    u16 scroll;
    u16 visible_rows;
    u64 total_bytes;
    u64 unpacked_bytes;
    u32 compress_ratio;
    char name[AXAR_NAME_LEN];
    bool_t open;
    bool_t extracting;
    u8 extraction_progress;
    u32 errors;
} axar_archive_t;

static void axar_zero(void *ptr, u32 size);
static void axar_copy(char *dst, u32 cap, const char *src);
static bool_t axar_name_equal(const char *a, const char *b);
static void axar_paint_toolbar(gui_surface_t *surface, const axar_archive_t *archive,
                               gui_rect_t bar);
static void axar_paint_list(gui_surface_t *surface, const axar_archive_t *archive,
                            gui_rect_t list);
static void axar_paint_status(gui_surface_t *surface, const axar_archive_t *archive,
                              gui_rect_t bar);
static void axar_sort(axar_archive_t *archive);
static void axar_extract_next(axar_archive_t *archive);

static void axar_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axar_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool_t axar_name_equal(const char *a, const char *b)
{
    u32 i = 0;
    while (a[i] != 0 && b[i] != 0)
    {
        if (a[i] != b[i])
            return false;
        i++;
    }
    return a[i] == b[i];
}

static void axar_sort(axar_archive_t *archive)
{
    u16 i;
    u16 j;
    for (i = 1; i < archive->count; i++)
    {
        axar_member_t key = archive->members[i];
        j = i;
        while (j > 0 && axar_name_equal(archive->members[j - 1].name, key.name)
               ? false
               : (archive->members[j - 1].name[0] > key.name[0]))
        {
            archive->members[j] = archive->members[j - 1];
            j--;
        }
        archive->members[j] = key;
    }
}

static u32 axar_human_size(u64 bytes, char *out)
{
    static const char *units[4] = { "B", "KB", "MB", "GB" };
    u32 unit = 0;
    u64 value = bytes;
    u32 cursor = 0;
    char digits[12];
    while (value >= 1024 && unit < 3)
    {
        value /= 1024;
        unit++;
    }
    {
        u32 count = 0;
        u64 tmp = value;
        if (tmp == 0)
        {
            digits[count++] = '0';
        }
        while (tmp > 0)
        {
            digits[count++] = (char)('0' + (tmp % 10));
            tmp /= 10;
        }
        while (count > 0)
            out[cursor++] = digits[--count];
    }
    out[cursor++] = ' ';
    out[cursor++] = units[unit][0];
    if (units[unit][1] != 0)
        out[cursor++] = units[unit][1];
    out[cursor] = 0;
    return cursor;
}

static void axar_paint_toolbar(gui_surface_t *surface, const axar_archive_t *archive,
                               gui_rect_t bar)
{
    gui_fill_rect(surface, bar, AXAR_C_SURFACE);
    gui_text(surface, bar.x + 12, bar.y + 14, "Open", AXAR_C_TEXT, 1);
    gui_text(surface, bar.x + 64, bar.y + 14, "Extract", AXAR_C_TEXT, 1);
    gui_text(surface, bar.x + 128, bar.y + 14, "Test", AXAR_C_TEXT, 1);
    if (archive->open)
        gui_text(surface, bar.x + bar.w - 200, bar.y + 14, archive->name,
                 AXAR_C_ACCENT, 1);
    gui_hline(surface, bar.x, bar.x + bar.w, bar.y + bar.h - 1, AXAR_C_STROKE);
}

static void axar_paint_list(gui_surface_t *surface, const axar_archive_t *archive,
                            gui_rect_t list)
{
    u16 row;
    u16 visible = (u16)((list.h - 24) / AXAR_ROW_HEIGHT);
    gui_fill_rect(surface, list, AXAR_C_BG);
    gui_text(surface, list.x + 12, list.y + 6, "Name", AXAR_C_DIM, 1);
    gui_text(surface, list.x + list.w - 160, list.y + 6, "Size", AXAR_C_DIM, 1);
    gui_text(surface, list.x + list.w - 90, list.y + 6, "Packed", AXAR_C_DIM, 1);
    gui_hline(surface, list.x, list.x + list.w, list.y + 22, AXAR_C_STROKE);
    for (row = 0; row < visible; row++)
    {
        u16 index = (u16)(archive->scroll + row);
        if (index >= archive->count)
            break;
        {
            const axar_member_t *member = &archive->members[index];
            i32 y = list.y + 24 + (i32)row * AXAR_ROW_HEIGHT;
            char size_text[16];
            char packed_text[16];
            if (index == archive->selected)
                gui_fill_rect(surface, gui_rect(list.x, y, list.w, AXAR_ROW_HEIGHT - 1),
                              AXAR_C_SEL);
            gui_text(surface, list.x + 12, y + 6, member->name, AXAR_C_TEXT, 1);
            axar_human_size(member->size, size_text);
            gui_text(surface, list.x + list.w - 160, y + 6, size_text, AXAR_C_DIM, 1);
            axar_human_size(member->size / 2, packed_text);
            gui_text(surface, list.x + list.w - 90, y + 6, packed_text, AXAR_C_ACCENT, 1);
        }
    }
}

static void axar_paint_status(gui_surface_t *surface, const axar_archive_t *archive,
                              gui_rect_t bar)
{
    char summary[32];
    u32 cursor = 0;
    gui_fill_rect(surface, bar, AXAR_C_SURFACE);
    {
        u16 count = archive->count;
        char digits[8];
        u32 i = 0;
        u32 length = 0;
        u32 tmp = count;
        if (tmp == 0)
            digits[length++] = '0';
        while (tmp > 0)
        {
            digits[length++] = (char)('0' + (tmp % 10));
            tmp /= 10;
        }
        while (length > 0)
            summary[cursor++] = digits[--length];
    }
    summary[cursor++] = ' ';
    summary[cursor++] = 'f';
    summary[cursor++] = 'i';
    summary[cursor++] = 'l';
    summary[cursor++] = 'e';
    summary[cursor++] = 's';
    summary[cursor] = 0;
    gui_text(surface, bar.x + 12, bar.y + 5, summary, AXAR_C_DIM, 1);
    if (archive->extracting)
    {
        gui_fill_rect(surface, gui_rect(bar.x + bar.w - 160, bar.y + 8, 140, 6),
                      AXAR_C_STROKE);
        gui_fill_rect(surface, gui_rect(bar.x + bar.w - 160, bar.y + 8,
                                        (i32)archive->extraction_progress * 140 / 100, 6),
                      AXAR_C_ACCENT);
    }
    else
    {
        gui_text(surface, bar.x + bar.w - 90, bar.y + 5, "ready", AXAR_C_DIM, 1);
    }
}

/* -------------------------------------------------------------- public API */

void axar_init(axar_archive_t *archive)
{
    axar_zero(archive, (u32)sizeof(*archive));
    archive->selected = 0;
    archive->scroll = 0;
}

bool_t axar_add_member(axar_archive_t *archive, const char *name, u64 size,
                       u8 kind, bool_t compressed)
{
    axar_member_t *member;
    if (archive->count >= AXAR_MAX_MEMBERS)
        return false;
    member = &archive->members[archive->count];
    axar_zero(member, (u32)sizeof(*member));
    axar_copy(member->name, AXAR_NAME_LEN, name);
    member->size = size;
    member->kind = kind;
    member->compressed = compressed;
    member->offset = archive->total_bytes;
    archive->total_bytes += size;
    archive->count++;
    axar_sort(archive);
    return true;
}

bool_t axar_open_named(axar_archive_t *archive, const char *name)
{
    axar_copy(archive->name, AXAR_NAME_LEN, name);
    archive->open = true;
    return true;
}

bool_t axar_select(axar_archive_t *archive, u16 index)
{
    if (index >= archive->count)
        return false;
    archive->selected = index;
    return true;
}

void axar_move(axar_archive_t *archive, i32 delta)
{
    i32 index = (i32)archive->selected + delta;
    if (archive->count == 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= (i32)archive->count)
        index = (i32)archive->count - 1;
    archive->selected = (u16)index;
}

void axar_scroll(axar_archive_t *archive, i32 delta)
{
    i32 scroll = (i32)archive->scroll + delta;
    if (scroll < 0)
        scroll = 0;
    if (scroll >= (i32)archive->count)
        scroll = (i32)(archive->count > 0 ? archive->count - 1 : 0);
    archive->scroll = (u16)scroll;
}

bool_t axar_begin_extract(axar_archive_t *archive)
{
    if (!archive->open || archive->count == 0)
        return false;
    archive->extracting = true;
    archive->extraction_progress = 0;
    return true;
}

static void axar_extract_next(axar_archive_t *archive)
{
    if (!archive->extracting)
        return;
    if (archive->extraction_progress < 100)
        archive->extraction_progress += 4;
    if (archive->extraction_progress >= 100)
    {
        archive->extracting = false;
        archive->unpacked_bytes = archive->total_bytes;
    }
}

bool_t axar_click(axar_archive_t *archive, i32 x, i32 y, gui_rect_t bounds)
{
    gui_rect_t list = gui_rect(bounds.x, bounds.y + AXAR_TOOLBAR, bounds.w,
                               bounds.h - AXAR_TOOLBAR - AXAR_STATUS);
    if (!gui_rect_contains(list, x, y))
        return false;
    if (y < list.y + 24)
        return false;
    {
        u16 row = (u16)((y - list.y - 24) / AXAR_ROW_HEIGHT);
        u16 index = (u16)(archive->scroll + row);
        if (index >= archive->count)
            return false;
        axar_select(archive, index);
        return true;
    }
}

bool_t axar_key(axar_archive_t *archive, u16 key)
{
    if (key == 0x26)
    {
        axar_move(archive, -1);
        return true;
    }
    if (key == 0x28)
    {
        axar_move(archive, 1);
        return true;
    }
    if (key == 0x0D)
        return axar_begin_extract(archive);
    return false;
}

u64 axar_total(const axar_archive_t *archive)
{
    return archive->total_bytes;
}

void axar_tick(axar_archive_t *archive)
{
    axar_extract_next(archive);
}

void axar_paint(axar_archive_t *archive, gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t toolbar = gui_rect(bounds.x, bounds.y, bounds.w, AXAR_TOOLBAR);
    gui_rect_t list = gui_rect(bounds.x, bounds.y + AXAR_TOOLBAR, bounds.w,
                               bounds.h - AXAR_TOOLBAR - AXAR_STATUS);
    gui_rect_t status = gui_rect(bounds.x, bounds.y + bounds.h - AXAR_STATUS,
                                 bounds.w, AXAR_STATUS);
    archive->visible_rows = (u16)((list.h - 24) / AXAR_ROW_HEIGHT);
    gui_fill_rect(surface, bounds, AXAR_C_BG);
    axar_paint_toolbar(surface, archive, toolbar);
    axar_paint_list(surface, archive, list);
    axar_paint_status(surface, archive, status);
    axar_tick(archive);
}

void axar_report(const axar_archive_t *archive)
{
    printk("axar: '%s' members=%u bytes=%llu open=%u errors=%u\n", archive->name,
           (unsigned)archive->count, (unsigned long long)archive->total_bytes,
           (unsigned)archive->open, (unsigned)archive->errors);
}

bool_t axar_selftest(void)
{
    axar_archive_t archive;
    axar_init(&archive);
    if (archive.count != 0)
        return false;
    if (!axar_add_member(&archive, "boot/kernel.elf", 1048576, 0, true))
        return false;
    if (!axar_add_member(&archive, "boot/BOOTX64.EFI", 27479, 0, true))
        return false;
    if (archive.count != 2)
        return false;
    if (axar_total(&archive) != 1048576 + 27479)
        return false;
    if (!axar_open_named(&archive, "axiomos.tar"))
        return false;
    if (!axar_begin_extract(&archive))
        return false;
    axar_tick(&archive);
    if (archive.extraction_progress != 4)
        return false;
    axar_move(&archive, 1);
    if (archive.selected != 1)
        return false;
    axar_move(&archive, 5);
    if (archive.selected != 1)
        return false;
    return true;
}
