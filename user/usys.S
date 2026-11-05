/* fakeos userland SYSCALL wrappers (Gas AT&T for fakecc) */
    .text
    .globl atomic_cmpxchg, atomic_xchg, u_accept, u_arch_prctl, u_bind, u_brk, u_chdir, u_chmod
    .globl u_chown, u_clock_gettime, u_clone, u_close, u_connect, u_dup2, u_execve, u_exit
    .globl u_fork, u_fstat, u_fsync, u_ftruncate, u_futex, u_getcwd, u_getdents, u_getegid
    .globl u_geteuid, u_getgid, u_getgroups, u_getpeername, u_getpgid, u_getpid, u_getppid, u_getsid
    .globl u_getsockname, u_getsockopt, u_gettid, u_getuid, u_ioctl, u_kill, u_link, u_listen
    .globl u_lseek, u_lstat, u_mkdir, u_mmap, u_mprotect, u_munmap, u_nanosleep, u_newfstatat
    .globl u_nice, u_open, u_ping, u_pipe, u_poll, u_read, u_readlink, u_recvfrom
    .globl u_rename, u_rmdir, u_rt_sigaction, u_rt_sigreturn, u_select, u_sendto, u_set_tid_address, u_setgid
    .globl u_setgroups, u_setpgid, u_setregid, u_setreuid, u_setsid, u_setsockopt, u_setuid, u_shutdown
    .globl u_sig_restorer, u_socket, u_stat, u_statfs, u_symlink, u_sync, u_tcgetpgrp, u_tcsetpgrp, u_tty_setfg
    .globl u_umask, u_uname, u_unlink, u_utimensat, u_wait4, u_write, u_yield

    .macro USYSC1 name nr
    .globl \name
\name:
    mov $\nr, %eax
    syscall
    ret
    .endm

    USYSC1 u_read, 0
    USYSC1 u_write, 1
    USYSC1 u_open, 2
    USYSC1 u_close, 3
    USYSC1 u_dup2, 33
    USYSC1 u_lseek, 8
    USYSC1 u_mprotect, 10
    USYSC1 u_munmap, 11
    USYSC1 u_brk, 12
    USYSC1 u_getpid, 39
    USYSC1 u_getppid, 110
    USYSC1 u_uname, 63
    USYSC1 u_nanosleep, 35
    USYSC1 u_clock_gettime, 228
    USYSC1 u_nice, 503
    USYSC1 u_yield, 24
    USYSC1 u_fork, 57
    USYSC1 u_execve, 59
    USYSC1 u_wait4, 61
    USYSC1 u_chdir, 80
    USYSC1 u_mkdir, 83
    USYSC1 u_getcwd, 79
    USYSC1 u_getdents, 217
    USYSC1 u_rename, 82
    USYSC1 u_unlink, 87
    USYSC1 u_link, 86
    USYSC1 u_symlink, 88
    USYSC1 u_readlink, 89
    USYSC1 u_rmdir, 84
    USYSC1 u_pipe, 22
    USYSC1 u_sync, 162
    USYSC1 u_fsync, 74
    USYSC1 u_ping, 500
    USYSC1 u_socket, 41
    USYSC1 u_bind, 49
    USYSC1 u_connect, 42
    USYSC1 u_listen, 50
    USYSC1 u_accept, 43
    USYSC1 u_kill, 62
    USYSC1 u_setpgid, 109
    USYSC1 u_getpgid, 121
    USYSC1 u_setsid, 112
    USYSC1 u_getsid, 124
    USYSC1 u_shutdown, 48
    USYSC1 u_getsockname, 51
    USYSC1 u_getpeername, 52
    USYSC1 u_tty_setfg, 501
    USYSC1 u_tcgetpgrp, 502
    USYSC1 u_ftruncate, 77
    USYSC1 u_stat, 4
    USYSC1 u_fstat, 5
    USYSC1 u_lstat, 6
    USYSC1 u_chmod, 90
    USYSC1 u_umask, 95
    USYSC1 u_chown, 92
    USYSC1 u_getuid, 102
    USYSC1 u_geteuid, 107
    USYSC1 u_getgid, 104
    USYSC1 u_getegid, 108
    USYSC1 u_setuid, 105
    USYSC1 u_setgid, 106
    USYSC1 u_setreuid, 113
    USYSC1 u_setregid, 114
    USYSC1 u_getgroups, 115
    USYSC1 u_setgroups, 116
    USYSC1 u_gettid, 186
    USYSC1 u_arch_prctl, 158
    USYSC1 u_set_tid_address, 218


/* long u_mmap - 4th C arg RCX -> R10 */
u_mmap:
    mov %rcx, %r10
    mov $9, %eax
    syscall
    ret

u_sendto:
    mov %rcx, %r10
    mov $44, %eax
    syscall
    ret

u_recvfrom:
    mov %rcx, %r10
    mov $45, %eax
    syscall
    ret

u_tcsetpgrp:
    mov %rsi, %rdi
    mov $501, %eax
    syscall
    ret

u_setsockopt:
    mov %rcx, %r10
    mov $54, %eax
    syscall
    ret

u_getsockopt:
    mov %rcx, %r10
    mov $55, %eax
    syscall
    ret

u_select:
    mov %rcx, %r10
    mov $23, %eax
    syscall
    ret

u_poll:
    mov $7, %eax
    syscall
    ret

u_ioctl:
    mov $16, %eax
    syscall
    ret

u_rt_sigaction:
    mov %rcx, %r10
    mov $13, %eax
    syscall
    ret

u_sig_restorer:
    mov $15, %eax
    syscall
    ud2

u_rt_sigreturn:
    mov $15, %eax
    syscall
    ret

u_newfstatat:
    mov %rcx, %r10
    mov $262, %eax
    syscall
    ret

u_clone:
    mov %rcx, %r10
    mov $56, %eax
    syscall
    ret

u_futex:
    mov %rcx, %r10
    mov $202, %eax
    syscall
    ret


u_utimensat:
    mov %rcx, %r10
    mov $280, %eax
    syscall
    ret

u_statfs:
    mov $137, %eax
    syscall
    ret

u_exit:
    mov $60, %eax
    syscall
.hang:
    hlt
    jmp .hang

    .globl atomic_xchg, atomic_cmpxchg
atomic_xchg:
    mov %esi, %eax
    xchg %eax, (%rdi)
    ret

/* lock cmpxchg %edx, (%rdi) — fakecc has no lock/cmpxchg mnemonic yet */
atomic_cmpxchg:
    mov %esi, %eax
    .byte 0xF0, 0x0F, 0xB1, 0x17
    ret

