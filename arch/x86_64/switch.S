;/* fakeos - context switch, spinlocks, IRQ control (Gas AT&T for fakecc) */
    .text
    .globl switch_context, thread_trampoline
    .globl spin_test_and_set
    .globl spin_lock, spin_unlock, cpu_irq_save, cpu_irq_restore
    .extern kthread_exit, sched_finish_switch

switch_context:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    /* Target frame is 8 mod 16. Align for the C handoff callback. */
    sub $8, %rsp
    mov %rdx, %rdi
    call sched_finish_switch
    add $8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

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

spin_test_and_set:
    mov $1, %eax
    xchg %eax, (%rdi)
    ret

spin_lock:
    mov $1, %eax
.retry:
    xchg %eax, (%rdi)
    test %eax, %eax
    jz .acquired
    pause
    mov $1, %eax
    jmp .retry
.acquired:
    ret

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
