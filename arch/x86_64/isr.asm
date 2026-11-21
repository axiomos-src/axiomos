; AxiomOS - interrupt and exception stubs.
;
; Every vector from 0 to 255 has a stub that normalises the stack frame and
; calls irq_dispatch(vector, frame).  Vectors that push an error code are
; handled by the ISR_NOERR/ISR_ERR macros so that the frame layout is always
; identical:
;
;   +0   r15
;   +8   r14
;   ...  (see struct interrupt_frame in idt.h)
;
; This lets C handlers read registers without knowing which stub fired.

BITS 64
DEFAULT REL

extern irq_dispatch

%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push    qword 0
    push    qword %1
    jmp     isr_common
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push    qword %1
    jmp     isr_common
%endmacro

section .text

%assign i 0
%rep 256
  %if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
  %else
    ISR_NOERR i
  %endif
%assign i i+1
%endrep

isr_common:
    ; Save general purpose registers in the order expected by
    ; struct interrupt_frame (lowest address first).
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    ; Kernel-mode segments for the handler.
    xor     eax, eax
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax

    ; If we came from user mode the CPU switched to the kernel stack, but if
    ; we came from kernel mode we are already on it.  Save the interrupted
    ; stack pointer so the scheduler can resume the task.
    mov     rdi, [rsp + 15*8]           ; vector number
    mov     rsi, rsp
    ; Align the stack for the C call (SysV requires 16-byte alignment).
    mov     rbp, rsp
    and     rsp, -16
    call    irq_dispatch
    mov     rsp, rbp

    ; Restore registers in reverse.
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
    add     rsp, 16                          ; discard vector + error code
    iretq

; --------------------------------------------------------------------
; Table of stub addresses, exported for idt_init().
; --------------------------------------------------------------------
section .rodata
global isr_table
isr_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
