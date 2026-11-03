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
global u_unlink
global u_rmdir
global u_pipe
global u_sync
global u_ping
global u_socket
global u_bind
global u_connect
global u_sendto
global u_recvfrom
global u_listen
global u_accept
global u_kill
global u_tty_setfg
global u_rt_sigaction

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

; long u_unlink(const char *path)
USYSC1 u_unlink, 87

; long u_rmdir(const char *path)
USYSC1 u_rmdir, 84

; long u_pipe(int *pipefd)
USYSC1 u_pipe, 22

; long u_sync(void)
USYSC1 u_sync, 162

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

; long u_tty_setfg(long pid)
USYSC1 u_tty_setfg, 501

; long u_rt_sigaction(sig, act, old, setsize) -- 4th arg RCX to R10
align 16
u_rt_sigaction:
    mov     r10, rcx
    mov     eax, 13
    syscall
    ret

; void u_exit(long code) - does not return
align 16
u_exit:
    mov     eax, 60
    syscall
.hang:
    hlt
    jmp     .hang

section .note.GNU-stack noalloc noexec nowrite progbits
