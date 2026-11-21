/*
 * AxiomOS UEFI loader - final handoff stub (GNU as syntax).
 *
 * This is the GAS-toolchain twin of jump.asm and exists so that the loader can
 * be assembled by mingw-w64's `as` on hosts where nasm is unavailable.
 *
 * C prototype (ms_abi):
 *   void axi_jump_to_kernel(u64 entry, u64 bootinfo, u64 stack_top)
 *   rcx = entry, rdx = bootinfo, r8 = stack_top
 *
 * Target (System V ABI):  kmain(struct abp_boot_info *bi)   // rdi = bootinfo
 */

        .text
        .globl  axi_jump_to_kernel
        .def    axi_jump_to_kernel;     .scl 2; .type 32; .endef
        .p2align 4

axi_jump_to_kernel:
        cli
        cld
        movq    %rdx, %rdi
        movq    %r8, %rsp
        xorq    %rbp, %rbp
        xorq    %rbx, %rbx
        xorq    %r12, %r12
        xorq    %r13, %r13
        xorq    %r14, %r14
        xorq    %r15, %r15
        fninit
        movq    %rcx, %rax
        jmpq    *%rax
        .p2align 4
