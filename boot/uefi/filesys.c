/* AxiomOS UEFI loader - file system access.
 *
 * Only the EFI SimpleFileSystem protocol is used, which is backed by the
 * firmware FAT driver.  The loader reads kernel.elf and optional configuration
 * files from the ESP; writing is provided for the loader's crash log so that
 * a failed boot leaves evidence behind for the next run.
 */

#include "loader.h"

int fs_open_root(efi_boot_services *bs, efi_handle image_handle,
                 struct root_volume *out)
{
    efi_guid fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    efi_guid li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    efi_loaded_image_protocol *li = 0;
    efi_status status;

    u_memset(out, 0, sizeof(*out));
    status = bs->handle_protocol(image_handle, &li_guid, (void **)&li);
    if (EFI_ERROR(status) || !li) {
        con_printf("[fs] loaded image protocol unavailable: %x\n", (u64)status);
        return -1;
    }

    /* Try the device the image was loaded from first; fall back to the first
     * file-system handle in the system so that booting from a chain-loaded
     * loader still works. */
    if (li->device_handle) {
        status = bs->handle_protocol(li->device_handle, &fs_guid, (void **)&out->fs);
    } else {
        status = EFI_NOT_FOUND;
    }
    if (EFI_ERROR(status) || !out->fs) {
        efi_handle *handles = 0;
        efi_uintn count = 0;
        status = bs->locate_handle_buffer(ByProtocol, &fs_guid, 0, &count, &handles);
        if (EFI_ERROR(status) || count == 0) {
            con_printf("[fs] no simple file system: %x\n", (u64)status);
            return -1;
        }
        status = bs->handle_protocol(handles[0], &fs_guid, (void **)&out->fs);
        if (EFI_ERROR(status))
            return -1;
    }

    status = out->fs->open_volume(out->fs, &out->root);
    if (EFI_ERROR(status) || !out->root) {
        con_printf("[fs] open_volume failed: %x\n", (u64)status);
        return -1;
    }
    con_puts("[fs] volume opened\n");
    return 0;
}

static void make_ucs2_path(const char *path, efi_char16 *dst, efi_uintn cap)
{
    efi_uintn i = 0;
    /* Firmware paths use backslash separators. */
    while (path[i] && i + 1 < cap) {
        dst[i] = (path[i] == '/') ? (efi_char16)'\\' : (efi_char16)path[i];
        i++;
    }
    dst[i] = 0;
}

int fs_exists(struct root_volume *vol, const char *path)
{
    efi_char16 ucs2[260];
    efi_file_protocol *file = 0;
    efi_status status;
    if (!vol || !vol->root)
        return 0;
    make_ucs2_path(path, ucs2, 260);
    status = vol->root->open(vol->root, &file, ucs2, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status))
        return 0;
    file->close(file);
    return 1;
}

int fs_read_file(struct root_volume *vol, const char *path, void **buffer,
                 efi_uint64 *size, efi_uintn memory_type)
{
    efi_char16 ucs2[260];
    efi_file_protocol *file = 0;
    efi_status status;
    efi_uint64 file_size = 0;
    efi_uint8 info_buf[512];
    efi_uintn info_size = sizeof(info_buf);
    efi_file_info *info = (efi_file_info *)info_buf;

    if (!vol || !vol->root)
        return -1;

    make_ucs2_path(path, ucs2, 260);
    status = vol->root->open(vol->root, &file, ucs2, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file) {
        con_printf("[fs] cannot open %s (%x)\n", path, (u64)status);
        return -1;
    }

    status = file->get_info(file, &(efi_guid)EFI_FILE_INFO_ID, &info_size, info);
    if (!EFI_ERROR(status))
        file_size = info->file_size;
    if (!file_size) {
        /* Some implementations do not report size for directories. */
        con_printf("[fs] %s reports zero size\n", path);
        file->close(file);
        return -1;
    }

    *buffer = (void *)memmap_alloc_any_raw(file_size + EFI_PAGE_SIZE, memory_type);
    if (!*buffer) {
        con_printf("[fs] out of memory reading %s (%u bytes)\n", path,
                   (u64)file_size);
        file->close(file);
        return -1;
    }

    {
        efi_uintn read_size = (efi_uintn)file_size;
        status = file->read(file, &read_size, *buffer);
        if (EFI_ERROR(status)) {
            con_printf("[fs] read %s failed: %x\n", path, (u64)status);
            file->close(file);
            return -1;
        }
        file_size = read_size;
    }

    file->close(file);
    *size = file_size;
    con_printf("[fs] loaded %s (%u bytes) at %p\n", path, file_size, *buffer);
    return 0;
}

int fs_write_file(struct root_volume *vol, const char *path,
                  const void *buffer, efi_uint64 size)
{
    efi_char16 ucs2[260];
    efi_file_protocol *file = 0;
    efi_status status;
    efi_uintn write_size = (efi_uintn)size;

    if (!vol || !vol->root)
        return -1;
    make_ucs2_path(path, ucs2, 260);
    status = vol->root->open(vol->root, &file, ucs2,
                             EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE |
                             EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file) {
        con_printf("[fs] cannot create %s (%x)\n", path, (u64)status);
        return -1;
    }
    status = file->write(file, &write_size, buffer);
    file->close(file);
    return EFI_ERROR(status) ? -1 : 0;
}
