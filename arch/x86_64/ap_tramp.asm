; ==============================================================================
; fakeos - AP SIPI trampoline (flat binary, physical load address 0x8000)
; SIPI vector 0x08 => CS:IP = 0x0800:0000. BSP copies this blob via HHDM and
; fills tramp_cr3 / tramp_stack / tramp_entry / tramp_cpu before each SIPI.
; Requires kernel PML4[0] identity (boot_pdpt) restored for the long-mode
; transition; AP then jumps to high-half ap_main and never returns.
; ==============================================================================

ORG 0x8000

BITS 16
cli
cld
xor     ax, ax
mov     ds, ax
mov     es, ax
mov     ss, ax
mov     sp, 0x7C00

lgdt    [tramp_gdtr]

mov     eax, cr0
or      eax, 1
mov     cr0, eax
jmp     0x08:pm32

BITS 32
pm32:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    ; CR4.PAE + SSE (OSFXSR|OSXMMEXCPT) — ISR saves XMM
    mov     eax, cr4
    or      eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov     cr4, eax

    ; CR3 = shared kernel PML4 (physical)
    mov     eax, [tramp_cr3]
    mov     cr3, eax

    ; IA32_EFER.LME
    mov     ecx, 0xC0000080
    rdmsr
    or      eax, (1 << 8)
    wrmsr

    ; CR0.PG|WP|MP|PE, clear EM so SSE executes (match boot.asm)
    mov     eax, cr0
    and     eax, ~(1 << 2)
    or      eax, 0x80010003
    mov     cr0, eax

    jmp     0x18:lm64

BITS 64
lm64:
    mov     ax, 0x20
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    mov     rsp, [tramp_stack]
    xor     ebp, ebp
    mov     edi, [tramp_cpu]          ; arg0 = logical CPU index
    mov     rax, [tramp_entry]
    jmp     rax

align 16
tramp_gdt:
    dq 0
    dq 0x00CF9A000000FFFF             ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF             ; 0x10: 32-bit data
    dq 0x00AF9A000000FFFF             ; 0x18: 64-bit code (L=1)
    dq 0x00CF92000000FFFF             ; 0x20: data
tramp_gdt_end:

align 16
tramp_gdtr:
    dw tramp_gdt_end - tramp_gdt - 1
    dd tramp_gdt

; BSP-written fields at fixed file offsets 0xC8.. (smp.c OFF_*)
times (0xC8 - ($ - $$)) db 0
tramp_cr3:      dq 0
tramp_stack:    dq 0
tramp_entry:    dq 0
tramp_cpu:      dd 0
tramp_done:     dd 0
