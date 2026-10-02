; ==============================================================================
; fakeos userland - C runtime entry. The kernel builds an SysV initial stack:
;   RSP -> argc, argv pointers..., NULL, NULL (empty envp)
; main() follows the hosted convention and its return value becomes exit code.
; ==============================================================================

[bits 64]
default rel

global _start
extern main
extern u_exit

section .text

align 16
_start:
    xor     rbp, rbp
    mov     rdi, [rsp]              ; argc
    lea     rsi, [rsp + 8]          ; argv
    call    main
    mov     rdi, rax                ; main's return value is the exit code
    call    u_exit
.hang:
    hlt
    jmp     .hang

section .note.GNU-stack noalloc noexec nowrite progbits
