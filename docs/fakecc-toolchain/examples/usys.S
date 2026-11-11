/* gas port of fakeos user/usys.asm — macro stubs */
    .text

    .macro USYSC1 name nr
    .globl \name
    .align 16
\name:
    mov $\nr, %eax
    syscall
    ret
    .endm

    USYSC1 u_read, 0
    USYSC1 u_write, 1
    USYSC1 u_open, 2
    USYSC1 u_close, 3
    USYSC1 u_exit, 60

    .globl u_mmap
    .align 16
u_mmap:
    mov %rcx, %r10
    mov $9, %eax
    syscall
    ret
