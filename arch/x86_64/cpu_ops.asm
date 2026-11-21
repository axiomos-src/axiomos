; AxiomOS - low level x86_64 primitives.
;
; Every routine here has a C prototype declared in axiom/kernel.h or in the
; module header that owns it.  Keeping the inline assembly in one file makes
; it easy to audit the ABI boundaries between C, Rust and assembly.

BITS 64
DEFAULT REL

section .text

; --------------------------------------------------------------------
; GDT / IDT loading
; void gdt_load(u64 gdtr_value, u16 code_sel, u16 data_sel)
;   rdi = packed gdtr value, si = code selector, dx = data selector
; --------------------------------------------------------------------
global gdt_load
gdt_load:
    lgdt    [rdi]

    ; reload CS through a far return
    mov     rax, rsi
    push    rax
    lea     rax, [rel .gdt_reload]
    push    rax
    retfq
.gdt_reload:
    mov     ax, dx
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    ret

; --------------------------------------------------------------------
; void tss_load(u16 selector)
; --------------------------------------------------------------------
global tss_load
tss_load:
    ltr     di
    ret

; --------------------------------------------------------------------
; void idt_load(u64 idtr)
; --------------------------------------------------------------------
global idt_load
idt_load:
    lidt    [rdi]
    ret

; --------------------------------------------------------------------
; Port I/O
; --------------------------------------------------------------------
global io_inb
io_inb:
    mov     dx, di
    xor     eax, eax
    in      al, dx
    ret

global io_inw
io_inw:
    mov     dx, di
    xor     eax, eax
    in      ax, dx
    ret

global io_inl
io_inl:
    mov     dx, di
    in      eax, dx
    ret

global io_outb
io_outb:
    mov     dx, di
    mov     al, sil
    out     dx, al
    ret

global io_outw
io_outw:
    mov     dx, di
    mov     ax, si
    out     dx, ax
    ret

global io_outl
io_outl:
    mov     dx, di
    mov     eax, esi
    out     dx, eax
    ret

global io_wait
io_wait:
    mov     al, 0
    out     0x80, al
    ret

; --------------------------------------------------------------------
; String operations
; void *memcpy(void *dst, const void *src, u64 n)
; --------------------------------------------------------------------
global memcpy
memcpy:
    mov     rax, rdi
    mov     rcx, rdx
    cmp     rcx, 8
    jb      .byte_copy
    ; SSE 16-byte copy when the length allows it
    mov     r8, rcx
    shr     rcx, 4
    cmp     rcx, 0
    je      .tail
.copy16:
    movdqu  xmm0, [rsi]
    movdqu  [rdi], xmm0
    add     rsi, 16
    add     rdi, 16
    dec     rcx
    jnz     .copy16
.tail:
    mov     rcx, r8
    and     rcx, 15
.byte_copy:
    rep movsb
    ret

global memmove
memmove:
    mov     rax, rdi
    cmp     rdi, rsi
    je      .done
    ja      .backward
    mov     rcx, rdx
    rep movsb
.done:
    ret
.backward:
    mov     rcx, rdx
    lea     rdi, [rdi + rcx - 1]
    lea     rsi, [rsi + rcx - 1]
    std
    rep movsb
    cld
    ret

; void *memset(void *dst, int value, u64 n)
global memset
memset:
    mov     r8, rdi                 ; return value
    mov     r11, rdx                ; save n
    movzx   eax, sil
    mov     r9, 0x0101010101010101
    imul    rax, r9                 ; byte pattern in rax
    mov     rcx, r11
    shr     rcx, 3
    rep stosq
    mov     rcx, r11
    and     rcx, 7
    movzx   eax, sil
    rep stosb
    mov     rax, r8
    ret

; int memcmp(const void *a, const void *b, u64 n)
global memcmp
memcmp:
    xor     eax, eax
    mov     rcx, rdx
    test    rcx, rcx
    jz      .equal
.loop:
    mov     al, [rdi]
    mov     r8b, [rsi]
    cmp     al, r8b
    jne     .diff
    inc     rdi
    inc     rsi
    dec     rcx
    jnz     .loop
.equal:
    xor     eax, eax
    ret
.diff:
    movzx   eax, al
    movzx   r8d, r8b
    sub     eax, r8d
    ret

; u64 strlen(const char *s)
global strlen
strlen:
    xor     eax, eax
.loop:
    cmp     byte [rdi + rax], 0
    je      .done
    inc     rax
    jmp     .loop
.done:
    ret

; --------------------------------------------------------------------
; CPU control
; --------------------------------------------------------------------
global cpu_get_rflags
cpu_get_rflags:
    pushfq
    pop     rax
    ret

global cpu_set_rflags
cpu_set_rflags:
    push    rdi
    popfq
    ret

global cpu_cpuid
; void cpu_cpuid(u32 leaf, u32 subleaf, u32 *out)
cpu_cpuid:
    push    rbx
    ; rdx is the output pointer, but CPUID overwrites EDX with leaf output,
    ; so the pointer has to live in a register the instruction never touches.
    mov     r11, rdx
    mov     eax, edi
    mov     ecx, esi
    cpuid
    mov     [r11], eax
    mov     [r11 + 4], ebx
    mov     [r11 + 8], ecx
    mov     [r11 + 12], edx
    pop     rbx
    ret

global cpu_hlt_forever
cpu_hlt_forever:
.cli_loop:
    cli
    hlt
    jmp     .cli_loop

global cpu_rdmsr
cpu_rdmsr:
    mov     ecx, edi
    rdmsr
    shl     rdx, 32
    or      rax, rdx
    ret

global cpu_wrmsr
cpu_wrmsr:
    mov     ecx, edi
    mov     rax, rsi
    mov     rdx, rsi
    shr     rdx, 32
    wrmsr
    ret

; --------------------------------------------------------------------
; Task/thread context switching (System V ABI)
;
; struct thread_regs layout (see arch/x86_64/thread.h):
;   +0   r15, +8 r14, +16 r13, +24 r12, +32 rbx, +40 rbp
;   +48  rsp (target stack pointer), +56 rip
; --------------------------------------------------------------------
global context_switch
; void context_switch(void **save_rsp, void *new_rsp)
context_switch:
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    pushfq
    mov     rax, rsp
    mov     [rdi], rax
    mov     rsp, rsi
    popfq
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret

global thread_bootstrap_trampoline
extern thread_exit
thread_bootstrap_trampoline:
    ; rdi = entry point, rsi = argument
    sti
    call    rdi
    call    thread_exit
.halt:
    cli
    hlt
    jmp     .halt

section .note.GNU-stack noalloc noexec nowrite progbits
