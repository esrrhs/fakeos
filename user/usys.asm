; ==============================================================================
; fakeos userland - raw SYSCALL/SYSRET wrappers (SysV AMD64 args, nr in RAX)
; ==============================================================================

[bits 64]
default rel

global u_read
global u_write
global u_open
global u_close
global u_dup2
global u_lseek
global u_mmap
global u_munmap
global u_mprotect
global u_brk
global u_exit
global u_getpid
global u_yield
global u_fork
global u_execve
global u_wait4
global u_chdir
global u_mkdir
global u_getcwd
global u_getdents
global u_rename
global u_unlink
global u_rmdir
global u_pipe
global u_sync
global u_fsync
global u_ping
global u_socket
global u_bind
global u_connect
global u_sendto
global u_recvfrom
global u_listen
global u_accept
global u_kill
global u_setpgid
global u_getpgid
global u_setsid
global u_getsid
global u_shutdown
global u_getsockname
global u_getpeername
global u_tty_setfg
global u_tcsetpgrp
global u_tcgetpgrp
global u_setsockopt
global u_getsockopt
global u_select
global u_poll
global u_ioctl
global u_rt_sigaction
global u_rt_sigreturn
global u_sig_restorer
global u_ftruncate
global u_stat
global u_fstat
global u_lstat
global u_newfstatat
global u_futex
global u_gettid
global u_clone
global u_arch_prctl
global u_set_tid_address

section .text

; Syscall ABI: number in RAX, args in RDI, RSI, RDX, R10, R8, R9.
; RCX/R11 are destroyed by the CPU (return RIP/RFLAGS); RCX is therefore
; unusable for the 4th argument, which the kernel reads from R10 instead.

%macro USYSC1 2
align 16
%1:
    mov     eax, %2
    syscall
    ret
%endmacro

; long u_read(long fd, void *buf, unsigned long len)
USYSC1 u_read, 0

; long u_write(long fd, const void *buf, unsigned long len)
USYSC1 u_write, 1

; long u_open(const char *path, long flags, long mode)
USYSC1 u_open, 2

; long u_close(long fd)
USYSC1 u_close, 3

; long u_dup2(long oldfd, long newfd)
USYSC1 u_dup2, 33

; long u_lseek(long fd, long off, long whence)
USYSC1 u_lseek, 8

; long u_mmap(addr,len,prot,flags,fd,off) - 4th C arg arrives in RCX, which
; SYSCALL overwrites with the return RIP; move it to R10 (kernel's 4th slot).
align 16
u_mmap:
    mov     r10, rcx
    mov     eax, 9
    syscall
    ret

; long u_mprotect(void *addr, unsigned long len, long prot) -- Linux nr 10
USYSC1 u_mprotect, 10

; long u_munmap(void *addr, unsigned long len) -- Linux nr 11
USYSC1 u_munmap, 11

; unsigned long u_brk(unsigned long addr)
USYSC1 u_brk, 12

; long u_getpid(void)
USYSC1 u_getpid, 39

; long u_yield(void)
USYSC1 u_yield, 24

; long u_fork(void)
USYSC1 u_fork, 57

; long u_execve(const char *path, char **argv, char **envp)
USYSC1 u_execve, 59

; long u_wait4(long pid, long *status, long options, long *rusage)
USYSC1 u_wait4, 61

; long u_chdir(const char *path)
USYSC1 u_chdir, 80

; long u_mkdir(const char *path)
USYSC1 u_mkdir, 83

; long u_getcwd(char *buf, unsigned long len)
USYSC1 u_getcwd, 79

; long u_getdents(long fd, void *buf, unsigned long len)
USYSC1 u_getdents, 217

; long u_rename(const char *oldpath, const char *newpath)
USYSC1 u_rename, 82

; long u_unlink(const char *path)
USYSC1 u_unlink, 87

; long u_rmdir(const char *path)
USYSC1 u_rmdir, 84

; long u_pipe(int *pipefd)
USYSC1 u_pipe, 22

; long u_sync(void)
USYSC1 u_sync, 162

; long u_fsync(long fd)
USYSC1 u_fsync, 74

; long u_ping(unsigned int ip_host_order)
USYSC1 u_ping, 500

; long u_socket(long domain, long type, long protocol)
USYSC1 u_socket, 41

; long u_bind(long fd, const void *addr, unsigned long len)
USYSC1 u_bind, 49

; long u_connect(long fd, const void *addr, unsigned long len)
USYSC1 u_connect, 42

; long u_sendto(fd,buf,len,flags,addr,addrlen) - 4th C arg RCX -> R10
align 16
u_sendto:
    mov     r10, rcx
    mov     eax, 44
    syscall
    ret

; long u_recvfrom(fd,buf,len,flags,addr,addrlen)
align 16
u_recvfrom:
    mov     r10, rcx
    mov     eax, 45
    syscall
    ret

; long u_listen(long fd, long backlog)
USYSC1 u_listen, 50

; long u_accept(long fd, void *addr, unsigned int *alen)
USYSC1 u_accept, 43

; long u_kill(long pid, long sig)
USYSC1 u_kill, 62

; long u_setpgid(long pid, long pgid)
USYSC1 u_setpgid, 109

; long u_getpgid(long pid)
USYSC1 u_getpgid, 121

; long u_setsid(void)
USYSC1 u_setsid, 112

; long u_getsid(long pid)
USYSC1 u_getsid, 124

; long u_shutdown(long fd, long how)
USYSC1 u_shutdown, 48

; long u_getsockname(long fd, void *addr, unsigned int *alen)
USYSC1 u_getsockname, 51

; long u_getpeername(long fd, void *addr, unsigned int *alen)
USYSC1 u_getpeername, 52

; long u_tty_setfg(long pgid) - alias of tcsetpgrp (fg pgid; 0 clears)
USYSC1 u_tty_setfg, 501

; long u_tcsetpgrp(long fd, long pgid) - fd ignored (single console); pgid in RSI
; Linux uses ioctl; we keep custom nr 501 with pgid in RDI for tty_setfg
; and also accept (fd,pgid) via this wrapper that moves RSI->RDI.
align 16
u_tcsetpgrp:
    mov     rdi, rsi
    mov     eax, 501
    syscall
    ret

; long u_tcgetpgrp(long fd) - returns fg pgid (custom nr 502)
USYSC1 u_tcgetpgrp, 502

; long u_setsockopt(fd,level,optname,optval,optlen) - 4th RCX -> R10
align 16
u_setsockopt:
    mov     r10, rcx
    mov     eax, 54
    syscall
    ret

; long u_getsockopt(fd,level,optname,optval,optlen)
align 16
u_getsockopt:
    mov     r10, rcx
    mov     eax, 55
    syscall
    ret

; long u_select(nfds, readfds, writefds, exceptfds, timeout)
align 16
u_select:
    mov     r10, rcx
    mov     eax, 23
    syscall
    ret

; long u_poll(struct pollfd *fds, unsigned long nfds, long timeout_ms)
align 16
u_poll:
    mov     eax, 7
    syscall
    ret

; long u_ioctl(long fd, unsigned long req, unsigned long arg)
align 16
u_ioctl:
    mov     eax, 16
    syscall
    ret

; long u_rt_sigaction(sig, act, old, setsize) -- 4th arg RCX to R10
align 16
u_rt_sigaction:
    mov     r10, rcx
    mov     eax, 13
    syscall
    ret


; Signal trampoline: installed as sa_restorer. After the handler `ret`s here,
; RSP points at sigframe[1]; do rt_sigreturn with no nested call so the kernel
; can find the frame at RSP-8.
align 16
u_sig_restorer:
    mov     eax, 15
    syscall
    ud2

; long u_rt_sigreturn(void) -- Linux nr 15
align 16
u_rt_sigreturn:
    mov     eax, 15
    syscall
    ret

; long u_ftruncate(long fd, unsigned long length)
USYSC1 u_ftruncate, 77

; long u_stat(const char *path, void *statbuf) -- Linux nr 4
USYSC1 u_stat, 4

; long u_fstat(long fd, void *statbuf) -- Linux nr 5
USYSC1 u_fstat, 5

; long u_lstat(const char *path, void *statbuf) -- Linux nr 6 (no symlinks)
USYSC1 u_lstat, 6

; long u_newfstatat(long dfd, const char *path, void *statbuf, long flags)
; 4th C arg RCX -> R10
align 16
u_newfstatat:
    mov     r10, rcx
    mov     eax, 262
    syscall
    ret

; long u_gettid(void)
USYSC1 u_gettid, 186

; long u_clone(unsigned long flags, void *stack, void *fn, void *arg)
; 4th C arg is RCX; SYSCALL clobbers RCX with return RIP → move to R10.
align 16
u_clone:
    mov     r10, rcx
    mov     eax, 56
    syscall
    ret

; long u_futex(uaddr, op, val, timeout, uaddr2, val3) -- 4th C arg RCX -> R10
align 16
u_futex:
    mov     r10, rcx
    mov     eax, 202
    syscall
    ret


; long u_arch_prctl(long code, unsigned long addr) -- Linux nr 158
USYSC1 u_arch_prctl, 158

; long u_set_tid_address(int *tidptr) -- Linux nr 218
USYSC1 u_set_tid_address, 218

; void u_exit(long code) - does not return
align 16
u_exit:
    mov     eax, 60
    syscall
.hang:
    hlt
    jmp     .hang

; ---- userland atomics (for pthread_mutex on futex) -----------------------
; unsigned int atomic_xchg(volatile unsigned int *p [RDI], unsigned int v [ESI])
; returns previous *p
global atomic_xchg
align 16
atomic_xchg:
    mov     eax, esi
    xchg    dword [rdi], eax
    ret

; unsigned int atomic_cmpxchg(volatile unsigned int *p [RDI],
;                             unsigned int expected [ESI],
;                             unsigned int desired [EDX])
; returns previous *p (compare-and-swap with LOCK CMPXCHG)
global atomic_cmpxchg
align 16
atomic_cmpxchg:
    mov     eax, esi
    lock cmpxchg dword [rdi], edx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
