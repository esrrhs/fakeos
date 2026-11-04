/* gas port of fakeos user/crt0.asm */
    .text
    .globl _start
    .extern main
    .extern u_exit

    .align 16
_start:
    xor %rbp, %rbp
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    call main
    mov %rax, %rdi
    call u_exit
.hang:
    hlt
    jmp .hang
