/* AxiomOS kernel linker script.
 *
 * The kernel is linked at 1 MiB, matching the physical placement used by the
 * UEFI loader (ELF p_paddr == p_vaddr for phase 1).  Sections are page
 * aligned so that the loader can map each PT_LOAD segment with independent
 * permissions later on.
 */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    . = 0x100000;
    __kernel_start = .;

    .text : ALIGN(4096) {
        *(.text.entry)
        *(.text .text.*)
    }
    __text_end = .;

    .rodata : ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        *(.rodata.font*)
        __rodata_end = .;
    }

    .data : ALIGN(4096) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment)
        *(.interp)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.rela*)
    }
}

/* Symbols exported to C for the integrity manifest. */
__kernel_size = __kernel_end - __kernel_start;
