; ==============================================================================
; fakeos - Kernel thread context switching, spinlocks and IRQ control
; ==============================================================================

[bits 64]
default rel

extern kthread_exit

global switch_context
global thread_trampoline
global spin_lock
global spin_unlock
global cpu_irq_save
global cpu_irq_restore

section .text

; ------------------------------------------------------------------------------
; Cooperative/preemptive context switch (SysV AMD64 callee-saved convention)
;   void switch_context(u64 *prev_rsp [RDI], u64 next_rsp [RSI])
;
; Saves the six callee-saved registers on the current stack, persists RSP into
; *prev_rsp, loads next_rsp and restores the target's frame. A freshly created
; thread's stack is synthesized to look exactly like this frame, with the
; return address pointing at thread_trampoline.
; ------------------------------------------------------------------------------
align 16
switch_context:
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     [rdi], rsp
    mov     rsp, rsi
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret

; First-time entry of a new kernel thread. Initial frame registers:
;   RBX = argument (passed in RDI to the thread entry)
;   R12 = entry function address
align 16
thread_trampoline:
    mov     rdi, rbx
    sti                             ; Kernel threads run interrupt-enabled
    mov     rax, r12
    call    rax
    xor     edi, edi                ; Normal exit code 0
    call    kthread_exit
.hang:                                ; kthread_exit never returns
    cli
    hlt
    jmp     .hang

; ------------------------------------------------------------------------------
; Ticket-free test-and-set spinlock
;   void spin_lock(volatile u32 *lock [RDI])
; ------------------------------------------------------------------------------
align 16
spin_lock:
    mov     eax, 1
.retry:
    xchg    [rdi], eax
    test    eax, eax
    jz      .acquired
.pause:
    pause
    mov     eax, 1
    jmp     .retry
.acquired:
    ret

;   void spin_unlock(volatile u32 *lock [RDI])
align 16
spin_unlock:
    mov     dword [rdi], 0
    ret

; ------------------------------------------------------------------------------
; Interrupt flag save/restore
;   u64 cpu_irq_save(void)      -> previous RFLAGS (with interrupts disabled)
;   void cpu_irq_restore(u64 f) -> RDI restored via POPFQ
; ------------------------------------------------------------------------------
cpu_irq_save:
    pushfq
    pop     rax
    cli
    ret

cpu_irq_restore:
    push    rdi
    popfq
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
