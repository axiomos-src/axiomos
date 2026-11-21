/*
 * AxiomOS - UEFI Boot Loader
 * efi.h - Minimal but complete EFI type definitions for x86_64 firmware.
 *
 * This header intentionally mirrors the UEFI 2.8/2.9 specification using the
 * Microsoft x64 calling convention (which is what GCC targeting mingw-w64
 * emits and what firmware expects).  Only the definitions actually required
 * by the loader are provided, but the writer kept the layout faithful to the
 * spec so that firmware-returned pointers are interpreted correctly.
 */
#ifndef AXIOM_EFI_H
#define AXIOM_EFI_H

/* ------------------------------------------------------------------ */
/* Fundamental types                                                   */
/* ------------------------------------------------------------------ */

typedef unsigned char       efi_bool;
typedef unsigned char       efi_uint8;
typedef unsigned short      efi_uint16;
typedef unsigned int        efi_uint32;
typedef unsigned long long  efi_uint64;
typedef signed char         efi_int8;
typedef signed short        efi_int16;
typedef signed int          efi_int32;
typedef signed long long    efi_int64;
typedef unsigned long long  efi_uintn;
typedef efi_int64           efi_intn;
typedef char                efi_char8;
typedef efi_uint16          efi_char16;

typedef efi_uintn           efi_status;
typedef void               *efi_handle;
typedef void               *efi_event;

#define EFI_TRUE  ((efi_bool)1)
#define EFI_FALSE ((efi_bool)0)

#define EFIAPI __attribute__((ms_abi))
#define EFI_INLINE static inline

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */

#define EFI_SUCCESS                 0
#define EFI_ERROR_BIT               ((efi_uintn)0x8000000000000000ULL)
#define EFI_LOAD_ERROR              (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER       (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED             (EFI_ERROR_BIT | 3)
#define EFI_BAD_BUFFER_SIZE         (EFI_ERROR_BIT | 4)
#define EFI_BUFFER_TOO_SMALL        (EFI_ERROR_BIT | 5)
#define EFI_NOT_READY               (EFI_ERROR_BIT | 6)
#define EFI_DEVICE_ERROR            (EFI_ERROR_BIT | 7)
#define EFI_WRITE_PROTECTED         (EFI_ERROR_BIT | 8)
#define EFI_OUT_OF_RESOURCES        (EFI_ERROR_BIT | 9)
#define EFI_VOLUME_CORRUPTED        (EFI_ERROR_BIT | 10)
#define EFI_VOLUME_FULL             (EFI_ERROR_BIT | 11)
#define EFI_NO_MEDIA                (EFI_ERROR_BIT | 12)
#define EFI_MEDIA_CHANGED           (EFI_ERROR_BIT | 13)
#define EFI_NOT_FOUND               (EFI_ERROR_BIT | 14)
#define EFI_ACCESS_DENIED           (EFI_ERROR_BIT | 15)
#define EFI_NO_RESPONSE             (EFI_ERROR_BIT | 16)
#define EFI_NO_MAPPING              (EFI_ERROR_BIT | 17)
#define EFI_TIMEOUT                 (EFI_ERROR_BIT | 18)
#define EFI_NOT_STARTED             (EFI_ERROR_BIT | 19)
#define EFI_ALREADY_STARTED         (EFI_ERROR_BIT | 20)
#define EFI_ABORTED                 (EFI_ERROR_BIT | 21)
#define EFI_SECURITY_VIOLATION      (EFI_ERROR_BIT | 26)

/* Firmware sets bit 63 for every failing status, so test that bit alone
 * instead of comparing the whole value against EFI_SUCCESS. */
#define EFI_ERROR(status)           (((status) & EFI_ERROR_BIT) != 0)

/* ------------------------------------------------------------------ */
/* GUIDs and table header                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    efi_uint32 data1;
    efi_uint16 data2;
    efi_uint16 data3;
    efi_uint8  data4[8];
} efi_guid;

#define EFI_GUID(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { (a), (b), (c), { (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) } }

/* EFI System Table */
#define EFI_SYSTEM_TABLE_SIGNATURE      0x5453595320494249ULL
/* EFI Boot Services Table */
#define EFI_BOOT_SERVICES_SIGNATURE     0x56524553544f4f42ULL
/* EFI Runtime Services Table */
#define EFI_RUNTIME_SERVICES_SIGNATURE  0x56524553544e5552ULL

typedef struct {
    efi_uint64 signature;
    efi_uint32 revision;
    efi_uint32 header_size;
    efi_uint32 crc32;
    efi_uint32 reserved;
} efi_table_header;

/* ------------------------------------------------------------------ */
/* Memory types and allocation                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14,
    EfiMaximumMemoryType = 15
} efi_memory_type;

typedef enum {
    AllocateAnyPages = 0,
    AllocateMaxAddress = 1,
    AllocateAddress = 2,
    MaxAllocateType = 3
} efi_allocate_type;

#define EFI_PAGE_SIZE 4096ULL
#define EFI_PAGE_MASK (EFI_PAGE_SIZE - 1)
#define EFI_PAGES_TO_SIZE(p) ((p) << 12)

typedef struct {
    efi_uint32 type;
    efi_uint32 pad;
    efi_uint64 physical_start;
    efi_uint64 virtual_start;
    efi_uint64 number_of_pages;
    efi_uint64 attribute;
} efi_memory_descriptor;

#define EFI_MEMORY_UC            0x0000000000000001ULL
#define EFI_MEMORY_WC            0x0000000000000002ULL
#define EFI_MEMORY_WT            0x0000000000000004ULL
#define EFI_MEMORY_WB            0x0000000000000008ULL
#define EFI_MEMORY_UCE           0x0000000000000010ULL
#define EFI_MEMORY_WP            0x0000000000001000ULL
#define EFI_MEMORY_RP            0x0000000000002000ULL
#define EFI_MEMORY_XP            0x0000000000004000ULL
#define EFI_MEMORY_RUNTIME       (0x8000000000000000ULL)

/* ------------------------------------------------------------------ */
/* Protocols and services                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    AllHandles = 0,
    ByRegisterNotify = 1,
    ByProtocol = 2
} efi_locate_search_type;

typedef struct efi_table_header_system efi_system_table;
typedef struct efi_table_header_boot   efi_boot_services;
typedef struct efi_table_header_runtime efi_runtime_services;

typedef efi_status (EFIAPI *efi_get_time)(void *, void *);
typedef efi_status (EFIAPI *efi_set_time)(void *);

/* Text output ------------------------------------------------------- */

typedef struct {
    efi_status (EFIAPI *reset)(void *this, efi_bool extended_verification);
    efi_status (EFIAPI *output_string)(void *this, const efi_char16 *str);
    efi_status (EFIAPI *test_string)(void *this, const efi_char16 *str);
    efi_status (EFIAPI *query_mode)(void *this, efi_uintn mode, efi_uintn *cols, efi_uintn *rows);
    efi_status (EFIAPI *set_mode)(void *this, efi_uintn mode);
    efi_status (EFIAPI *set_attribute)(void *this, efi_uintn attr);
    efi_status (EFIAPI *clear_screen)(void *this);
    efi_status (EFIAPI *set_cursor_position)(void *this, efi_uintn col, efi_uintn row);
    efi_status (EFIAPI *enable_cursor)(void *this, efi_bool visible);
    const void *mode;
} efi_simple_text_output_protocol;

/* Graphics output --------------------------------------------------- */

typedef struct {
    efi_uint8 blue;
    efi_uint8 green;
    efi_uint8 red;
    efi_uint8 reserved;
} efi_graphics_output_blt_pixel;

typedef enum {
    EfiPixelFormatRedGreenBlueReserved8BitPerColor = 0,
    EfiPixelFormatBlueGreenRedReserved8BitPerColor = 1,
    EfiPixelFormatBitMask = 2,
    EfiPixelFormatPixelBltOnly = 3,
    EfiPixelFormatPixelFormatMax = 4
} efi_graphics_pixel_format;

typedef struct {
    efi_uint32 red_mask;
    efi_uint32 green_mask;
    efi_uint32 blue_mask;
    efi_uint32 reserved_mask;
} efi_pixel_bitmask;

typedef struct {
    efi_uint32 version;
    efi_uint32 horizontal_resolution;
    efi_uint32 vertical_resolution;
    efi_graphics_pixel_format pixel_format;
    efi_pixel_bitmask pixel_information;
    efi_uint32 pixels_per_scan_line;
} efi_graphics_output_mode_information;

typedef struct {
    efi_uint32 max_mode;
    efi_uint32 mode;
    efi_graphics_output_mode_information *info;
    efi_uint32 size_of_info;
    efi_uint64 frame_buffer_base;
    efi_uint32 frame_buffer_size;
} efi_graphics_output_mode;

typedef enum {
    EfiBltVideoFill = 0,
    EfiBltVideoToBltBuffer = 1,
    EfiBltBufferToVideo = 2,
    EfiBltVideoToVideo = 3,
    EfiGraphicsOutputBltOperationMax = 4
} efi_graphics_output_blt_operation;

typedef struct efi_graphics_output_protocol {
    efi_status (EFIAPI *query_mode)(struct efi_graphics_output_protocol *this,
                                    efi_uint32 mode_number,
                                    efi_uint32 *size_of_info,
                                    efi_graphics_output_mode_information **info);
    efi_status (EFIAPI *set_mode)(struct efi_graphics_output_protocol *this,
                                  efi_uint32 mode_number);
    void *blt;
    efi_graphics_output_mode *mode;
} efi_graphics_output_protocol;

/* Simple file system ------------------------------------------------- */

typedef struct {
    efi_uint64 size;
    efi_uint64 file_size;
    efi_uint64 physical_size;
    efi_uint8  create_time[16];
    efi_uint8  last_access_time[16];
    efi_uint8  modification_time[16];
    efi_uint64 attribute;
    efi_char16 file_name[260];
} efi_file_info;

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_DIRECTORY   0x0000000000000010ULL
#define EFI_FILE_HIDDEN      0x0000000000000002ULL

typedef struct efi_file_protocol {
    efi_uint64 revision;
    efi_status (EFIAPI *open)(struct efi_file_protocol *this,
                              struct efi_file_protocol **new_handle,
                              const efi_char16 *file_name,
                              efi_uint64 open_mode,
                              efi_uint64 attributes);
    efi_status (EFIAPI *close)(struct efi_file_protocol *this);
    efi_status (EFIAPI *delete_file)(struct efi_file_protocol *this);
    efi_status (EFIAPI *read)(struct efi_file_protocol *this,
                              efi_uintn *buffer_size,
                              void *buffer);
    efi_status (EFIAPI *write)(struct efi_file_protocol *this,
                               efi_uintn *buffer_size,
                               const void *buffer);
    efi_status (EFIAPI *get_position)(struct efi_file_protocol *this, efi_uint64 *position);
    efi_status (EFIAPI *set_position)(struct efi_file_protocol *this, efi_uint64 position);
    efi_status (EFIAPI *get_info)(struct efi_file_protocol *this,
                                  const efi_guid *info_type,
                                  efi_uintn *buffer_size,
                                  void *buffer);
    efi_status (EFIAPI *set_info)(struct efi_file_protocol *this,
                                  const efi_guid *info_type,
                                  efi_uintn buffer_size,
                                  void *buffer);
    efi_status (EFIAPI *flush)(struct efi_file_protocol *this);
} efi_file_protocol;

typedef struct {
    efi_uint64 revision;
    efi_status (EFIAPI *open_volume)(void *this, efi_file_protocol **root);
} efi_simple_file_system_protocol;

/* Device path -------------------------------------------------------- */

typedef struct {
    efi_uint8 type;
    efi_uint8 sub_type;
    efi_uint8 length[2];
} efi_device_path_protocol;

/* Loaded image ------------------------------------------------------- */

typedef struct {
    efi_uint32 revision;
    efi_handle parent_handle;
    efi_system_table *system_table;
    efi_handle device_handle;
    efi_device_path_protocol *file_path;
    void *reserved;
    efi_uint32 load_options_size;
    void *load_options;
    void *image_base;
    efi_uint64 image_size;
    efi_uintn image_code_type;
    efi_uintn image_data_type;
    efi_status (EFIAPI *unload)(efi_handle image_handle);
} efi_loaded_image_protocol;

/* Configuration tables ----------------------------------------------- */

typedef struct {
    efi_guid vendor_guid;
    void *vendor_table;
} efi_configuration_table;

#define EFI_ACPI_TABLE_GUID     EFI_GUID(0x8868e871, 0xe4f1, 0x11d3, 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81)
#define EFI_ACPI_20_TABLE_GUID  EFI_ACPI_TABLE_GUID
#define EFI_SMBIOS_TABLE_GUID   EFI_GUID(0xeb9d2d31, 0x2d88, 0x11d3, 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d)

/* ------------------------------------------------------------------ */
/* Boot services                                                       */
/* ------------------------------------------------------------------ */

typedef efi_status (EFIAPI *efi_allocate_pages)(efi_allocate_type type,
                                                efi_uintn memory_type,
                                                efi_uintn pages,
                                                efi_uint64 *memory);
typedef efi_status (EFIAPI *efi_free_pages)(efi_uint64 memory, efi_uintn pages);
typedef efi_status (EFIAPI *efi_get_memory_map)(efi_uintn *map_size,
                                                efi_memory_descriptor *map,
                                                efi_uintn *map_key,
                                                efi_uintn *descriptor_size,
                                                efi_uint32 *descriptor_version);
typedef efi_status (EFIAPI *efi_allocate_pool)(efi_uintn pool_type,
                                               efi_uintn size,
                                               void **buffer);
typedef efi_status (EFIAPI *efi_free_pool)(void *buffer);
typedef efi_status (EFIAPI *efi_exit_boot_services)(efi_handle image_handle,
                                                   efi_uintn map_key);
typedef efi_status (EFIAPI *efi_handle_protocol)(efi_handle handle,
                                                 const efi_guid *protocol,
                                                 void **interface);
typedef efi_status (EFIAPI *efi_locate_protocol)(const efi_guid *protocol,
                                                 void *registration,
                                                 void **interface);
typedef efi_status (EFIAPI *efi_locate_handle_buffer)(efi_locate_search_type search_type,
                                                      const efi_guid *protocol,
                                                      void *search_key,
                                                      efi_uintn *no_handles,
                                                      efi_handle **buffer);
typedef efi_status (EFIAPI *efi_open_protocol)(efi_handle handle,
                                               const efi_guid *protocol,
                                               void **interface,
                                               efi_handle agent_handle,
                                               efi_handle controller_handle,
                                               efi_uint32 attributes);
typedef efi_status (EFIAPI *efi_stall)(efi_uintn microseconds);
typedef efi_status (EFIAPI *efi_set_watchdog_timer)(efi_uintn timeout,
                                                   void *code,
                                                   efi_uintn data_size,
                                                   efi_char16 *data);

struct efi_table_header_boot {
    efi_table_header hdr;
    void *raise_tpl;
    void *restore_tpl;
    efi_allocate_pages  allocate_pages;
    efi_free_pages      free_pages;
    efi_get_memory_map  get_memory_map;
    efi_allocate_pool   allocate_pool;
    efi_free_pool       free_pool;
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    efi_handle_protocol       handle_protocol;
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    efi_exit_boot_services    exit_boot_services;
    void *get_next_monotonic_count;
    efi_stall                 stall;
    efi_set_watchdog_timer    set_watchdog_timer;
    void *connect_controller;
    void *disconnect_controller;
    void *open_protocol;
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    efi_locate_handle_buffer  locate_handle_buffer;
    efi_locate_protocol       locate_protocol;
    void *install_multiple_protocol_interfaces;
    void *uninstall_multiple_protocol_interfaces;
    void *calculate_crc32;
    void *copy_mem;
    void *set_mem;
    void *create_event_ex;
};

struct efi_table_header_runtime {
    efi_table_header hdr;
    efi_get_time get_time;
    efi_set_time set_time;
    void *get_wakeup_time;
    void *set_wakeup_time;
    void *set_virtual_address_map;
    void *convert_pointer;
    void *get_variable;
    void *get_next_variable_name;
    void *set_variable;
    void *get_next_high_monotonic_count;
    efi_status (EFIAPI *reset_system)(efi_uint32 reset_type, efi_status status,
                                      efi_uintn data_size, void *reset_data);
};

struct efi_table_header_system {
    efi_table_header hdr;
    efi_char16 *firmware_vendor;
    efi_uint32 firmware_revision;
    efi_handle console_in_handle;
    void *con_in;
    efi_handle console_out_handle;
    efi_simple_text_output_protocol *con_out;
    efi_handle standard_error_handle;
    efi_simple_text_output_protocol *std_err;
    efi_runtime_services *runtime_services;
    efi_boot_services *boot_services;
    efi_uintn number_of_table_entries;
    efi_configuration_table *configuration_table;
};

/* ------------------------------------------------------------------ */
/* Protocol GUIDs used by the loader                                   */
/* ------------------------------------------------------------------ */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    EFI_GUID(0x9042a9de, 0x23dc, 0x4a38, 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a)
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    EFI_GUID(0x964e5b22, 0x6459, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    EFI_GUID(0x5b1b31a1, 0x9562, 0x11d2, 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_FILE_INFO_ID \
    EFI_GUID(0x09576e92, 0x6d3f, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    EFI_GUID(0x09576e91, 0x6d3f, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_MP_SERVICES_PROTOCOL_GUID \
    EFI_GUID(0x3fdda605, 0xa76e, 0x4f46, 0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3f, 0x04)

/* ------------------------------------------------------------------ */
/* Device path helpers                                                 */
/* ------------------------------------------------------------------ */

#define DEVICE_PATH_TYPE_MEDIA_DEVICE   0x04
#define DEVICE_PATH_SUB_TYPE_FILE_PATH  0x04

typedef struct {
    efi_uint8 type;
    efi_uint8 sub_type;
    efi_uint8 length[2];
    efi_char16 path[1];
} efi_file_path_device_path;

/* ------------------------------------------------------------------ */
/* Inline helpers                                                      */
/* ------------------------------------------------------------------ */

EFI_INLINE void axi_zero_mem(void *buffer, efi_uintn size)
{
    efi_uint8 *p = (efi_uint8 *)buffer;
    while (size--)
        *p++ = 0;
}

EFI_INLINE void axi_copy_mem(void *dst, const void *src, efi_uintn size)
{
    efi_uint8 *d = (efi_uint8 *)dst;
    const efi_uint8 *s = (const efi_uint8 *)src;
    while (size--)
        *d++ = *s++;
}

EFI_INLINE efi_uintn axi_str_len16(const efi_char16 *str)
{
    efi_uintn n = 0;
    while (str[n])
        n++;
    return n;
}

#endif /* AXIOM_EFI_H */

