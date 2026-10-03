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

; Trap frame layout (grows down from the per-thread kernel stack top T):
;
;   T-264 (B): 8-byte user-RSP pad + 256 bytes saved XMM0-15 ([B+8..B+264))
;   T-384 (F): 128-byte GPR frame (struct syscall_frame), pad at F+120
;
; XMM must survive syscalls: kernel C handlers emit SSE spills, and the user
; compiler keeps aggregate temporaries in vector registers around its raw
; __syscall sites. The user RSP is stashed in a global conduit (SFMASK clears
; IF on entry); the syscall number stays in RAX through the pushes and is
; pushed last, so the conduit reload (which clobbers RAX) happens only after
; the number is safely in its frame slot. fork_ret_trampoline lands on this
; identical 392-byte layout.
align 16
syscall_entry:
    mov     [rel syscall_user_rsp], rsp
    mov     rsp, [rel syscall_kstack_top]
    sub     rsp, 264            ; 256 XMM bytes + 8 user-RSP pad (16-aligned)
    movdqa  [rsp + 0x08], xmm0
    movdqa  [rsp + 0x18], xmm1
    movdqa  [rsp + 0x28], xmm2
    movdqa  [rsp + 0x38], xmm3
    movdqa  [rsp + 0x48], xmm4
    movdqa  [rsp + 0x58], xmm5
    movdqa  [rsp + 0x68], xmm6
    movdqa  [rsp + 0x78], xmm7
    movdqa  [rsp + 0x88], xmm8
    movdqa  [rsp + 0x98], xmm9
    movdqa  [rsp + 0xa8], xmm10
    movdqa  [rsp + 0xb8], xmm11
    movdqa  [rsp + 0xc8], xmm12
    movdqa  [rsp + 0xd8], xmm13
    movdqa  [rsp + 0xe8], xmm14
    movdqa  [rsp + 0xf8], xmm15
    push    r11                 ; frame +112 user RFLAGS
    push    rcx                 ; frame +104 user RIP
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
    push    rax                 ; frame +0 syscall number (RAX still intact)
    mov     rax, [rel syscall_user_rsp]
    mov     [rsp + 120], rax    ; frame +120 pad = user RSP
    mov     rdi, rsp            ; struct syscall_frame *
    cld
    call    syscall_dispatch    ; return value in RAX

; Common return path: RAX = syscall return value, RSP = frame base.
; (fork_ret_trampoline joins here on the child's copied frame.)
global syscall_epilogue
syscall_epilogue:
    add     rsp, 8              ; drop syscall-number slot (retval in RAX)
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
    movdqa  xmm0,  [rsp + 0x08]
    movdqa  xmm1,  [rsp + 0x18]
    movdqa  xmm2,  [rsp + 0x28]
    movdqa  xmm3,  [rsp + 0x38]
    movdqa  xmm4,  [rsp + 0x48]
    movdqa  xmm5,  [rsp + 0x58]
    movdqa  xmm6,  [rsp + 0x68]
    movdqa  xmm7,  [rsp + 0x78]
    movdqa  xmm8,  [rsp + 0x88]
    movdqa  xmm9,  [rsp + 0x98]
    movdqa  xmm10, [rsp + 0xa8]
    movdqa  xmm11, [rsp + 0xb8]
    movdqa  xmm12, [rsp + 0xc8]
    movdqa  xmm13, [rsp + 0xd8]
    movdqa  xmm14, [rsp + 0xe8]
    movdqa  xmm15, [rsp + 0xf8]
    mov     rsp, [rsp]          ; pad holds THIS thread's user RSP
    ; NOTE: nasm does not recognize the "sysretq" mnemonic; encode by hand.
    db      0x48, 0x0F, 0x07    ; 64-bit SYSRET (RIP=RCX, RFLAGS=R11)

; ------------------------------------------------------------------------------
; First entry into ring 3 for a freshly created user thread. On resume the
; scheduler switch frame delivers R12 = user entry RIP, RBX = user RSP.
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
; fork() child return path. The child's kernel stack carries a byte-copy of
; the full 392-byte trap frame (GPRs + pad + XMM); switch resumes with
; R12 = frame base. fork() returns 0 in the child.
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
