; ==============================================================================
; fakeos - embedded rootfs images (built from user/ at user virtual addresses)
;
; Five static ELF user programs plus the /etc/motd text. rootfs_publish() in
; kernel/rootfs.c releases them into Ramfs at boot. All blobs live in the
; kernel .rodata and are mapped read-only into the higher-half image.
; ==============================================================================

[bits 64]

%macro BLOB 2
global %1%+_start
global %1%+_end
align 8
%1%+_start:
    incbin %2
%1%+_end:
%endmacro

section .rodata

BLOB user_init,  "build/user/init.elf"
BLOB user_sh,    "build/user/sh.elf"
BLOB user_hello, "build/user/hello.elf"
BLOB user_cat,   "build/user/cat.elf"
BLOB user_ls,    "build/user/ls.elf"
BLOB motd,       "user/motd.txt"

section .note.GNU-stack noalloc noexec nowrite progbits
