; AxiomOS kernel entry point.
;
; Called by the UEFI loader through axi_jump_to_kernel with:
;   rdi = pointer to struct abp_boot_info
;   rsp = kernel stack (16-byte aligned minus 8, as a call frame would be)
;
; This stub is deliberately minimal: it installs a flat GDT so the kernel does
; not depend on firmware segment cache contents, then jumps into C land.  The
; kernel's own GDT (with TSS and per-CPU descriptors) replaces this one later
; during gdt_init().

BITS 64
DEFAULT REL

global _start
extern kernel_main
extern __bss_start
extern __bss_end

section .text.entry
_start:
    cli
    cld

    ; --- install the bootstrap GDT ----------------------------------
    lgdt        [rel .gdt_ptr]

    ; Reload CS with a far return to the 64-bit code selector.
    push        qword 0x0008
    lea         rax, [rel .after_cs]
    push        rax
    retfq

.after_cs:
    mov         ax, 0x0010
    mov         ds, ax
    mov         es, ax
    mov         fs, ax
    mov         gs, ax
    mov         ss, ax

    ; --- clear direction flag and SSE state -------------------------
    fninit
    ldmxcsr     [rel .mxcsr_default]

    ; --- align the stack --------------------------------------------
    mov         rbp, rdi                ; keep boot_info in rbp across the call
    and         rsp, -16
    sub         rsp, 8                  ; SysV: rsp % 16 == 8 at entry

    ; --- zero .bss ----------------------------------------------------
    ; The loader already zeroed NOBITS, but a debug build may have relocated
    ; the image; clearing again costs microseconds and removes doubt.
    lea         rdi, [rel __bss_start]
    lea         rcx, [rel __bss_end]
    sub         rcx, rdi
    shr         rcx, 3
    xor         rax, rax
    rep stosq

    ; --- call into C ---------------------------------------------------
    mov         rdi, rbp                ; boot_info -> first argument
    xor         rax, rax                ; SysV varargs: rax = #vector regs
    call        kernel_main

.halt:
    cli
    hlt
    jmp         .halt

align 16
.mxcsr_default:
    dd          0x1F80                  ; all SSE exceptions masked

align 8
.gdt:
    dq          0x0000000000000000      ; null
    dq          0x00AF9A000000FFFF      ; 0x08: 64-bit code, DPL0
    dq          0x00AF92000000FFFF      ; 0x10: data, DPL0
    dq          0x00AFFA000000FFFF      ; 0x18: 64-bit code, DPL3
    dq          0x00AFF2000000FFFF      ; 0x20: data, DPL3
.gdt_end:

.gdt_ptr:
    dw          .gdt_end - .gdt - 1
    dq          .gdt

section .note.GNU-stack noalloc noexec nowrite progbits
