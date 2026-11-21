/* AxiomOS UEFI loader - ELF64 image loader.
 *
 * The kernel is linked as a static ET_EXEC image with 2 MiB aligned PT_LOAD
 * segments so that the loader can place them at their physical load address
 * with a single firmware allocation each.  p_paddr is honoured when non-zero;
 * otherwise p_vaddr is used (AxiomOS builds with -Ttext and identity mapping
 * for phase 1).
 */

#include "loader.h"

#define PT_LOAD 1
#define PT_NOTE 4
#define PT_DYNAMIC 2
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62

struct elf64_ehdr {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};

struct elf64_phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};

#define ELF_PF_X 1
#define ELF_PF_W 2
#define ELF_PF_R 4

int elf_load(struct root_volume *vol, efi_boot_services *bs, const char *path,
             struct loaded_elf *out)
{
    void *file = 0;
    efi_uint64 file_size = 0;
    struct elf64_ehdr *eh;
    struct elf64_phdr *ph;
    u64 lowest = ~0ULL;
    u64 highest = 0;
    int rc = -1;

    u_memset(out, 0, sizeof(*out));
    if (fs_read_file(vol, path, &file, &file_size, EfiLoaderData) != 0)
        return -1;

    eh = (struct elf64_ehdr *)file;
    if (u_memcmp(eh->ident, "\177ELF", 4) != 0) {
        con_puts("[elf] bad magic\n");
        goto done;
    }
    if (eh->ident[4] != 2 || eh->ident[5] != 1) {
        con_puts("[elf] not a little-endian ELF64 image\n");
        goto done;
    }
    if (eh->machine != EM_X86_64) {
        con_printf("[elf] wrong machine %u\n", (u64)eh->machine);
        goto done;
    }
    if (eh->type != ET_EXEC) {
        con_printf("[elf] unexpected type %u (ET_EXEC expected)\n",
                   (u64)eh->type);
        goto done;
    }

    con_printf("[elf] entry %p, %u program headers at offset %u\n",
               (void *)eh->entry, (u64)eh->phnum, (u64)eh->phoff);
    if (eh->phentsize != sizeof(struct elf64_phdr)) {
        con_puts("[elf] unexpected program header size\n");
        goto done;
    }

    ph = (struct elf64_phdr *)((u8 *)file + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        u64 dest;
        u64 seg_paddr;
        u64 seg_size;
        if (ph[i].type != PT_LOAD)
            continue;
        if (ph[i].filesz == 0 && ph[i].memsz == 0)
            continue;
        if (ph[i].offset + ph[i].filesz > file_size) {
            con_puts("[elf] segment extends past end of file\n");
            goto done;
        }

        seg_paddr = ph[i].paddr ? ph[i].paddr : ph[i].vaddr;
        seg_size = ph[i].memsz;

        dest = memmap_alloc_at(bs, seg_paddr, seg_size, EfiLoaderCode);
        if (!dest) {
            con_printf("[elf] cannot place segment %i at %p (%u bytes)\n",
                       (u64)i, (void *)seg_paddr, seg_size);
            goto done;
        }
        if (dest != (seg_paddr & ~EFI_PAGE_MASK)) {
            con_printf("[elf] segment %i relocated to %p\n", (u64)i,
                       (void *)dest);
        }
        u_memcpy((void *)(dest + (seg_paddr & EFI_PAGE_MASK)),
                 (u8 *)file + ph[i].offset, ph[i].filesz);
        if (ph[i].memsz > ph[i].filesz) {
            u_memset((void *)(dest + (seg_paddr & EFI_PAGE_MASK) + ph[i].filesz),
                     0, ph[i].memsz - ph[i].filesz);
        }

        if (seg_paddr < lowest)
            lowest = seg_paddr;
        if (seg_paddr + seg_size > highest)
            highest = seg_paddr + seg_size;
        con_printf("[elf] segment %i: vaddr %p -> %p, filesz %u memsz %u %s%s%s\n",
                   (u64)i, (void *)ph[i].vaddr, (void *)seg_paddr,
                   ph[i].filesz, ph[i].memsz,
                   (ph[i].flags & ELF_PF_R) ? "r" : "-",
                   (ph[i].flags & ELF_PF_W) ? "w" : "-",
                   (ph[i].flags & ELF_PF_X) ? "x" : "-");
    }

    out->entry = eh->entry;
    out->base = lowest;
    out->size = highest - lowest;
    out->image_size = file_size;
    out->phdr_count = eh->phnum;
    rc = 0;

done:
    return rc;
}
