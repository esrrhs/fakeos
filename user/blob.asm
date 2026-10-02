; ==============================================================================
; fakeos - embedded userland image (built from user/ at user virtual addresses)
; ==============================================================================

[bits 64]

global user_elf_start
global user_elf_end

section .rodata

align 8
user_elf_start:
    incbin "build/user/init.elf"
user_elf_end:

section .note.GNU-stack noalloc noexec nowrite progbits
