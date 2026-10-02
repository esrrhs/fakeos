; ==============================================================================
; fakeos - 64-bit x86-64 Higher-Half Kernel Bootstrap
; ==============================================================================

[bits 32]

; ------------------------------------------------------------------------------
; Multiboot 1 & Multiboot 2 Headers
; ------------------------------------------------------------------------------
section .multiboot
align 8

; Multiboot 1 Header
MB1_MAGIC       equ 0x1BADB002
MB1_FLAGS       equ 0x00000003  ; Page-aligned modules + memory info
MB1_CHECKSUM    equ -(MB1_MAGIC + MB1_FLAGS)

align 4
dd MB1_MAGIC
dd MB1_FLAGS
dd MB1_CHECKSUM

; Multiboot 2 Header
MB2_MAGIC       equ 0xE85250D6
MB2_ARCH        equ 0           ; i386 32-bit protected mode
MB2_LENGTH      equ (mb2_header_end - mb2_header_start)
MB2_CHECKSUM    equ -(MB2_MAGIC + MB2_ARCH + MB2_LENGTH)

align 8
mb2_header_start:
dd MB2_MAGIC
dd MB2_ARCH
dd MB2_LENGTH
dd MB2_CHECKSUM

; Multiboot 2 End Tag
dw 0    ; type = 0
dw 0    ; flags = 0
dd 8    ; size = 8
mb2_header_end:

; ------------------------------------------------------------------------------
; 32-Bit Protected Mode Entry Point
; ------------------------------------------------------------------------------
section .boot.text
global _start
extern kmain

_start:
    cli                         ; Disable hardware interrupts

    ; Save Multiboot boot information passed by bootloader
    ; EAX = Multiboot magic (0x2BADB002 for MB1, 0x36D76289 for MB2)
    ; EBX = Multiboot info physical address
    mov [mb_magic], eax
    mov [mb_info], ebx

    ; --------------------------------------------------------------------------
    ; Setup Early 4-Level Page Tables for 64-bit Long Mode
    ; PML4 -> PDPT -> PD (2MB huge pages, full 1 GiB coverage)
    ;
    ; The single boot_pd maps the first 1 GiB of physical memory and is shared
    ; by three virtual windows through one PDPT:
    ;   PML4[0]   -> boot_pdpt : temporary identity window (removed by the PMM
    ;                             once all low-address accesses are gone)
    ;   PML4[256] -> boot_pdpt : HHDM at 0xFFFF800000000000 (kernel-managed
    ;                             direct map of all physical memory)
    ;   PML4[511] -> boot_pdpt : higher-half kernel window 0xFFFFFFFF80000000
    ;                             (uses PDPT slot 510, same PD)
    ; --------------------------------------------------------------------------

    ; PML4 slot targets (physical frame address + Present | Writable)
    mov eax, boot_pdpt
    or eax, 0x03                ; Present | Writable
    mov [boot_pml4], eax            ; PML4[0]   identity (transitional)
    mov [boot_pml4 + 256 * 8], eax  ; PML4[256] HHDM 0xFFFF800000000000
    mov [boot_pml4 + 511 * 8], eax  ; PML4[511] kernel 0xFFFFFFFF80000000

    ; PDPT[0]   -> boot_pd (0 - 1GB identity / HHDM window)
    ; PDPT[510] -> boot_pd (0xFFFFFFFF80000000 corresponds to PDPT index 510)
    mov eax, boot_pd
    or eax, 0x03                ; Present | Writable
    mov [boot_pdpt], eax
    mov [boot_pdpt + 510 * 8], eax

    ; Map the full first 1 GiB (512 x 2MB huge pages) into boot_pd.
    ; 0x83 = Present (bit 0) | Writable (bit 1) | Huge Page 2MB (bit 7)
    mov ecx, 0
.map_pages:
    mov eax, 0x200000           ; 2MB per page
    mul ecx
    or eax, 0x83                ; Flags: Present | Writable | Huge (2MB)
    mov [boot_pd + ecx * 8], eax
    mov dword [boot_pd + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512                ; 512 * 2MB = 1 GiB mapped
    jne .map_pages

    ; Load CR3 with physical address of PML4
    mov eax, boot_pml4
    mov cr3, eax

    ; Enable PAE (bit 5) and SSE: OSFXSR (bit 9), OSXMMEXCPT (bit 10) in CR4
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    ; Enable Long Mode (LME) in IA32_EFER MSR (0xC0000080, bit 8)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; Configure CR0: Enable Paging (PG, bit 31), WP (bit 16), MP (bit 1), PE (bit 0)
    ; Clear EM (bit 2) so SSE instructions execute natively
    mov eax, cr0
    and eax, ~(1 << 2)          ; Clear EM
    or eax, 0x80010003          ; PG | WP | MP | PE
    mov cr0, eax

    ; Load 64-bit Global Descriptor Table
    lgdt [gdt64_ptr]

    ; Far jump to 64-bit code in lower identity-mapped memory
    jmp 0x08:long_mode_entry_lower

; ------------------------------------------------------------------------------
; 64-Bit Transition Stub (Lower Identity Space)
; ------------------------------------------------------------------------------
[bits 64]
long_mode_entry_lower:
    ; Reload segment registers with 64-bit data segment selector (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far absolute jump to higher-half kernel entry point!
    mov rax, long_mode_higher_half
    jmp rax

; ------------------------------------------------------------------------------
; 64-Bit Kernel Entry Point (Higher-Half Address Space: 0xFFFFFFFF8010xxxx)
; ------------------------------------------------------------------------------
section .text
[bits 64]
default rel

long_mode_higher_half:
    ; Set 64-bit kernel stack
    mov rsp, stack_top

    ; Pass boot parameters to kmain(uint64_t magic, uint64_t mbi_addr)
    ; SysV AMD64 ABI: 1st argument = RDI, 2nd argument = RSI.
    ; mb_magic/mb_info live in .boot.bss at low physical addresses (~1 MB),
    ; which are NOT within RIP-relative range of this high-half code, so use
    ; absolute disp32 addressing (reachable through the identity mapping).
    default abs
    mov edi, [mb_magic]
    mov esi, [mb_info]
    default rel

    ; Call the C kernel entry point
    call kmain

    ; Halt the processor if kmain returns
    cli
.hang:
    hlt
    jmp .hang

; ------------------------------------------------------------------------------
; Bootstrap Data (GDT & Pointers in Lower Physical Memory)
; ------------------------------------------------------------------------------
section .boot.data
align 16
gdt64:
    dq 0                        ; Null descriptor
    dq 0x00209A0000000000       ; 64-bit Code descriptor (L=1, D=0, Exec/Read, Ring 0)
    dq 0x0000920000000000       ; 64-bit Data descriptor (Writable, Ring 0)
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1    ; Limit (16-bit)
    dd gdt64                    ; Base address (32-bit physical)

; ------------------------------------------------------------------------------
; Bootstrap BSS (Page Tables and Bootloader State in Lower Physical Memory)
; ------------------------------------------------------------------------------
section .boot.bss nobits alloc write
alignb 4
mb_magic:
    resd 1
mb_info:
    resd 1

alignb 4096
global boot_pml4
boot_pml4:
    resb 4096
global boot_pdpt
boot_pdpt:
    resb 4096
global boot_pd
boot_pd:
    resb 4096

; ------------------------------------------------------------------------------
; 64-bit Kernel BSS (Allocated in Higher-Half Memory)
; ------------------------------------------------------------------------------
section .bss nobits alloc write
alignb 16
stack_bottom:
    resb 65536                  ; 64 KB initial kernel stack
stack_top:

; Non-executable stack note to suppress GNU ld warnings
section .note.GNU-stack noalloc noexec nowrite progbits
