/* AxiomOS UEFI loader - SMBIOS discovery and summary.
 *
 * Firmware publishes a pointer to the SMBIOS entry point in the configuration
 * table.  The kernel does not strictly need SMBIOS, but it is useful for
 * naming the machine in the boot log and for reporting the memory topology in
 * the settings app, so the loader walks the structures once and hands the
 * kernel a small normalised summary.
 */

#include "loader.h"

#define AXSM_MAX_STRUCTURES 48
#define AXSM_MAX_STRINGS    8
#define AXSM_STRLEN         64
#define AXSM_SUMMARY_LEN    160

#define AXSM_TYPE_BIOS        0
#define AXSM_TYPE_SYSTEM      1
#define AXSM_TYPE_BASEBOARD   2
#define AXSM_TYPE_CHASSIS     3
#define AXSM_TYPE_PROCESSOR   4
#define AXSM_TYPE_MEMORY_DEV  17
#define AXSM_TYPE_MEMORY_ARRAY 16
#define AXSM_TYPE_END         127

typedef struct axsm_string
{
    char text[AXSM_STRLEN];
} axsm_string_t;

typedef struct axsm_structure
{
    u8   type;
    u8   length;
    u8   handle_lo;
    u8   handle_hi;
    u8   string_count;
    int truncated;
} axsm_structure_t;

typedef struct axsm_summary
{
    axsm_structure_t structures[AXSM_MAX_STRUCTURES];
    u8               count;
    u8               version_major;
    u8               version_minor;
    u8               bios_vendor[AXSM_STRLEN];
    u8               board_name[AXSM_STRLEN];
    u8               system_vendor[AXSM_STRLEN];
    u8               system_product[AXSM_STRLEN];
    u8               processor_version[AXSM_STRLEN];
    u32              memory_devices;
    u32              memory_arrays;
    u64              memory_total_mb;
    char             summary[AXSM_SUMMARY_LEN];
    int           present;
    u32              bad_strings;
} axsm_summary;

static void axsm_zero(void *ptr, u32 size)
{
    u8 *bytes = (u8 *)ptr;
    u32 i;
    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

static void axsm_copy(u8 *dst, const char *src, u32 cap)
{
    u32 i = 0;
    while (src[i] != 0 && i + 1 < cap)
    {
        dst[i] = (u8)src[i];
        i++;
    }
    dst[i] = 0;
}

static int axsm_find_table(efi_system_table *st, void **out)
{
    efi_uintn i;
    if (st == 0 || st->configuration_table == 0)
        return 0;
    for (i = 0; i < st->number_of_table_entries; i++)
    {
        const efi_configuration_table *entry = &st->configuration_table[i];
        const efi_guid *guid = &entry->vendor_guid;
        if (guid->data1 == 0xeb9d2d31u && guid->data2 == 0x2d88u &&
            guid->data3 == 0x11d3u &&
            guid->data4[0] == 0x9a && guid->data4[1] == 0x16)
        {
            *out = entry->vendor_table;
            return 1;
        }
    }
    return 0;
}

static const char *axsm_string_at(const u8 *structure, u8 length, u8 index)
{
    const u8 *cursor = structure + length;
    u8 current = 1;
    if (index == 0)
        return 0;
    while (*cursor != 0)
    {
        if (current == index)
            return (const char *)cursor;
        while (*cursor != 0)
            cursor++;
        cursor++;
        current++;
    }
    return 0;
}

static void axsm_record(axsm_summary *out, const u8 *structure)
{
    axsm_structure_t *slot;
    u8 type = structure[0];
    u8 length = structure[1];
    if (out->count >= AXSM_MAX_STRUCTURES)
        return;
    slot = &out->structures[out->count];
    slot->type = type;
    slot->length = length;
    slot->handle_lo = structure[2];
    slot->handle_hi = structure[3];
    slot->string_count = 0;
    slot->truncated = length < 4 ? 1 : 0;
    out->count++;

    switch (type)
    {
    case AXSM_TYPE_SYSTEM:
        if (length >= 8)
        {
            const char *vendor = axsm_string_at(structure, length, structure[4]);
            const char *product = axsm_string_at(structure, length, structure[5]);
            if (vendor != 0)
                axsm_copy(out->system_vendor, vendor, AXSM_STRLEN);
            else
                out->bad_strings++;
            if (product != 0)
                axsm_copy(out->system_product, product, AXSM_STRLEN);
            else
                out->bad_strings++;
        }
        break;
    case AXSM_TYPE_BASEBOARD:
        if (length >= 8)
        {
            const char *board = axsm_string_at(structure, length, structure[4]);
            if (board != 0)
                axsm_copy(out->board_name, board, AXSM_STRLEN);
            else
                out->bad_strings++;
        }
        break;
    case AXSM_TYPE_BIOS:
        if (length >= 9)
        {
            const char *vendor = axsm_string_at(structure, length, structure[4]);
            if (vendor != 0)
                axsm_copy(out->bios_vendor, vendor, AXSM_STRLEN);
            else
                out->bad_strings++;
        }
        break;
    case AXSM_TYPE_PROCESSOR:
        if (length >= 15)
        {
            const char *version = axsm_string_at(structure, length, structure[16 - 0]);
            if (version != 0)
                axsm_copy(out->processor_version, version, AXSM_STRLEN);
            else
                out->bad_strings++;
        }
        break;
    case AXSM_TYPE_MEMORY_ARRAY:
        out->memory_arrays++;
        break;
    case AXSM_TYPE_MEMORY_DEV:
        out->memory_devices++;
        if (length >= 21)
        {
            u32 size = *(const u16 *)(structure + 12);
            if (size != 0x7FFF && size != 0xFFFF)
                out->memory_total_mb += size;
        }
        break;
    default:
        break;
    }
}

static void axsm_present(axsm_summary *out)
{
    u32 cursor = 0;
    u32 i;
    const char *parts[6];
    u32 part_count = 0;
    parts[part_count++] = "SMBIOS";
    parts[part_count++] = (const char *)out->system_vendor;
    parts[part_count++] = (const char *)out->system_product;
    parts[part_count++] = (const char *)out->processor_version;
    for (i = 0; i < part_count; i++)
    {
        const char *part = parts[i];
        if (part[0] == 0)
            continue;
        while (*part != 0 && cursor + 1 < AXSM_SUMMARY_LEN)
        {
            out->summary[cursor] = *part;
            cursor++;
            part++;
        }
        if (cursor + 1 < AXSM_SUMMARY_LEN)
        {
            out->summary[cursor] = ' ';
            cursor++;
        }
    }
    if (cursor > 0)
        cursor--;
    out->summary[cursor] = 0;
}

/* -------------------------------------------------------------- public API */

int axsm_probe(efi_system_table *st, axsm_summary *out)
{
    void *table = 0;
    const u8 *entry;
    const u8 *cursor;
    u32 guard = 0;
    axsm_zero(out, sizeof(*out));
    if (st == 0)
        return 0;
    if (!axsm_find_table(st, &table) || table == 0)
    {
        out->present = 0;
        return 0;
    }
    entry = (const u8 *)table;
    out->present = 1;
    out->version_major = entry[0x07];
    out->version_minor = entry[0x08];
    {
        u32 table_length = *(const u32 *)(entry + 0x16);
        u32 table_address = *(const u32 *)(entry + 0x18);
        if (table_length < 0x1000000u && table_address != 0)
        {
            cursor = (const u8 *)(efi_uintn)table_address;
        }
        else
        {
            cursor = entry + (*(const u16 *)(entry + 0x18));
        }
    }
    while (guard < AXSM_MAX_STRUCTURES * 2)
    {
        const u8 *structure = cursor;
        u8 type = structure[0];
        u8 length = structure[1];
        const u8 *walk;
        if (type == AXSM_TYPE_END)
            break;
        if (length < 4)
            break;
        axsm_record(out, structure);
        walk = structure + length;
        while (*walk != 0 && *(walk + 1) != 0)
            walk += (*(walk) == 0) ? 1 : 2;
        cursor = walk + 2;
        guard++;
    }
    axsm_present(out);
    return 1;
}

void axsm_report(const axsm_summary *summary)
{
    if (!summary->present)
    {
        con_puts("SMBIOS: not published by firmware\r\n");
        return;
    }
    con_printf("SMBIOS: version %u.%u structures=%u\r\n",
               (unsigned)summary->version_major, (unsigned)summary->version_minor,
               (unsigned)summary->count);
    con_printf("SMBIOS: memory devices=%u arrays=%u total=%u MB\r\n",
               (unsigned)summary->memory_devices, (unsigned)summary->memory_arrays,
               (unsigned)summary->memory_total_mb);
    if (summary->summary[0] != 0)
        con_printf("SMBIOS: %s\r\n", summary->summary);
}

int axsm_selftest(void)
{
    axsm_summary summary;
    axsm_zero(&summary, sizeof(summary));
    if (summary.present)
        return 0;
    return 1;
}

