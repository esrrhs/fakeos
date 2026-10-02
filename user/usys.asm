; ==============================================================================
; fakeos userland - raw SYSCALL/SYSRET wrappers (SysV AMD64 args, nr in RAX)
; ==============================================================================

[bits 64]
default rel

global u_write
global u_exit
global u_getpid
global u_yield
global u_fork

section .text

; Syscall ABI: number in RAX, args in RDI, RSI, RDX, R10, R8, R9.
; RCX/R11 are destroyed by the CPU (return RIP/RFLAGS); RCX is therefore
; unusable for the 4th argument, which the kernel reads from R10 instead.

; long u_write(long fd, const void *buf, unsigned long len)
align 16
u_write:
    mov     eax, 1
    syscall
    ret

; void u_exit(long code) - does not return
align 16
u_exit:
    mov     eax, 60
    syscall
.hang:
    hlt                         ; unreachable if the kernel honored exit
    jmp     .hang

; long u_getpid(void)
align 16
u_getpid:
    mov     eax, 39
    syscall
    ret

; long u_yield(void)
align 16
u_yield:
    mov     eax, 24
    syscall
    ret

; long u_fork(void)
align 16
u_fork:
    mov     eax, 57
    syscall
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
