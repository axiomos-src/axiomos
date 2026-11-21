/*
 * AxiomOS boot protocol (Axiom Boot Protocol, "ABP v1")
 *
 * The UEFI loader fills in this structure and passes a pointer to it in RDI
 * when transferring control to the kernel entry point.  The structure is
 * allocated by the loader from EfiLoaderData and is therefore identity address-
 * able (firmware does not remap memory for PE images in the way Linux kernel
 * does); the kernel consumes it before reusing the pages.
 */
#ifndef AXIOM_BOOT_INFO_H
#define AXIOM_BOOT_INFO_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed int         i32;
typedef signed long long   i64;
typedef unsigned long      usize;

#define ABP_MAGIC       0x4F5346424D495841ULL /* "AXIMBFS0" */
#define ABP_VERSION     1

#define ABP_MAX_MEMMAP_ENTRIES  512
#define ABP_MAX_CMDLINE_CHARS   1024

/* Memory region types exported to the kernel.  These are AxiomOS types, not
 * firmware types: the loader normalises the map so the kernel never has to
 * know about EfiLoaderCode and friends. */
enum abp_memory_type {
    ABP_MEM_USABLE      = 0,  /* general purpose RAM, free after handoff      */
    ABP_MEM_RESERVED    = 1,  /* reserved by firmware, never touched          */
    ABP_MEM_ACPI_RECLAIM= 2,  /* ACPI tables, reusable after table parsing    */
    ABP_MEM_ACPI_NVS    = 3,  /* ACPI non-volatile storage                    */
    ABP_MEM_KERNEL      = 4,  /* kernel image + modules                       */
    ABP_MEM_BOOTINFO    = 5,  /* this structure and loader scratch            */
    ABP_MEM_FRAMEBUFFER = 6,  /* GOP framebuffer, must be mapped WC           */
    ABP_MEM_UNUSABLE    = 7
};

struct abp_memory_entry {
    u64 base;
    u64 length;
    u32 type;
    u32 attributes;     /* 1 = EFI_MEMORY_WB, bit1 = runtime */
    u64 reserved;
};

struct abp_framebuffer {
    u64 base;
    u32 width;
    u32 height;
    u32 stride;         /* pixels per scanline, not bytes */
    u32 bpp;
    u32 red_mask;
    u32 green_mask;
    u32 blue_mask;
    u32 reserved_mask;
    u32 pixel_format;   /* 0 = RGB reserve, 1 = BGR reserve, 2 = bitmask */
    u32 rotate;         /* 0 = none */
    u32 reserved[2];
};

struct abp_module {
    char name[64];
    u64 base;
    u64 size;
    u64 entry;
};

/* The complete handoff block.  Keep 8-byte aligned; the size is fixed so that
 * future versions can be version-checked safely. */
struct abp_boot_info {
    u64 magic;
    u32 version;
    u32 size;

    u64 kernel_base;            /* physical base of the loaded kernel image */
    u64 kernel_size;
    u64 kernel_entry;           /* relocated entry point (physical)         */
    u64 stack_top;              /* initial rsp value                        */
    u64 stack_size;

    u64 acpi_rsdp;              /* physical address of RSDP/XSDT, 0 if none */
    u64 efi_system_table;       /* for runtime services, physical address   */
    u64 efi_image_handle;

    u32 memmap_entry_count;
    u32 memmap_entry_size;
    u64 memmap_entries;         /* phys addr of struct abp_memory_entry[]  */

    struct abp_framebuffer fb;

    u32 module_count;
    u32 reserved0;
    u64 modules;                /* phys addr of struct abp_module[]        */

    char cmdline[ABP_MAX_CMDLINE_CHARS];
    char loader_name[64];
    char loader_version[32];

    u64 boot_unix_time;         /* seconds since epoch from EFI RTC, if any */
    u64 reserved[16];
} __attribute__((packed));

#endif /* AXIOM_BOOT_INFO_H */
