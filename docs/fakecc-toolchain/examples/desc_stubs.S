/* gas port sketch of fakeos arch/x86_64/desc.asm (S3).
 * ISR stubs (error / no-error), isr_common (GPR+XMM), isr_stub_table,
 * gdt_flush / idt_load / tss_load, MSR/CR/invlpg helpers, exc_* triggers.
 *
 * Gap vs nasm: no %assign-style counters inside .rept — vectors are listed
 * explicitly (or via .macro with a numeric arg). Vector 255 is included;
 * table covers 0..47 only (same as fakeos).
 * Assemble: fakecc -c docs/fakecc-toolchain/examples/desc_stubs.S -o desc_stubs.o
 */
    .text
    .globl gdt_flush, idt_load, tss_load
    .globl msr_read, msr_write, cr2_read, rflags_read
    .globl tlb_flush, cr3_read, cr3_write, invlpg_page
    .globl isr_stub_table, isr_stub_255, isr_common
    .globl exc_de_trigger, exc_de_recover
    .globl exc_ud_trigger, exc_ud_recover
    .globl exc_pf_trigger, exc_pf_recover
    .extern gdt
    .extern idt
    .extern isr_dispatch

    .macro ISR_NOERR n
    .globl isr_stub_\n
isr_stub_\n:
    push $0
    push $\n
    jmp isr_common
    .endm

    .macro ISR_ERR n
    .globl isr_stub_\n
isr_stub_\n:
    push $\n
    jmp isr_common
    .endm

    ISR_NOERR 0
    ISR_NOERR 1
    ISR_NOERR 2
    ISR_NOERR 3
    ISR_NOERR 4
    ISR_NOERR 5
    ISR_NOERR 6
    ISR_NOERR 7
    ISR_ERR 8
    ISR_NOERR 9
    ISR_ERR 10
    ISR_ERR 11
    ISR_ERR 12
    ISR_ERR 13
    ISR_ERR 14
    ISR_NOERR 15
    ISR_NOERR 16
    ISR_ERR 17
    ISR_NOERR 18
    ISR_NOERR 19
    ISR_NOERR 20
    ISR_ERR 21
    ISR_NOERR 22
    ISR_NOERR 23
    ISR_NOERR 24
    ISR_NOERR 25
    ISR_NOERR 26
    ISR_NOERR 27
    ISR_NOERR 28
    ISR_NOERR 29
    ISR_NOERR 30
    ISR_NOERR 31
    ISR_NOERR 32
    ISR_NOERR 33
    ISR_NOERR 34
    ISR_NOERR 35
    ISR_NOERR 36
    ISR_NOERR 37
    ISR_NOERR 38
    ISR_NOERR 39
    ISR_NOERR 40
    ISR_NOERR 41
    ISR_NOERR 42
    ISR_NOERR 43
    ISR_NOERR 44
    ISR_NOERR 45
    ISR_NOERR 46
    ISR_NOERR 47
    ISR_NOERR 255

    .align 16
isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    sub $256, %rsp
    movdqa %xmm0,  0x00(%rsp)
    movdqa %xmm1,  0x10(%rsp)
    movdqa %xmm2,  0x20(%rsp)
    movdqa %xmm3,  0x30(%rsp)
    movdqa %xmm4,  0x40(%rsp)
    movdqa %xmm5,  0x50(%rsp)
    movdqa %xmm6,  0x60(%rsp)
    movdqa %xmm7,  0x70(%rsp)
    movdqa %xmm8,  0x80(%rsp)
    movdqa %xmm9,  0x90(%rsp)
    movdqa %xmm10, 0xa0(%rsp)
    movdqa %xmm11, 0xb0(%rsp)
    movdqa %xmm12, 0xc0(%rsp)
    movdqa %xmm13, 0xd0(%rsp)
    movdqa %xmm14, 0xe0(%rsp)
    movdqa %xmm15, 0xf0(%rsp)
    mov %rsp, %rdi
    add $256, %rdi
    cld
    call isr_dispatch
    movdqa 0x00(%rsp), %xmm0
    movdqa 0x10(%rsp), %xmm1
    movdqa 0x20(%rsp), %xmm2
    movdqa 0x30(%rsp), %xmm3
    movdqa 0x40(%rsp), %xmm4
    movdqa 0x50(%rsp), %xmm5
    movdqa 0x60(%rsp), %xmm6
    movdqa 0x70(%rsp), %xmm7
    movdqa 0x80(%rsp), %xmm8
    movdqa 0x90(%rsp), %xmm9
    movdqa 0xa0(%rsp), %xmm10
    movdqa 0xb0(%rsp), %xmm11
    movdqa 0xc0(%rsp), %xmm12
    movdqa 0xd0(%rsp), %xmm13
    movdqa 0xe0(%rsp), %xmm14
    movdqa 0xf0(%rsp), %xmm15
    add $256, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp
    iretq

gdt_flush:
    lgdt gdtr(%rip)
    push $0x08
    lea .reload_cs(%rip), %rax
    push %rax
    retfq
.reload_cs:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ret

idt_load:
    lidt idtr(%rip)
    ret

tss_load:
    ltr %di
    ret

msr_read:
    mov %edi, %ecx
    rdmsr
    shl $32, %rdx
    or %rdx, %rax
    ret

msr_write:
    mov %edi, %ecx
    mov %esi, %eax
    mov %rsi, %rdx
    shr $32, %rdx
    wrmsr
    ret

cr2_read:
    mov %cr2, %rax
    ret

rflags_read:
    pushfq
    pop %rax
    ret

tlb_flush:
    mov %cr3, %rax
    mov %rax, %cr3
    ret

cr3_read:
    mov %cr3, %rax
    ret

cr3_write:
    mov %rdi, %cr3
    ret

invlpg_page:
    invlpg (%rdi)
    ret

exc_de_trigger:
    xor %ecx, %ecx
    mov $1, %rax
    xor %edx, %edx
    div %rcx
exc_de_recover:
    ret

exc_ud_trigger:
    ud2
exc_ud_recover:
    ret

exc_pf_trigger:
    mov $0x0000000040000000, %rax
    mov %rax, (%rax)
exc_pf_recover:
    ret

    .data
    .align 8
isr_stub_table:

    .quad isr_stub_0
    .quad isr_stub_1
    .quad isr_stub_2
    .quad isr_stub_3
    .quad isr_stub_4
    .quad isr_stub_5
    .quad isr_stub_6
    .quad isr_stub_7
    .quad isr_stub_8
    .quad isr_stub_9
    .quad isr_stub_10
    .quad isr_stub_11
    .quad isr_stub_12
    .quad isr_stub_13
    .quad isr_stub_14
    .quad isr_stub_15
    .quad isr_stub_16
    .quad isr_stub_17
    .quad isr_stub_18
    .quad isr_stub_19
    .quad isr_stub_20
    .quad isr_stub_21
    .quad isr_stub_22
    .quad isr_stub_23
    .quad isr_stub_24
    .quad isr_stub_25
    .quad isr_stub_26
    .quad isr_stub_27
    .quad isr_stub_28
    .quad isr_stub_29
    .quad isr_stub_30
    .quad isr_stub_31
    .quad isr_stub_32
    .quad isr_stub_33
    .quad isr_stub_34
    .quad isr_stub_35
    .quad isr_stub_36
    .quad isr_stub_37
    .quad isr_stub_38
    .quad isr_stub_39
    .quad isr_stub_40
    .quad isr_stub_41
    .quad isr_stub_42
    .quad isr_stub_43
    .quad isr_stub_44
    .quad isr_stub_45
    .quad isr_stub_46
    .quad isr_stub_47

    .section .rodata
    .align 4
gdtr:
    .word 55
    .quad gdt
idtr:
    .word 4095
    .quad idt

