; ==============================================================================
; fakeos - x86-64 GDT / IDT / TSS loading, MSR access and unified ISR stubs
; ==============================================================================

[bits 64]
default rel

extern gdt
extern idt
extern isr_dispatch

global gdt_flush
global idt_load
global tss_load
global msr_read
global msr_write
global cr2_read
global rflags_read
global tlb_flush
global cr3_read
global cr3_write
global invlpg_page
global isr_stub_table
global isr_stub_255

global exc_de_trigger
global exc_de_recover
global exc_ud_trigger
global exc_ud_recover
global exc_pf_trigger
global exc_pf_recover

section .text

; ------------------------------------------------------------------------------
; Table-system register loading
; ------------------------------------------------------------------------------

; void gdt_flush(void)
; Loads the GDT whose pseudo-descriptor (gdtr) references the C symbol `gdt`,
; then atomically reloads CS via a 64-bit far return and refreshes data segs.
gdt_flush:
    lgdt [rel gdtr]
    push    0x08                    ; Kernel 64-bit code selector
    lea     rax, [rel .reload_cs]
    push    rax
    retfq                           ; REX.W far return: pops RIP then 16-bit CS
.reload_cs:
    mov     ax, 0x10                ; Kernel data selector
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    ret

; void idt_load(void)
idt_load:
    lidt [rel idtr]
    ret

; void tss_load(uint16_t selector)  RDI = selector
tss_load:
    ltr     di
    ret

; ------------------------------------------------------------------------------
; Model Specific Registers
; ------------------------------------------------------------------------------

; uint64_t msr_read(uint32_t msr)  RDI = MSR number
msr_read:
    mov     ecx, edi
    rdmsr
    shl     rdx, 32
    or      rax, rdx
    ret

; void msr_write(uint32_t msr, uint64_t value)  RDI = MSR, RSI = value
msr_write:
    mov     ecx, edi
    mov     eax, esi
    mov     rdx, rsi
    shr     rdx, 32
    wrmsr
    ret

; uint64_t cr2_read(void)
cr2_read:
    mov     rax, cr2
    ret

; uint64_t rflags_read(void)
rflags_read:
    pushfq
    pop     rax
    ret

; void tlb_flush(void) - reload CR3 to invalidate all non-global TLB entries
tlb_flush:
    mov     rax, cr3
    mov     cr3, rax
    ret

; uint64_t cr3_read(void)
cr3_read:
    mov     rax, cr3
    ret

; void cr3_write(uint64_t value)  RDI = new CR3
cr3_write:
    mov     cr3, rdi
    ret

; void invlpg_page(uint64_t vaddr)  RDI = virtual address
invlpg_page:
    invlpg  [rdi]
    ret

; ------------------------------------------------------------------------------
; Exception self-test triggers (called from C). Each label `*_recover` is the
; RIP that the C handler redirects execution to in order to survive the fault.
; ------------------------------------------------------------------------------

; #DE Divide Error: div by zero
exc_de_trigger:
    xor     ecx, ecx
    mov     rax, 1
    xor     edx, edx
    div     rcx
exc_de_recover:
    ret

; #UD Undefined Opcode
exc_ud_trigger:
    ud2
exc_ud_recover:
    ret

; #PF Page Fault: write to 1 GiB, the first byte above the boot identity /
; HHDM window (which covers only 0..1 GiB - 1), so no PDPT entry exists.
exc_pf_trigger:
    mov     rax, 0x0000000040000000
    mov     [rax], rax
exc_pf_recover:
    ret

; ------------------------------------------------------------------------------
; Interrupt service stub generation
;
; Stubs normalize the CPU interrupt frame so every vector supplies a synthetic
; error code: vectors whose CPU pushes a hardware error code keep it, all
; others push zero. The uniform stack layout (pushed top -> bottom):
;
;   [vector#, error_code, RIP, CS, RFLAGS, RSP, SS] + 15 general registers
;
; ------------------------------------------------------------------------------

%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push    0
    push    %1
    jmp     isr_common
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push    %1
    jmp     isr_common
%endmacro

; CPU pushes a real error code for vectors: 8,10,11,12,13,14,17,21
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
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

; External interrupt slots 32..47 (32 = Local APIC timer)
%assign vec 32
%rep 16
ISR_NOERR vec
%assign vec vec + 1
%endrep

; Vector 255: Local APIC spurious interrupt (must NOT issue EOI)
ISR_NOERR 255

; uint64_t isr_stub_table[48] - entry addresses for vectors 0..47
align 8
isr_stub_table:
%assign vec 0
%rep 48
    dq      isr_stub_ %+ vec
%assign vec vec + 1
%endrep

; Common dispatch path: save the full general-purpose register file, pass a
; pointer to the trap frame in RDI (SysV AMD64 first argument), call the C
; dispatcher, then restore everything and return from the interrupt.
align 16
isr_common:
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    mov     rdi, rsp                ; struct trap_frame *frame
    cld
    call    isr_dispatch

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
    add     rsp, 16                 ; Discard vector number and error code
    iretq

; ------------------------------------------------------------------------------
; Pseudo-descriptors (reference tables defined in C)
; ------------------------------------------------------------------------------
section .rodata
align 4
gdtr:
    dw      7 * 8 - 1               ; 7 descriptors x 8 bytes, minus 1
    dq      gdt

idtr:
    dw      256 * 16 - 1            ; 256 IDT gates x 16 bytes, minus 1
    dq      idt

section .note.GNU-stack noalloc noexec nowrite progbits
