; AxiomOS UEFI loader - final handoff stub.
;
; The loader is built with the Microsoft x64 ABI (mingw-w64), while the kernel
; is built with the System V ABI (GNU toolchain).  This stub is the single
; place where the two meet: it converts the argument registers and switches to
; the kernel stack.
;
; C prototype (ms_abi):
;   void axi_jump_to_kernel(u64 entry, u64 bootinfo, u64 stack_top)
;   rcx = entry, rdx = bootinfo, r8 = stack_top
;
; Target (sysv):
;   void kmain(struct abp_boot_info *bi)   // rdi = bootinfo

BITS 64
DEFAULT REL

global axi_jump_to_kernel

section .text
axi_jump_to_kernel:
    cli
    cld
    mov     rdi, rdx            ; bootinfo -> first SysV argument
    mov     rsp, r8             ; kernel stack
    xor     rbp, rbp
    xor     rbx, rbx
    xor     r12, r12
    xor     r13, r13
    xor     r14, r14
    xor     r15, r15
    ; Clear direction flag and make sure the x87/SSE units are sane.
    fninit
    mov     rax, rcx            ; entry point
    jmp     rax

section .note.GNU-stack noalloc noexec nowrite progbits
