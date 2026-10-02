; ==============================================================================
; fakeos - SYSCALL/SYSRET fast system call path and ring-3 entry trampolines
; ==============================================================================

[bits 64]
default rel

extern syscall_dispatch         ; C handler in kernel/syscall.c
extern syscall_kstack_top       ; u64 in kernel/sched.c: current kstack top

global syscall_entry
global user_iret_trampoline
global fork_ret_trampoline
global syscall_user_rsp

section .text

; ------------------------------------------------------------------------------
; SYSCALL entry (MSR_LSTAR). CPU state on entry:
;   RCX = user return RIP, R11 = user RFLAGS, RSP = user stack pointer.
;   CS/SS loaded from STAR, RFLAGS &= ~SFMASK (we mask IF|DF), no stack switch.
; The user stack pointer is stashed through a private conduit slot, then stored
; INTO the per-syscall frame (the pad slot at +120): a single global cannot
; survive preemption, since another thread's SYSCALL would overwrite it before
; a preempted thread reaches the epilogue. Frame travel also makes fork() copy
; the correct user RSP to the child for free.
;
; Frame built on the kernel stack (128 bytes, 16-byte aligned for the C call):
;   [0]   rax (syscall number)
;   [8]   r9  [16] r8  [24] r10  [32] rdx  [40] rsi  [48] rdi   (arguments)
;   [56]  r15 [64] r14 [72] r13  [80] r12  [88] rbp  [96] rbx   (user regs)
;   [104] rcx = user RIP, [112] r11 = user RFLAGS, [120] user RSP
; ------------------------------------------------------------------------------
align 16
syscall_entry:
    mov     [rel syscall_user_rsp], rsp
    mov     rsp, [rel syscall_kstack_top]
    sub     rsp, 8              ; keeps RSP 16-byte aligned for the C call
    push    r11
    push    rcx
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    push    rdi
    push    rsi
    push    rdx
    push    r10
    push    r8
    push    r9
    push    rax
    mov     rax, [rel syscall_user_rsp]   ; conduit is safe: IF=0 since entry
    mov     [rsp + 120], rax              ; frame pad slot = user RSP
    mov     rdi, rsp            ; struct syscall_frame *
    cld
    call    syscall_dispatch    ; return value in RAX

; Common return path: RAX = syscall return value, RSP = frame base.
; (fork_ret_trampoline joins here on the child's copied frame.)
global syscall_epilogue
syscall_epilogue:
    add     rsp, 8              ; drop saved RAX slot (retval already in RAX)
    pop     r9
    pop     r8
    pop     r10
    pop     rdx
    pop     rsi
    pop     rdi
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    pop     rcx                 ; user RIP
    pop     r11                 ; user RFLAGS
    mov     rsp, [rsp]          ; frame pad slot holds THIS syscall's user RSP
    ; NOTE: nasm does not recognize the "sysretq" mnemonic (it silently parses
    ; as a label definition and emits no bytes). Emit REX.W + 0F 07 by hand:
    ; this is 64-bit SYSRET (RIP=RCX, RFLAGS=R11, CS/SS from STAR, ring 3).
    db      0x48, 0x0F, 0x07

; ------------------------------------------------------------------------------
; First entry into ring 3 for a freshly created user thread. Installed as the
; return address of the synthesized switch_context frame, with:
;   R12 = user entry RIP, RBX = user stack pointer.
; CR3 was already switched to the process address space by the scheduler.
; ------------------------------------------------------------------------------
align 16
user_iret_trampoline:
    push    0x1B                ; SS  = user data (0x18) | RPL3
    push    rbx                 ; RSP = user stack
    push    0x202               ; RFLAGS: IF=1, reserved bit 1 set
    push    0x23                ; CS  = user code (0x20) | RPL3
    push    r12                 ; RIP = entry point
    iretq

; ------------------------------------------------------------------------------
; fork() child return path. The child's kernel stack carries a byte-copy of the
; parent's syscall frame; switch_context resumes the child with R12 = frame
; base. fork() returns 0 in the child.
; ------------------------------------------------------------------------------
align 16
fork_ret_trampoline:
    xor     eax, eax
    mov     rsp, r12
    jmp     syscall_epilogue

; ------------------------------------------------------------------------------
section .data
align 8
syscall_user_rsp:   dq 0        ; stashed user RSP across one syscall

section .note.GNU-stack noalloc noexec nowrite progbits
