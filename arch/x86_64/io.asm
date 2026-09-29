; ==============================================================================
; fakeos - Low-level x86-64 CPU and Port I/O Instructions
; ==============================================================================

[bits 64]
default rel

global outb
global inb
global outw
global inw
global outl
global inl
global io_wait
global cpu_halt
global cpu_cli
global cpu_sti

section .text

; void outb(uint16_t port, uint8_t val)
; RDI = port, RSI = val
outb:
    mov dx, di
    mov al, sil
    out dx, al
    ret

; uint8_t inb(uint16_t port)
; RDI = port
inb:
    mov dx, di
    xor eax, eax
    in al, dx
    ret

; void outw(uint16_t port, uint16_t val)
; RDI = port, RSI = val
outw:
    mov dx, di
    mov ax, si
    out dx, ax
    ret

; uint16_t inw(uint16_t port)
; RDI = port
inw:
    mov dx, di
    xor eax, eax
    in ax, dx
    ret

; void outl(uint16_t port, uint32_t val)
; RDI = port, RSI = val
outl:
    mov dx, di
    mov eax, esi
    out dx, eax
    ret

; uint32_t inl(uint16_t port)
; RDI = port
inl:
    mov dx, di
    in eax, dx
    ret

; void io_wait(void)
; Small delay for slow legacy I/O buses
io_wait:
    out 0x80, al
    ret

; void cpu_halt(void)
cpu_halt:
    hlt
    ret

; void cpu_cli(void)
cpu_cli:
    cli
    ret

; void cpu_sti(void)
cpu_sti:
    sti
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
