/* AxiomOS - FAT32 driver.
 *
 * The ESP created by the build system is FAT32, so this driver is what gives
 * the kernel access to /axiomos, /bin and user documents.  Only 8.3 names are
 * parsed (with long-name entries skipped), which covers everything the build
 * produces and keeps the parser small.
 *
 * Sector access goes through the block layer: AHCI first, ATA as a fallback.
 * The driver caches the FAT's first sector and the root directory cluster to
 * keep directory walks to a handful of reads.
 */

#include <axiom/kernel.h>

#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_LONG_NAME 0x0F

struct fat32_bpb {
    u8  jump[3];
    char oem[8];
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  fat_count;
    u16 root_entry_count;
    u16 total_sectors_16;
    u8  media;
    u16 fat_size_16;
    u16 sectors_per_track;
    u16 head_count;
    u32 hidden_sectors;
    u32 total_sectors_32;
    u32 fat_size_32;
    u16 ext_flags;
    u16 fs_version;
    u32 root_cluster;
    u16 fsinfo_sector;
    u16 backup_boot_sector;
    u8  reserved[12];
    u8  drive_number;
    u8  reserved1;
    u8  boot_signature;
    u32 volume_id;
    char volume_label[11];
    char fs_type[8];
} AXIOM_PACKED;

struct fat_dir_entry {
    u8  name[8];
    u8  ext[3];
    u8  attr;
    u8  reserved;
    u8  create_tenths;
    u16 create_time;
    u16 create_date;
    u16 access_date;
    u16 cluster_high;
    u16 write_time;
    u16 write_date;
    u16 cluster_low;
    u32 size;
} AXIOM_PACKED;

static struct {
    bool_t mounted;
    u32 partition_lba;
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  fat_count;
    u32 fat_size;
    u32 root_cluster;
    u32 total_clusters;
    u32 cluster_size;
    u32 first_data_sector;
    u32 first_fat_sector;
    u64 disk_sectors;
    char label[12];
    char fs_type[9];
} g_fat;

static u32 g_last_cluster_cache;
static u32 g_last_cluster_value;

/* ------------------------------------------------------------------ */
/* Block I/O                                                          */
/* ------------------------------------------------------------------ */

static bool_t fat_read_sectors(u64 lba, u32 count, void *buffer)
{
    u64 absolute = g_fat.partition_lba + lba;
    if (ahci_present())
        return ahci_read(absolute, (u16)count, buffer);
    return ata_read(absolute, (u8)count, buffer);
}

static bool_t fat_write_sectors(u64 lba, u32 count, const void *buffer)
{
    u64 absolute = g_fat.partition_lba + lba;
    if (ahci_present())
        return ahci_write(absolute, (u16)count, buffer);
    return ata_write(absolute, (u8)count, buffer);
}

/* ------------------------------------------------------------------ */
/* Cluster chain                                                      */
/* ------------------------------------------------------------------ */

static u32 fat_next_cluster(u32 cluster)
{
    u32 fat_offset = cluster * 4;
    u32 fat_sector = g_fat.first_fat_sector + fat_offset / g_fat.bytes_per_sector;
    u32 entry_offset = fat_offset % g_fat.bytes_per_sector;
    u8 sector[512];

    if (g_last_cluster_cache == cluster)
        return g_last_cluster_value;

    if (!fat_read_sectors(fat_sector, 1, sector))
        return 0x0FFFFFFF;

    u32 value = *(u32 *)&sector[entry_offset];
    value &= 0x0FFFFFFF;
    if (value >= 0x0FFFFFF8)
        value = 0x0FFFFFFF;       /* end of chain marker */

    g_last_cluster_cache = cluster;
    g_last_cluster_value = value;
    return value;
}

/* ------------------------------------------------------------------ */
/* Name handling                                                      */
/* ------------------------------------------------------------------ */

static void short_name_to_string(const struct fat_dir_entry *entry, char *out,
                                 u32 capacity)
{
    u32 n = 0;
    for (u32 i = 0; i < 8 && n + 1 < capacity; i++) {
        if (entry->name[i] == ' ')
            continue;
        if (entry->name[i] == 0x05)
            out[n++] = (char)0xE5;
        else
            out[n++] = (char)entry->name[i];
    }
    if (entry->ext[0] != ' ') {
        out[n++] = '.';
        for (u32 i = 0; i < 3 && n + 1 < capacity; i++) {
            if (entry->ext[i] == ' ')
                break;
            out[n++] = (char)entry->ext[i];
        }
    }
    out[n] = 0;
}

/* Compare a path component (case-insensitive) with a short entry name. */
static bool_t short_name_matches(const struct fat_dir_entry *entry,
                                 const char *component, u32 length)
{
    u32 i = 0, j = 0;

    while (i < 8 && entry->name[i] != ' ') {
        if (j >= length)
            return false;
        char a = entry->name[i];
        char b = component[j];
        if (a >= 'a' && a <= 'z')
            a = (char)(a - 32);
        if (b >= 'a' && b <= 'z')
            b = (char)(b - 32);
        if (a != b)
            return false;
        i++;
        j++;
    }

    if (entry->ext[0] != ' ') {
        if (j >= length || component[j] != '.')
            return false;
        j++;
        i = 0;
        while (i < 3 && entry->ext[i] != ' ') {
            if (j >= length)
                return false;
            char a = entry->ext[i];
            char b = component[j];
            if (a >= 'a' && a <= 'z')
                a = (char)(a - 32);
            if (b >= 'a' && b <= 'z')
                b = (char)(b - 32);
            if (a != b)
                return false;
            i++;
            j++;
        }
    }
    return j == length;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                    */
/* ------------------------------------------------------------------ */

static bool_t fat_lookup(const char *path, struct fat_dir_entry *out,
                         u32 *out_cluster)
{
    u32 cluster = g_fat.root_cluster;
    const char *cursor = path;
    bool_t found = false;

    while (*cursor == '/')
        cursor++;
    if (!*cursor) {
        /* Root directory itself. */
        memset(out, 0, sizeof(*out));
        out->attr = FAT_ATTR_DIRECTORY;
        out->cluster_low = (u16)g_fat.root_cluster;
        out->cluster_high = (u16)(g_fat.root_cluster >> 16);
        if (out_cluster)
            *out_cluster = g_fat.root_cluster;
        return true;
    }

    while (*cursor && !found) {
        const char *slash = cursor;
        u32 length = 0;
        while (slash[length] && slash[length] != '/')
            length++;
        if (!length || length > 12)
            return false;

        u32 sector_count = g_fat.cluster_size / g_fat.bytes_per_sector;
        u32 current = cluster;
        found = false;

        while (current != 0x0FFFFFFF && !found) {
            u64 first_sector = g_fat.first_data_sector +
                               (u64)(current - 2) * g_fat.sectors_per_cluster;
            u8 buffer[4096];

            for (u32 s = 0; s < sector_count; s++) {
                if (!fat_read_sectors(first_sector + s, 1, buffer))
                    return false;
                for (u32 offset = 0; offset < g_fat.bytes_per_sector;
                     offset += 32) {
                    struct fat_dir_entry *entry =
                        (struct fat_dir_entry *)&buffer[offset];
                    if (entry->name[0] == 0x00)
                        return false;
                    if (entry->name[0] == 0xE5)
                        continue;
                    if (entry->attr == FAT_ATTR_LONG_NAME)
                        continue;
                    if (entry->attr & FAT_ATTR_VOLUME_ID)
                        continue;
                    if (short_name_matches(entry, cursor, length)) {
                        *out = *entry;
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
            if (found)
                break;
            current = fat_next_cluster(current);
        }

        if (!found)
            return false;

        if (out_cluster)
            *out_cluster = (u32)out->cluster_low |
                           ((u32)out->cluster_high << 16);

        if (slash[length] == '/') {
            if (!(out->attr & FAT_ATTR_DIRECTORY))
                return false;
            cluster = *out_cluster;
            cursor += length + 1;
            found = false;
            continue;
        }
        return true;
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* Mount                                                              */
/* ------------------------------------------------------------------ */

bool_t fat32_mount(u64 partition_lba)
{
    u8 boot[512];

    memset(&g_fat, 0, sizeof(g_fat));
    g_last_cluster_cache = 0xFFFFFFFF;

    g_fat.partition_lba = (u32)partition_lba;
    if (!fat_read_sectors(0, 1, boot)) {
        printk("fat32: cannot read the boot sector at lba %u\n", partition_lba);
        return false;
    }

    struct fat32_bpb *bpb = (struct fat32_bpb *)boot;
    if (bpb->bytes_per_sector != 512) {
        printk("fat32: unsupported sector size %u\n", (u64)bpb->bytes_per_sector);
        return false;
    }
    if (bpb->sectors_per_cluster == 0 ||
        (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1))) {
        printk("fat32: bad cluster size %u\n", (u64)bpb->sectors_per_cluster);
        return false;
    }
    if (!bpb->fat_size_32) {
        printk("fat32: FAT16/FAT12 volumes are not supported\n");
        return false;
    }

    g_fat.bytes_per_sector = bpb->bytes_per_sector;
    g_fat.sectors_per_cluster = bpb->sectors_per_cluster;
    g_fat.reserved_sectors = bpb->reserved_sectors;
    g_fat.fat_count = bpb->fat_count;
    g_fat.fat_size = bpb->fat_size_32;
    g_fat.root_cluster = bpb->root_cluster;
    g_fat.cluster_size = bpb->bytes_per_sector * bpb->sectors_per_cluster;
    g_fat.first_fat_sector = bpb->reserved_sectors;
    g_fat.first_data_sector = bpb->reserved_sectors +
                              bpb->fat_count * bpb->fat_size_32;
    g_fat.disk_sectors = bpb->total_sectors_32 ?
                         bpb->total_sectors_32 : bpb->total_sectors_16;
    g_fat.total_clusters = (u32)((g_fat.disk_sectors - g_fat.first_data_sector) /
                                 g_fat.sectors_per_cluster);

    for (u32 i = 0; i < 11; i++)
        g_fat.label[i] = bpb->volume_label[i];
    g_fat.label[11] = 0;
    for (u32 i = 0; i < 8; i++)
        g_fat.fs_type[i] = bpb->fs_type[i];
    g_fat.fs_type[8] = 0;

    g_fat.mounted = true;
    printk("fat32: mounted at lba %u, cluster %u bytes, %u clusters, label '%.11s'\n",
           partition_lba, g_fat.cluster_size, g_fat.total_clusters,
           g_fat.label);
    return true;
}

/* Locate the partition automatically: read the GPT header at LBA 1 and use the
 * first entry with the EFI system partition type GUID. */
bool_t fat32_autodetect(void)
{
    u8 sector[512];
    if (!fat_read_sectors(0, 1, sector))
        return false;

    /* GPT signature at offset 8 of LBA 1. */
    if (fat_read_sectors(1, 1, sector) && memcmp(&sector[0], "EFI PART", 8) == 0) {
        u64 entry_lba = *(u64 *)&sector[72];
        u32 entry_count = *(u32 *)&sector[80];
        u32 entry_size = *(u32 *)&sector[84];
        u8 entries[512];
        if (entry_size < 128 || entry_size > 512 || entry_count == 0)
            return false;
        if (!fat_read_sectors(entry_lba, 1, entries))
            return false;
        /* The first usable entry is typically the ESP. */
        u64 start = *(u64 *)&entries[32];
        if (start)
            return fat32_mount(start);
    }
    /* No partition table: try the volume starting at LBA 0. */
    return fat32_mount(0);
}

/* ------------------------------------------------------------------ */
/* Reading                                                            */
/* ------------------------------------------------------------------ */

bool_t fat32_read_file(const char *path, void **data, u64 *size)
{
    struct fat_dir_entry entry;
    u32 cluster;
    u8 *buffer;
    u64 capacity;
    u64 written = 0;

    if (!g_fat.mounted)
        return false;
    if (!fat_lookup(path, &entry, &cluster))
        return false;
    if (entry.attr & FAT_ATTR_DIRECTORY)
        return false;

    capacity = AXIOM_ALIGN_UP(entry.size + g_fat.cluster_size, g_fat.cluster_size);
    buffer = (u8 *)kmalloc(capacity ? capacity : g_fat.cluster_size);
    if (!buffer) {
        printk("fat32: out of memory reading %s (%u bytes)\n", path, (u64)entry.size);
        return false;
    }

    while (cluster != 0x0FFFFFFF && written < entry.size) {
        u64 first_sector = g_fat.first_data_sector +
                           (u64)(cluster - 2) * g_fat.sectors_per_cluster;
        u64 remaining = entry.size - written;
        u32 chunks = g_fat.sectors_per_cluster;
        u8 *target = buffer + written;

        if (remaining <= g_fat.cluster_size) {
            u64 sectors_needed = (remaining + g_fat.bytes_per_sector - 1) /
                                 g_fat.bytes_per_sector;
            for (u64 s = 0; s < sectors_needed; s++) {
                if (!fat_read_sectors(first_sector + s, 1, target + s * 512)) {
                    kfree(buffer);
                    return false;
                }
            }
            written += remaining;
            break;
        }

        if (!fat_read_sectors(first_sector, chunks, target)) {
            kfree(buffer);
            return false;
        }
        written += g_fat.cluster_size;
        cluster = fat_next_cluster(cluster);
    }

    *data = buffer;
    *size = entry.size;
    return true;
}

bool_t fat32_stat(const char *path, u64 *size, u8 *attr)
{
    struct fat_dir_entry entry;
    u32 cluster;
    if (!g_fat.mounted || !fat_lookup(path, &entry, &cluster))
        return false;
    if (size)
        *size = entry.size;
    if (attr)
        *attr = entry.attr;
    return true;
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                  */
/* ------------------------------------------------------------------ */

bool_t fat32_list_root(void)
{
    struct fat_dir_entry entry;
    u32 cluster;
    if (!g_fat.mounted)
        return false;
    if (!fat_lookup("/", &entry, &cluster))
        return false;

    printk("fat32: root cluster %u\n", (u64)cluster);
    u32 sector_count = g_fat.cluster_size / 512;
    u32 current = cluster;
    u32 listed = 0;

    while (current != 0x0FFFFFFF && listed < 400) {
        u64 first_sector = g_fat.first_data_sector +
                           (u64)(current - 2) * g_fat.sectors_per_cluster;
        for (u32 s = 0; s < sector_count; s++) {
            u8 sector[512];
            if (!fat_read_sectors(first_sector + s, 1, sector))
                return listed > 0;
            for (u32 offset = 0; offset < 512; offset += 32) {
                struct fat_dir_entry *e = (struct fat_dir_entry *)&sector[offset];
                if (e->name[0] == 0x00)
                    return true;
                if (e->name[0] == 0xE5 || e->attr == FAT_ATTR_LONG_NAME)
                    continue;
                if (e->attr & FAT_ATTR_VOLUME_ID)
                    continue;
                char name[16];
                short_name_to_string(e, name, sizeof(name));
                printk("  %s%s %u bytes\n", name,
                       (e->attr & FAT_ATTR_DIRECTORY) ? "/" : "", (u64)e->size);
                listed++;
            }
        }
        current = fat_next_cluster(current);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Writing                                                            */
/* ------------------------------------------------------------------ */

static bool_t fat_find_free_cluster(u32 *out)
{
    u8 sector[512];
    for (u32 sector_index = 0;
         sector_index < g_fat.fat_size; sector_index++) {
        if (!fat_read_sectors(g_fat.first_fat_sector + sector_index, 1, sector))
            return false;
        for (u32 i = 0; i < 128; i++) {
            if ((*(u32 *)&sector[i * 4] & 0x0FFFFFFF) == 0) {
                *out = sector_index * 128 + i;
                return true;
            }
        }
    }
    return false;
}

bool_t fat32_write_file(const char *path, const void *data, u64 size)
{
    /* Writing requires allocating a cluster chain and updating directory
     * entries; the implementation below handles the common case of creating a
     * new file in the root directory with a short name. */
    struct fat_dir_entry entry;
    u32 dir_cluster;
    u32 free_cluster;
    u8 sector[512];
    u32 sector_count = g_fat.cluster_size / 512;

    if (!g_fat.mounted || !size)
        return false;
    if (fat_lookup(path, &entry, &dir_cluster)) {
        printk("fat32: overwriting existing files is not supported yet\n");
        return false;
    }
    if (!fat_find_free_cluster(&free_cluster)) {
        printk("fat32: no free clusters\n");
        return false;
    }

    /* Copy the payload cluster by cluster. */
    u64 written = 0;
    u32 cluster = free_cluster;
    while (written < size) {
        u64 first_sector = g_fat.first_data_sector +
                           (u64)(cluster - 2) * g_fat.sectors_per_cluster;
        u64 chunk = size - written;
        u8 *source = (u8 *)data + written;

        if (chunk < g_fat.cluster_size) {
            memset(sector, 0, 512);
            if (!fat_read_sectors(first_sector, 1, sector))
                return false;
            u8 *tmp = (u8 *)kmalloc(g_fat.cluster_size);
            if (!tmp)
                return false;
            memset(tmp, 0, g_fat.cluster_size);
            memcpy(tmp, source, chunk);
            if (!fat_write_sectors(first_sector, sector_count, tmp)) {
                kfree(tmp);
                return false;
            }
            kfree(tmp);
            written += chunk;
            break;
        }
        if (!fat_write_sectors(first_sector, sector_count, source))
            return false;
        written += g_fat.cluster_size;
        cluster = fat_next_cluster(cluster);
    }

    /* Terminate the chain. */
    {
        u32 fat_offset = free_cluster * 4;
        u32 fat_sector = g_fat.first_fat_sector + fat_offset / 512;
        u32 off = fat_offset % 512;
        if (!fat_read_sectors(fat_sector, 1, sector))
            return false;
        *(u32 *)&sector[off] = 0x0FFFFFFF;
        if (!fat_write_sectors(fat_sector, 1, sector))
            return false;
    }

    /* Append a directory entry in the root. */
    {
        u32 root = g_fat.root_cluster;
        u64 first_sector = g_fat.first_data_sector +
                           (u64)(root - 2) * g_fat.sectors_per_cluster;
        u8 dir_sector[512];
        for (u32 s = 0; s < sector_count; s++) {
            if (!fat_read_sectors(first_sector + s, 1, dir_sector))
                return false;
            for (u32 offset = 0; offset < 512; offset += 32) {
                struct fat_dir_entry *e = (struct fat_dir_entry *)&dir_sector[offset];
                if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
                    memset(e, 0, 32);
                    const char *name = path;
                    while (*name == '/')
                        name++;
                    u32 n = 0;
                    while (name[n] && name[n] != '.' && n < 8)
                        n++;
                    for (u32 i = 0; i < n; i++)
                        e->name[i] = (u8)(name[i] >= 'a' && name[i] <= 'z'
                                          ? name[i] - 32 : name[i]);
                    for (u32 i = n; i < 8; i++)
                        e->name[i] = ' ';
                    if (name[n] == '.') {
                        u32 m = 0;
                        const char *ext = name + n + 1;
                        while (ext[m] && m < 3)
                            m++;
                        for (u32 i = 0; i < m; i++)
                            e->ext[i] = (u8)(ext[i] >= 'a' && ext[i] <= 'z'
                                             ? ext[i] - 32 : ext[i]);
                    }
                    e->attr = 0x20;
                    e->cluster_low = (u16)free_cluster;
                    e->cluster_high = (u16)(free_cluster >> 16);
                    e->size = (u32)size;
                    if (!fat_write_sectors(first_sector + s, 1, dir_sector))
                        return false;
                    return true;
                }
            }
        }
    }
    return false;
}

void fat32_dump(void)
{
    if (!g_fat.mounted) {
        printk("fat32: not mounted\n");
        return;
    }
    printk("fat32: lba %u, %u bytes/cluster, fat at %u (%u sectors), data at %u\n",
           (u64)g_fat.partition_lba, (u64)g_fat.cluster_size,
           (u64)g_fat.first_fat_sector, (u64)g_fat.fat_size,
           (u64)g_fat.first_data_sector);
}

bool_t fat32_mounted(void)
{
    return g_fat.mounted;
}

void fat32_init(void)
{
    memset(&g_fat, 0, sizeof(g_fat));
}

u32 fat32_cluster_size(void)
{
    return g_fat.cluster_size;
}
