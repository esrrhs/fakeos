;/* fakeos - SYSCALL/SYSRET path and ring-3 trampolines (Gas AT&T for fakecc)
 * Per-CPU GS conduit: [gs:0]=kstack top, [gs:8]=user-RSP scratch.
 */
    .text
    .globl syscall_entry, syscall_epilogue
    .globl user_iret_trampoline, fork_ret_trampoline
    .extern syscall_dispatch

syscall_entry:
    mov %rsp, %gs:8
    mov %gs:0, %rsp
    sub $264, %rsp
    movdqa %xmm0,  0x08(%rsp)
    movdqa %xmm1,  0x18(%rsp)
    movdqa %xmm2,  0x28(%rsp)
    movdqa %xmm3,  0x38(%rsp)
    movdqa %xmm4,  0x48(%rsp)
    movdqa %xmm5,  0x58(%rsp)
    movdqa %xmm6,  0x68(%rsp)
    movdqa %xmm7,  0x78(%rsp)
    movdqa %xmm8,  0x88(%rsp)
    movdqa %xmm9,  0x98(%rsp)
    movdqa %xmm10, 0xa8(%rsp)
    movdqa %xmm11, 0xb8(%rsp)
    movdqa %xmm12, 0xc8(%rsp)
    movdqa %xmm13, 0xd8(%rsp)
    movdqa %xmm14, 0xe8(%rsp)
    movdqa %xmm15, 0xf8(%rsp)
    push %r11
    push %rcx
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r9
    push %rax
    mov %gs:8, %rax
    mov %rax, 120(%rsp)
    mov %rsp, %rdi
    cld
    call syscall_dispatch

syscall_epilogue:
    add $8, %rsp
    pop %r9
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    pop %rcx
    pop %r11
    movdqa 0x08(%rsp), %xmm0
    movdqa 0x18(%rsp), %xmm1
    movdqa 0x28(%rsp), %xmm2
    movdqa 0x38(%rsp), %xmm3
    movdqa 0x48(%rsp), %xmm4
    movdqa 0x58(%rsp), %xmm5
    movdqa 0x68(%rsp), %xmm6
    movdqa 0x78(%rsp), %xmm7
    movdqa 0x88(%rsp), %xmm8
    movdqa 0x98(%rsp), %xmm9
    movdqa 0xa8(%rsp), %xmm10
    movdqa 0xb8(%rsp), %xmm11
    movdqa 0xc8(%rsp), %xmm12
    movdqa 0xd8(%rsp), %xmm13
    movdqa 0xe8(%rsp), %xmm14
    movdqa 0xf8(%rsp), %xmm15
    mov (%rsp), %rsp
    sysretq

user_iret_trampoline:
    mov %r13, %rdi
    push $0x1B
    push %rbx
    push $0x202
    push $0x23
    push %r12
    iretq

fork_ret_trampoline:
    xor %eax, %eax
    mov %r12, %rsp
    jmp syscall_epilogue
