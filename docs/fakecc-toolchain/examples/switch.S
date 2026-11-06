/* gas port of fakeos arch/x86_64/switch.asm — fakecc -c switch.S */
    .text
    .globl switch_context, thread_trampoline
    .globl spin_lock, spin_unlock
    .globl cpu_irq_save, cpu_irq_restore
    .extern kthread_exit

    .align 16
switch_context:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

    .align 16
thread_trampoline:
    mov %rbx, %rdi
    sti
    mov %r12, %rax
    call *%rax
    xor %edi, %edi
    call kthread_exit
.hang:
    cli
    hlt
    jmp .hang

    .align 16
spin_lock:
    mov $1, %eax
.retry:
    xchg %eax, (%rdi)
    test %eax, %eax
    jz .acquired
.pause:
    pause
    mov $1, %eax
    jmp .retry
.acquired:
    ret

    .align 16
spin_unlock:
    movl $0, (%rdi)
    ret

cpu_irq_save:
    pushfq
    pop %rax
    cli
    ret

cpu_irq_restore:
    push %rdi
    popfq
    ret
