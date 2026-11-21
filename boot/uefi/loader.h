/* AxiomOS UEFI loader - internal interfaces */
#ifndef AXIOM_LOADER_H
#define AXIOM_LOADER_H

#include "efi.h"
#include "bootinfo.h"

/* ---- console (loader.c / console.c) --------------------------------- */
void        con_init(efi_system_table *st);
void        con_puts(const char *s);
void        con_printf(const char *fmt, ...);
void        con_clear(void);
void        con_hex(u64 value);
void        con_dec(u64 value);

/* ---- graphics (gop.c) ---------------------------------------------- */
struct gop_state {
    efi_graphics_output_protocol *proto;
    efi_uint32 width;
    efi_uint32 height;
    efi_uint32 stride;
    efi_uint32 bpp;
    efi_uint32 red_mask;
    efi_uint32 green_mask;
    efi_uint32 blue_mask;
    efi_uint32 reserved_mask;
    efi_uint32 pixel_format;
    efi_uint64 fb_base;
    efi_uint64 fb_size;
    efi_uint32 chosen_mode;
};

int  gop_init(efi_boot_services *bs, struct gop_state *out);
void gop_fill(struct gop_state *g, efi_uint32 color);
void gop_putpixel(struct gop_state *g, efi_uint32 x, efi_uint32 y, efi_uint32 color);
void gop_hline(struct gop_state *g, efi_uint32 x, efi_uint32 y, efi_uint32 w, efi_uint32 color);
void gop_rect(struct gop_state *g, u32 x, u32 y, u32 w, u32 h, u32 color);
void con_draw_string(u8 *fb, u32 pitch, u32 x, u32 y, const char *s, u32 fg, u32 scale);
void con_draw_char(u8 *fb, u32 pitch, u32 x, u32 y, char c, u32 fg, u32 scale);
void gop_progress(struct gop_state *g, efi_uint32 percent);
void gop_banner(struct gop_state *g);

/* ---- filesystem access (filesys.c) --------------------------------- */
struct root_volume {
    efi_file_protocol *root;
    efi_simple_file_system_protocol *fs;
};

int  fs_open_root(efi_boot_services *bs, efi_handle image_handle, struct root_volume *out);
int  fs_read_file(struct root_volume *vol, const char *path, void **buffer,
                  efi_uint64 *size, efi_uintn memory_type);
int  fs_write_file(struct root_volume *vol, const char *path,
                   const void *buffer, efi_uint64 size);
int  fs_exists(struct root_volume *vol, const char *path);

/* ---- ELF loading (elf.c) ------------------------------------------- */
struct loaded_elf {
    efi_uint64 entry;       /* relocated entry, physical address */
    efi_uint64 base;        /* lowest loaded vaddr              */
    efi_uint64 size;        /* span of image                    */
    efi_uint64 image_size;  /* file size                        */
    efi_uint64 phdr_count;
};

int  elf_load(struct root_volume *vol, efi_boot_services *bs, const char *path,
              struct loaded_elf *out);

/* ---- memory map (memmap.c) ----------------------------------------- */
struct memmap_state {
    efi_memory_descriptor *map;
    efi_uintn map_size;
    efi_uintn map_key;
    efi_uintn descriptor_size;
    efi_uint32 descriptor_version;
    /* normalised copy handed to the kernel */
    struct abp_memory_entry *entries;
    efi_uint32 entry_count;
    efi_uint64 total_ram;
    efi_uint64 highest_address;
};

int  memmap_acquire(efi_boot_services *bs, struct memmap_state *out);
int  memmap_normalize(struct memmap_state *mm,
                      efi_uint64 kernel_base, efi_uint64 kernel_size,
                      efi_uint64 bootinfo_base, efi_uint64 bootinfo_size,
                      efi_uint64 fb_base, efi_uint64 fb_size);
int  memmap_exit_from_map(efi_boot_services *bs, efi_handle image, struct memmap_state *mm);
u64  memmap_total_usable(struct memmap_state *mm);
u64  memmap_alloc_at(efi_boot_services *bs, u64 base, u64 size, efi_uintn type);
u64  memmap_alloc_any_raw(u64 size, efi_uintn type);

/* ---- architecture glue (jump.asm) ---------------------------------- */
void axi_jump_to_kernel(u64 entry, u64 bootinfo, u64 stack_top)
    __attribute__((noreturn));
u64  memmap_alloc_below(efi_boot_services *bs, u64 limit, u64 size, efi_uintn type);

/* ---- ACPI (acpi.c) -------------------------------------------------- */
u64  acpi_find_rsdp(efi_system_table *st);

/* ---- misc utilities (util.c) --------------------------------------- */
void *u_memset(void *dst, int c, efi_uintn n);
void *u_memcpy(void *dst, const void *src, efi_uintn n);
int   u_memcmp(const void *a, const void *b, efi_uintn n);
efi_uintn u_strlen(const char *s);
int   u_streq(const char *a, const char *b);
void  u_strcpy(char *dst, const char *src, efi_uintn cap);
u64   rdtsc(void);
void  cpu_relax(void);
void  halt_forever(void) __attribute__((noreturn));
void  u_to_dec(u64 value, char *out);
void  u_to_hex(u64 value, char *out, int min_digits);
efi_uintn ascii_to_ucs2(const char *src, efi_char16 *dst, efi_uintn cap);

#endif /* AXIOM_LOADER_H */
