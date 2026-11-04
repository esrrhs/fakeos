; ==============================================================================
; fakeos - embedded rootfs images (built from user/ at user virtual addresses)
;
; Six static ELF user programs plus the /etc/motd text. rootfs_publish() in
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
BLOB user_cp,    "build/user/cp.elf"
BLOB user_rm,    "build/user/rm.elf"
BLOB user_touch, "build/user/touch.elf"
BLOB user_mv,    "build/user/mv.elf"
BLOB user_ping,  "build/user/ping.elf"
BLOB user_udptest, "build/user/udptest.elf"
BLOB user_nc,    "build/user/nc.elf"
BLOB user_ed,    "build/user/ed.elf"
BLOB user_login, "build/user/login.elf"
BLOB user_head,  "build/user/head.elf"
BLOB user_wc,    "build/user/wc.elf"
BLOB user_getent, "build/user/getent.elf"
BLOB user_tail,  "build/user/tail.elf"
BLOB user_grep,  "build/user/grep.elf"
BLOB user_printf,"build/user/printf.elf"
BLOB user_tee,   "build/user/tee.elf"
BLOB user_basename, "build/user/basename.elf"
BLOB user_dirname, "build/user/dirname.elf"
BLOB user_kbuild, "build/user/kbuild.elf"
BLOB user_ldfake, "build/user/ldfake.elf"
BLOB user_dumpbin, "build/user/dumpbin.elf"
BLOB motd,       "user/motd.txt"
BLOB environ,    "user/environ.txt"
BLOB resolv_conf,"user/resolv.conf"
BLOB hosts_file, "user/hosts"
BLOB passwd_file,"user/passwd"
BLOB group_file, "user/group"

section .note.GNU-stack noalloc noexec nowrite progbits
