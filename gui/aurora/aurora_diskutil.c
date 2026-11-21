/* AxiomOS Aurora - disk utility.
 *
 * Shows the block devices the kernel discovered, their partition maps and the
 * SMART style health counters that the storage drivers expose.  The module does
 * not talk to hardware directly: it renders the model maintained by the block
 * layer and turns user gestures (verify, repair, format) into queued jobs.
 */

#include <axiom/types.h>
#include <axiom/kernel.h>
#include "gui.h"

#define AXDU_MAX_DISKS       8
#define AXDU_MAX_PARTITIONS  10
#define AXDU_NAME_LEN        28
#define AXDU_MODEL_LEN       32
#define AXDU_ATTR_COUNT      10
#define AXDU_JOB_COUNT       6

#define AXDU_DISK_HDD        0
#define AXDU_DISK_SSD        1
#define AXDU_DISK_NVME       2
#define AXDU_DISK_USB        3
#define AXDU_DISK_VIRTUAL    4

#define AXDU_FS_NONE         0
#define AXDU_FS_AXFS         1
#define AXDU_FS_FAT32        2
#define AXDU_FS_EXT2         3
#define AXDU_FS_TMPFS        4

#define AXDU_JOB_NONE        0
#define AXDU_JOB_VERIFY      1
#define AXDU_JOB_REPAIR      2
#define AXDU_JOB_BALANCE     3
#define AXDU_JOB_FORMAT      4
#define AXDU_JOB_TRIM        5

#define AXDU_COLOUR_BG       GUI_RGB(0x14, 0x16, 0x1B)
#define AXDU_COLOUR_CARD     GUI_RGB(0x1D, 0x21, 0x29)
#define AXDU_COLOUR_TEXT     GUI_RGB(0xE9, 0xEB, 0xF1)
#define AXDU_COLOUR_MUTED    GUI_RGB(0x8C, 0x93, 0xA2)
#define AXDU_COLOUR_ACCENT   GUI_RGB(0x51, 0x8C, 0xF0)
#define AXDU_COLOUR_WARN     GUI_RGB(0xF2, 0xB0, 0x4B)
#define AXDU_COLOUR_DANGER   GUI_RGB(0xE8, 0x67, 0x67)
#define AXDU_COLOUR_BAR      GUI_RGB(0x26, 0x2C, 0x37)

typedef struct axdu_partition
{
    char label[AXDU_NAME_LEN];
    u64  offset_bytes;
    u64  size_bytes;
    u32  filesystem;
    u32  used_percent;
    bool_t bootable;
    bool_t encrypted;
    bool_t mounted;
    bool_t used;
} axdu_partition_t;

typedef struct axdu_attribute
{
    char name[AXDU_NAME_LEN];
    i32  value;
    i32  threshold;
    bool_t failing;
} axdu_attribute_t;

typedef struct axdu_disk
{
    char name[AXDU_NAME_LEN];
    char model[AXDU_MODEL_LEN];
    axdu_partition_t partitions[AXDU_MAX_PARTITIONS];
    axdu_attribute_t attributes[AXDU_ATTR_COUNT];
    u64  size_bytes;
    u32  partition_count;
    u32  attribute_count;
    u32  rotation_rpm;
    u32  temperature_c;
    u32  power_on_hours;
    u32  bad_sectors;
    u32  kind;
    u8   health_percent;
    bool_t removable;
    bool_t smart_available;
    bool_t used;
} axdu_disk_t;

typedef struct axdu_state
{
    axdu_disk_t disks[AXDU_MAX_DISKS];
    u32  disk_count;
    u32  selected;
    u32  selected_partition;
    u32  job;
    u32  job_progress;
    u32  job_disk;
    u32  jobs_completed;
    u32  sectors_read;
    u32  sectors_written;
    u32  errors;
    bool_t busy;
    bool_t confirm_pending;
    bool_t ready;
} axdu_state_t;

static axdu_state_t axdu_state;

static void axdu_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axdu_copy_string(char *dst, const char *src, u32 limit)
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

static void axdu_format_size(char *out, u64 bytes)
{
    u32 whole;
    u32 fraction;
    char digits[12];
    u32 count = 0;
    u32 i;
    if (bytes >= 0x40000000ull)
    {
        whole = (u32)(bytes >> 30);
        fraction = (u32)(((bytes >> 20) % 1024) * 10 / 1024);
        for (i = 0; i < 8 && whole != 0; i++)
        {
            digits[count++] = (char)('0' + (whole % 10));
            whole /= 10;
        }
        if (count == 0)
            digits[count++] = '0';
        for (i = 0; i < count; i++)
            out[i] = digits[count - 1 - i];
        out[count] = '.';
        out[count + 1] = (char)('0' + fraction);
        out[count + 2] = 'G';
        out[count + 3] = 'B';
        out[count + 4] = 0;
        return;
    }
    whole = (u32)(bytes >> 20);
    for (i = 0; i < 8 && whole != 0; i++)
    {
        digits[count++] = (char)('0' + (whole % 10));
        whole /= 10;
    }
    if (count == 0)
        digits[count++] = '0';
    for (i = 0; i < count; i++)
        out[i] = digits[count - 1 - i];
    out[count] = 'M';
    out[count + 1] = 'B';
    out[count + 2] = 0;
}

static const char *axdu_kind_name(u32 kind)
{
    switch (kind)
    {
    case AXDU_DISK_SSD:     return "SSD";
    case AXDU_DISK_NVME:    return "NVMe";
    case AXDU_DISK_USB:     return "USB";
    case AXDU_DISK_VIRTUAL: return "Virtual";
    default:                return "HDD";
    }
}

static const char *axdu_fs_name(u32 filesystem)
{
    switch (filesystem)
    {
    case AXDU_FS_AXFS:   return "AXFS";
    case AXDU_FS_FAT32:  return "FAT32";
    case AXDU_FS_EXT2:   return "ext2";
    case AXDU_FS_TMPFS:  return "tmpfs";
    default:             return "raw";
    }
}

static axdu_disk_t *axdu_disk(u32 index)
{
    if (index >= axdu_state.disk_count)
        return NULL_PTR;
    if (!axdu_state.disks[index].used)
        return NULL_PTR;
    return &axdu_state.disks[index];
}

/* -------------------------------------------------------------- public API */

void axdu_init(void)
{
    axdu_zero(&axdu_state, (u32)sizeof(axdu_state));
    axdu_state.ready = true;
}

u32 axdu_add_disk(const char *name, const char *model, u64 size_bytes, u32 kind)
{
    axdu_disk_t *disk;
    if (axdu_state.disk_count >= AXDU_MAX_DISKS)
        return 0xFFFFFFFFu;
    disk = &axdu_state.disks[axdu_state.disk_count];
    axdu_zero(disk, (u32)sizeof(*disk));
    axdu_copy_string(disk->name, name, AXDU_NAME_LEN);
    axdu_copy_string(disk->model, model, AXDU_MODEL_LEN);
    disk->size_bytes = size_bytes;
    disk->kind = kind;
    disk->health_percent = 100;
    disk->rotation_rpm = kind == AXDU_DISK_HDD ? 7200 : 0;
    disk->smart_available = true;
    disk->used = true;
    axdu_state.disk_count++;
    return axdu_state.disk_count - 1;
}

u32 axdu_add_partition(u32 disk_index, const char *label, u64 offset, u64 size, u32 filesystem)
{
    axdu_disk_t *disk = axdu_disk(disk_index);
    axdu_partition_t *partition;
    if (disk == NULL_PTR || disk->partition_count >= AXDU_MAX_PARTITIONS)
        return 0xFFFFFFFFu;
    if (offset + size > disk->size_bytes)
        return 0xFFFFFFFFu;
    partition = &disk->partitions[disk->partition_count];
    axdu_copy_string(partition->label, label, AXDU_NAME_LEN);
    partition->offset_bytes = offset;
    partition->size_bytes = size;
    partition->filesystem = filesystem;
    partition->used = true;
    disk->partition_count++;
    return disk->partition_count - 1;
}

bool_t axdu_set_usage(u32 disk_index, u32 partition_index, u32 percent, bool_t mounted)
{
    axdu_disk_t *disk = axdu_disk(disk_index);
    if (disk == NULL_PTR || partition_index >= disk->partition_count)
        return false;
    if (percent > 100)
        percent = 100;
    disk->partitions[partition_index].used_percent = percent;
    disk->partitions[partition_index].mounted = mounted;
    return true;
}

bool_t axdu_add_attribute(u32 disk_index, const char *name, i32 value, i32 threshold)
{
    axdu_disk_t *disk = axdu_disk(disk_index);
    axdu_attribute_t *attribute;
    if (disk == NULL_PTR || disk->attribute_count >= AXDU_ATTR_COUNT)
        return false;
    attribute = &disk->attributes[disk->attribute_count];
    axdu_copy_string(attribute->name, name, AXDU_NAME_LEN);
    attribute->value = value;
    attribute->threshold = threshold;
    attribute->failing = value < threshold;
    if (attribute->failing)
    {
        if (disk->health_percent > 5)
            disk->health_percent -= 5;
        axdu_state.errors++;
    }
    disk->attribute_count++;
    return true;
}

bool_t axdu_set_temperature(u32 disk_index, u32 celsius)
{
    axdu_disk_t *disk = axdu_disk(disk_index);
    if (disk == NULL_PTR)
        return false;
    disk->temperature_c = celsius;
    if (celsius > 70)
    {
        disk->health_percent = disk->health_percent > 10 ? disk->health_percent - 1 : 0;
        axdu_state.errors++;
    }
    return true;
}

bool_t axdu_select(u32 disk_index)
{
    if (axdu_disk(disk_index) == NULL_PTR)
        return false;
    axdu_state.selected = disk_index;
    return true;
}

u32 axdu_selected(void)
{
    return axdu_state.selected;
}

bool_t axdu_queue_job(u32 job)
{
    axdu_disk_t *disk = axdu_disk(axdu_state.selected);
    if (disk == NULL_PTR || axdu_state.busy)
        return false;
    if (job > AXDU_JOB_TRIM)
        return false;
    if (job == AXDU_JOB_FORMAT && !axdu_state.confirm_pending)
    {
        axdu_state.confirm_pending = true;
        return false;
    }
    axdu_state.job = job;
    axdu_state.job_progress = 0;
    axdu_state.job_disk = axdu_state.selected;
    axdu_state.busy = true;
    axdu_state.confirm_pending = false;
    AXIOM_UNUSED_VAR(disk);
    return true;
}

u32 axdu_progress_job(u32 step)
{
    if (!axdu_state.busy)
        return 0;
    axdu_state.job_progress += step;
    if (axdu_state.job_progress >= 100)
    {
        axdu_state.job_progress = 100;
        axdu_state.busy = false;
        axdu_state.jobs_completed++;
        axdu_state.job = AXDU_JOB_NONE;
    }
    return axdu_state.job_progress;
}

u32 axdu_job(void)
{
    return axdu_state.job;
}

u32 axdu_job_progress(void)
{
    return axdu_state.job_progress;
}

bool_t axdu_busy(void)
{
    return axdu_state.busy;
}

u32 axdu_disk_count(void)
{
    return axdu_state.disk_count;
}

u64 axdu_disk_size(u32 index)
{
    axdu_disk_t *disk = axdu_disk(index);
    return disk == NULL_PTR ? 0 : disk->size_bytes;
}

u8 axdu_health(u32 index)
{
    axdu_disk_t *disk = axdu_disk(index);
    return disk == NULL_PTR ? 0 : disk->health_percent;
}

const char *axdu_label(u32 disk_index, u32 partition_index)
{
    axdu_disk_t *disk = axdu_disk(disk_index);
    if (disk == NULL_PTR || partition_index >= disk->partition_count)
        return NULL_PTR;
    return disk->partitions[partition_index].label;
}

u32 axdu_mounted_count(void)
{
    u32 i;
    u32 j;
    u32 mounted = 0;
    for (i = 0; i < axdu_state.disk_count; i++)
    {
        const axdu_disk_t *disk = &axdu_state.disks[i];
        for (j = 0; j < disk->partition_count; j++)
        {
            if (disk->partitions[j].mounted)
                mounted++;
        }
    }
    return mounted;
}

u64 axdu_total_capacity(void)
{
    u64 total = 0;
    u32 i;
    for (i = 0; i < axdu_state.disk_count; i++)
        total += axdu_state.disks[i].size_bytes;
    return total;
}

/* ----------------------------------------------------------------- painting */

static void axdu_paint_disk_row(gui_surface_t *surface, gui_rect_t row, u32 index)
{
    const axdu_disk_t *disk = &axdu_state.disks[index];
    char size[24];
    gui_fill_round_rect(surface, row, 8,
                        index == axdu_state.selected ? GUI_RGB(0x22, 0x2C, 0x44)
                                                     : GUI_RGB(0x18, 0x1C, 0x24));
    gui_fill_round_rect(surface, gui_rect(row.x + 10, row.y + 8, 30, 30), 6,
                        GUI_RGB(0x2C, 0x34, 0x46));
    gui_text(surface, row.x + 52, row.y + 6, disk->name, AXDU_COLOUR_TEXT, 1);
    axdu_format_size(size, disk->size_bytes);
    gui_text(surface, row.x + 52, row.y + 24, size, AXDU_COLOUR_MUTED, 1);
    gui_text(surface, row.x + row.w - 96, row.y + 6, axdu_kind_name(disk->kind),
             AXDU_COLOUR_MUTED, 1);
    if (disk->health_percent < 80)
        gui_text(surface, row.x + row.w - 48, row.y + 6, "check", AXDU_COLOUR_WARN, 1);
}

static void axdu_paint_partition_bar(gui_surface_t *surface, gui_rect_t bar, u32 disk_index)
{
    const axdu_disk_t *disk = axdu_disk(disk_index);
    u32 i;
    if (disk == NULL_PTR)
        return;
    gui_fill_round_rect(surface, bar, 4, AXDU_COLOUR_BAR);
    for (i = 0; i < disk->partition_count; i++)
    {
        u64 start = disk->partitions[i].offset_bytes;
        u64 size = disk->partitions[i].size_bytes;
        i32 left = bar.x + (i32)((start * (u64)bar.w) / (disk->size_bytes == 0 ? 1 : disk->size_bytes));
        i32 width = (i32)((size * (u64)bar.w) / (disk->size_bytes == 0 ? 1 : disk->size_bytes));
        u32 colour = (i % 2 == 0) ? GUI_RGB(0x3B, 0x6B, 0xC8) : GUI_RGB(0x2F, 0x86, 0x94);
        if (width < 2)
            width = 2;
        gui_fill_rect(surface, gui_rect(left, bar.y, width, bar.h), colour);
    }
}

static void axdu_paint_details(gui_surface_t *surface, gui_rect_t card, u32 disk_index)
{
    const axdu_disk_t *disk = axdu_disk(disk_index);
    u32 i;
    char size[24];
    i32 y;
    if (disk == NULL_PTR)
        return;
    gui_text(surface, card.x + 16, card.y + 12, disk->model, AXDU_COLOUR_TEXT, 1);
    axdu_format_size(size, disk->size_bytes);
    gui_text(surface, card.x + 16, card.y + 32, size, AXDU_COLOUR_MUTED, 1);
    gui_text(surface, card.x + 100, card.y + 32, axdu_kind_name(disk->kind), AXDU_COLOUR_MUTED, 1);
    axdu_paint_partition_bar(surface, gui_rect(card.x + 16, card.y + 56, card.w - 32, 10),
                            disk_index);
    y = card.y + 78;
    for (i = 0; i < disk->partition_count && i < 4; i++)
    {
        const axdu_partition_t *partition = &disk->partitions[i];
        char psize[24];
        gui_text(surface, card.x + 16, y, partition->label, AXDU_COLOUR_TEXT, 1);
        axdu_format_size(psize, partition->size_bytes);
        gui_text(surface, card.x + 120, y, psize, AXDU_COLOUR_MUTED, 1);
        gui_text(surface, card.x + 200, y, axdu_fs_name(partition->filesystem),
                 AXDU_COLOUR_MUTED, 1);
        if (partition->mounted)
            gui_text(surface, card.x + 260, y, "mounted", AXDU_COLOUR_ACCENT, 1);
        if (partition->encrypted)
            gui_text(surface, card.x + 330, y, "encrypted", AXDU_COLOUR_MUTED, 1);
        y += 20;
    }
    y = card.y + 78 + 20 * 4 + 12;
    gui_fill_rect(surface, gui_rect(card.x + 16, y - 8, card.w - 32, 1), GUI_RGB(0x2A, 0x2F, 0x3B));
    gui_text(surface, card.x + 16, y, "SMART", AXDU_COLOUR_MUTED, 1);
    y += 20;
    for (i = 0; i < disk->attribute_count && i < 5; i++)
    {
        const axdu_attribute_t *attribute = &disk->attributes[i];
        gui_text(surface, card.x + 16, y, attribute->name, AXDU_COLOUR_TEXT, 1);
        if (attribute->failing)
            gui_text(surface, card.x + 200, y, "failing", AXDU_COLOUR_DANGER, 1);
        y += 18;
    }
}

void axdu_paint(gui_surface_t *surface, gui_rect_t bounds)
{
    gui_rect_t top = gui_rect(bounds.x, bounds.y, bounds.w, bounds.h / 2);
    gui_rect_t bottom = gui_rect(bounds.x, bounds.y + bounds.h / 2, bounds.w, bounds.h / 2);
    u32 i;
    gui_fill_rect(surface, bounds, AXDU_COLOUR_BG);
    gui_fill_round_rect(surface, gui_rect(top.x + 8, top.y + 8, top.w - 16, top.h - 16), 10,
                        AXDU_COLOUR_CARD);
    gui_text(surface, top.x + 20, top.y + 20, "Storage", AXDU_COLOUR_TEXT, 1);
    for (i = 0; i < axdu_state.disk_count && i < 4; i++)
    {
        if (!axdu_state.disks[i].used)
            continue;
        axdu_paint_disk_row(surface,
                            gui_rect(top.x + 16, top.y + 52 + (i32)i * 46, top.w - 32, 42),
                            i);
    }
    gui_fill_round_rect(surface, gui_rect(bottom.x + 8, bottom.y + 8, bottom.w - 16,
                                          bottom.h - 16), 10, GUI_RGB(0x1A, 0x1E, 0x26));
    axdu_paint_details(surface, gui_rect(bottom.x + 8, bottom.y + 8, bottom.w - 16,
                                         bottom.h - 16), axdu_state.selected);
    if (axdu_state.busy)
    {
        gui_rect_t bar = gui_rect(bottom.x + 16, bottom.y + bottom.h - 40, bottom.w - 32, 8);
        gui_fill_round_rect(surface, bar, 4, AXDU_COLOUR_BAR);
        gui_fill_round_rect(surface,
                            gui_rect(bar.x, bar.y, (bar.w * (i32)axdu_state.job_progress) / 100, 8),
                            4, AXDU_COLOUR_ACCENT);
    }
}

bool_t axdu_click(i32 x, i32 y, gui_rect_t bounds)
{
    gui_rect_t top = gui_rect(bounds.x, bounds.y, bounds.w, bounds.h / 2);
    u32 i;
    if (!gui_rect_contains(top, x, y))
        return false;
    for (i = 0; i < axdu_state.disk_count && i < 4; i++)
    {
        gui_rect_t row = gui_rect(top.x + 16, top.y + 52 + (i32)i * 46, top.w - 32, 42);
        if (gui_rect_contains(row, x, y))
            return axdu_select(i);
    }
    return false;
}

void axdu_report(void)
{
    u32 i;
    u32 j;
    printk("axdu: disks=%u mounted=%u capacity=%lu jobs=%u busy=%u\n",
           (unsigned)axdu_state.disk_count, (unsigned)axdu_mounted_count(),
           (unsigned long)axdu_total_capacity(), (unsigned)axdu_state.jobs_completed,
           (unsigned)axdu_state.busy);
    for (i = 0; i < axdu_state.disk_count; i++)
    {
        const axdu_disk_t *disk = &axdu_state.disks[i];
        if (!disk->used)
            continue;
        printk("  disk %u %s %s parts=%u health=%u%% temp=%uC\n", (unsigned)i,
               disk->name, disk->model, (unsigned)disk->partition_count,
               (unsigned)disk->health_percent, (unsigned)disk->temperature_c);
        for (j = 0; j < disk->partition_count; j++)
            printk("    part %u %s fs=%s used=%u%% mounted=%u\n", (unsigned)j,
                   disk->partitions[j].label,
                   axdu_fs_name(disk->partitions[j].filesystem),
                   (unsigned)disk->partitions[j].used_percent,
                   (unsigned)disk->partitions[j].mounted);
    }
}

bool_t axdu_selftest(void)
{
    u32 disk;
    u32 partition;
    axdu_init();
    disk = axdu_add_disk("nvme0n1", "Axiom NVMe 512", 512ull * 1024 * 1024 * 1024, AXDU_DISK_NVME);
    if (disk != 0)
        return false;
    partition = axdu_add_partition(0, "AXFS-ROOT", 1024 * 1024, 64ull * 1024 * 1024 * 1024,
                                   AXDU_FS_AXFS);
    if (partition != 0)
        return false;
    if (axdu_add_partition(0, "too big", 64ull * 1024 * 1024 * 1024,
                           600ull * 1024 * 1024 * 1024, AXDU_FS_FAT32) != 0xFFFFFFFFu)
        return false;
    if (axdu_add_partition(0, "DATA", 80ull * 1024 * 1024 * 1024,
                           128ull * 1024 * 1024 * 1024, AXDU_FS_FAT32) != 1)
        return false;
    if (!axdu_set_usage(0, 0, 62, true))
        return false;
    if (!axdu_add_attribute(0, "Reallocated", 4, 10))
        return false;
    if (!axdu_add_attribute(0, "WearLevel", 92, 20))
        return false;
    if (!axdu_set_temperature(0, 41))
        return false;
    if (axdu_mounted_count() != 1)
        return false;
    if (axdu_total_capacity() != 512ull * 1024 * 1024 * 1024)
        return false;
    if (!axdu_queue_job(AXDU_JOB_VERIFY))
        return false;
    if (axdu_progress_job(60) != 60)
        return false;
    if (axdu_progress_job(50) != 100)
        return false;
    if (axdu_busy())
        return false;
    if (axdu_state.jobs_completed != 1)
        return false;
    if (axdu_queue_job(AXDU_JOB_FORMAT))
        return false;
    if (!axdu_state.confirm_pending)
        return false;
    if (!axdu_queue_job(AXDU_JOB_FORMAT))
        return false;
    if (!axdu_queue_job(AXDU_JOB_TRIM))
        return false;
    if (axdu_health(0) != 100)
        return false;
    return true;
}

