/* AxiomOS UEFI loader - memory map acquisition and normalisation.
 *
 * Firmware memory maps change whenever an allocation happens, so the loader
 * acquires the map twice: once to size buffers, once immediately before
 * ExitBootServices.  The map key returned by the last successful call must be
 * passed to ExitBootServices, otherwise the call is rejected.
 */

#include "loader.h"

static efi_boot_services *g_bs;

/* Allocate pages below 4 GiB whenever possible: AxiomOS phase 1 identity maps
 * the low 4 GiB and does not want to deal with high allocations during the
 * handoff. */
u64 memmap_alloc_any_raw(u64 size, efi_uintn type)
{
    efi_uintn pages = (efi_uintn)((size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE);
    efi_uint64 addr = 0xFFFFFFFFULL;   /* try below 4 GiB first */
    efi_status status;

    if (!g_bs || !pages)
        return 0;

    status = g_bs->allocate_pages(AllocateMaxAddress, type, pages, &addr);
    if (EFI_ERROR(status)) {
        addr = 0;
        status = g_bs->allocate_pages(AllocateAnyPages, type, pages, &addr);
        if (EFI_ERROR(status))
            return 0;
    }
    u_memset((void *)addr, 0, pages * EFI_PAGE_SIZE);
    return addr;
}

u64 memmap_alloc_at(efi_boot_services *bs, u64 base, u64 size, efi_uintn type)
{
    efi_uintn pages = (efi_uintn)((size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE);
    efi_uint64 addr = base & ~EFI_PAGE_MASK;
    efi_status status;
    pages += (efi_uintn)((base - addr) ? 1 : 0);

    status = bs->allocate_pages(AllocateAddress, type, pages, &addr);
    if (EFI_ERROR(status))
        return 0;
    return addr;
}

u64 memmap_alloc_below(efi_boot_services *bs, u64 limit, u64 size, efi_uintn type)
{
    efi_uintn pages = (efi_uintn)((size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE);
    efi_uint64 addr = limit;
    efi_status status = bs->allocate_pages(AllocateMaxAddress, type, pages, &addr);
    if (EFI_ERROR(status))
        return 0;
    return addr;
}

int memmap_acquire(efi_boot_services *bs, struct memmap_state *out)
{
    efi_status status;
    efi_uintn size = 0;
    efi_uintn key = 0;
    efi_uintn desc_size = 0;
    efi_uint32 desc_version = 0;
    int attempts = 0;
    void *buffer;

    g_bs = bs;
    u_memset(out, 0, sizeof(*out));

    status = bs->get_memory_map(&size, 0, &key, &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status)) {
        con_printf("[mm] probe failed: %x\n", (u64)status);
        return -1;
    }
    size += desc_size ? desc_size : 48;
    size += 4096;   /* firmware may grow the map between calls */

    while (attempts++ < 8) {
        buffer = (void *)memmap_alloc_any_raw(size, EfiLoaderData);
        if (!buffer)
            return -1;
        status = bs->get_memory_map(&size, (efi_memory_descriptor *)buffer, &key,
                                    &desc_size, &desc_version);
        if (!EFI_ERROR(status))
            break;
        if (status == EFI_BUFFER_TOO_SMALL)
            continue;
        con_printf("[mm] get_memory_map failed: %x\n", (u64)status);
        return -1;
    }
    if (attempts >= 8)
        return -1;

    out->map = (efi_memory_descriptor *)buffer;
    out->map_size = size;
    out->map_key = key;
    out->descriptor_size = desc_size;
    out->descriptor_version = desc_version;
    con_printf("[mm] map: %u bytes, descriptor %u bytes, version %u, key %u\n",
               (u64)size, (u64)desc_size, (u64)desc_version, (u64)key);
    return 0;
}

int memmap_normalize(struct memmap_state *mm, efi_uint64 kernel_base,
                     efi_uint64 kernel_size, efi_uint64 bootinfo_base,
                     efi_uint64 bootinfo_size, efi_uint64 fb_base,
                     efi_uint64 fb_size)
{
    efi_uintn offset;
    u32 count = 0;
    efi_uint64 highest = 0;
    efi_uint64 usable = 0;

    mm->entries = (struct abp_memory_entry *)memmap_alloc_any_raw(
        sizeof(struct abp_memory_entry) * (ABP_MAX_MEMMAP_ENTRIES + 8),
        EfiLoaderData);
    if (!mm->entries)
        return -1;

    for (offset = 0; offset < mm->map_size; offset += mm->descriptor_size) {
        efi_memory_descriptor *d =
            (efi_memory_descriptor *)((u8 *)mm->map + offset);
        struct abp_memory_entry *e;
        u32 type;

        if (!d->number_of_pages)
            continue;
        if (count >= ABP_MAX_MEMMAP_ENTRIES)
            break;

        switch (d->type) {
        case EfiConventionalMemory:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiLoaderCode:
        case EfiLoaderData:
            type = ABP_MEM_USABLE;
            break;
        case EfiACPIReclaimMemory:
            type = ABP_MEM_ACPI_RECLAIM;
            break;
        case EfiACPIMemoryNVS:
            type = ABP_MEM_ACPI_NVS;
            break;
        case EfiUnusableMemory:
            type = ABP_MEM_UNUSABLE;
            break;
        case EfiPersistentMemory:
            type = ABP_MEM_USABLE;
            break;
        default:
            type = ABP_MEM_RESERVED;
            break;
        }

        e = &mm->entries[count++];
        e->base = d->physical_start;
        e->length = EFI_PAGES_TO_SIZE(d->number_of_pages);
        e->type = type;
        e->attributes = (d->attribute & EFI_MEMORY_RUNTIME) ? 2 : 1;
        if (type == ABP_MEM_USABLE)
            usable += e->length;
        if (e->base + e->length > highest)
            highest = e->base + e->length;
    }

    /* Now stamp the regions the kernel must not reuse. */
    {
        struct {
            u64 base;
            u64 size;
            u32 type;
        } marks[3];
        int nmarks = 0;
        marks[nmarks].base = kernel_base;
        marks[nmarks].size = kernel_size;
        marks[nmarks].type = ABP_MEM_KERNEL;
        nmarks++;
        marks[nmarks].base = bootinfo_base;
        marks[nmarks].size = bootinfo_size;
        marks[nmarks].type = ABP_MEM_BOOTINFO;
        nmarks++;
        if (fb_base && fb_size) {
            marks[nmarks].base = fb_base & ~EFI_PAGE_MASK;
            marks[nmarks].size = fb_size + (fb_base & EFI_PAGE_MASK);
            marks[nmarks].type = ABP_MEM_FRAMEBUFFER;
            nmarks++;
        }

        for (int m = 0; m < nmarks; m++) {
            /* Carve the mark out of any overlapping usable region.  The
             * normalised map is deliberately allowed to contain overlapping
             * entries: the kernel PMM treats marks as hard reservations and
             * processes them in order. */
            if (count + 2 < ABP_MAX_MEMMAP_ENTRIES + 8) {
                struct abp_memory_entry *e = &mm->entries[count++];
                e->base = marks[m].base;
                e->length = marks[m].size;
                e->type = marks[m].type;
                e->attributes = 0;
            }
        }
    }

    mm->entry_count = count;
    mm->total_ram = usable;
    mm->highest_address = highest;
    con_printf("[mm] %u entries, %u MiB usable, highest %p\n",
               (u64)count, usable / (1024 * 1024), (void *)highest);
    return 0;
}

u64 memmap_total_usable(struct memmap_state *mm)
{
    return mm->total_ram;
}

int memmap_exit_from_map(efi_boot_services *bs, efi_handle image,
                         struct memmap_state *mm)
{
    efi_status status;
    efi_uintn size = mm->map_size;
    efi_uintn key = 0;
    efi_uintn desc_size = 0;
    efi_uint32 version = 0;
    int attempts;

    /* Refresh the map in place; the bootinfo block and everything else the
     * loader needs is already allocated and stays valid after exit. */
    for (attempts = 0; attempts < 8; attempts++) {
        status = bs->get_memory_map(&size, mm->map, &key, &desc_size, &version);
        if (!EFI_ERROR(status))
            break;
        if (size > mm->map_size) {
            con_puts("[mm] map grew during handoff, retrying with new buffer\n");
            mm->map = (efi_memory_descriptor *)memmap_alloc_any_raw(size + 4096,
                                                                    EfiLoaderData);
            if (!mm->map)
                return -1;
            mm->map_size = size + 4096;
        }
    }
    if (attempts >= 8)
        return -1;

    status = bs->exit_boot_services(image, key);
    if (EFI_ERROR(status)) {
        con_printf("[mm] exit_boot_services failed: %x\n", (u64)status);
        /* One retry is allowed after a map-key mismatch. */
        status = bs->get_memory_map(&size, mm->map, &key, &desc_size, &version);
        if (!EFI_ERROR(status))
            status = bs->exit_boot_services(image, key);
        if (EFI_ERROR(status))
            return -1;
    }
    return 0;
}
