;/* fakeos userland C runtime entry (Gas AT&T for fakecc) */
    .text
    .globl _start
    .extern main
    .extern u_exit

_start:
    xor %ebp, %ebp
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    call main
    mov %rax, %rdi
    call u_exit
.hang:
    hlt
    jmp .hang
