package kernel;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);
extern u64 msr_read(u32 msr);
extern void msr_write(u32 msr, u64 value);

/* sysentry.asm */
extern void syscall_entry(void);

/* sched.c */
extern u32 sched_current_pid(void);
extern u64 sched_current_as_h(void);
extern u32 sched_alloc_pid(void);
extern u32 sched_clone_user(u64 frame_base, u64 child_as_h, u32 child_pid);
extern void sched_yield(void);
extern u32 sched_fd_close_all(void);
extern u32 sched_current_ppid(void);
extern void sched_reparent_children(u32 old_ppid, u32 new_ppid);
extern void kthread_exit(void);
extern void sched_wakeup(u64 chan);
extern u64 sched_wait4_chan(u32 parent_pid);

/* sysfile.c */
extern u64 sys_open(u64 upath, u64 flags, u64 mode);
extern u64 sys_close(u64 fd);
extern u64 sys_read_fd(u64 fd, u64 buf, u64 len);
extern u64 sys_write_fd(u64 fd, u64 buf, u64 len);
extern u64 sys_lseek(u64 fd, u64 off, u64 whence);
extern u64 sys_mkdir(u64 upath);
extern u64 sys_unlink(u64 upath);
extern u64 sys_rmdir(u64 upath);
extern u64 sys_dup2(u64 oldfd, u64 newfd);
extern u64 sys_pipe(u64 upipefd);
extern u64 sys_sync(void);
extern u64 sys_fsync(u64 fd);
extern u64 sys_ping(u64 ip);
extern u64 sys_socket(u64 domain, u64 type, u64 protocol);
extern u64 sys_bind(u64 fd, u64 uaddr, u64 ulen);
extern u64 sys_connect(u64 fd, u64 uaddr, u64 ulen);
extern u64 sys_sendto(u64 fd, u64 ubuf, u64 len, u64 flags, u64 uaddr, u64 ulen);
extern u64 sys_recvfrom(u64 fd, u64 ubuf, u64 len, u64 flags, u64 uaddr, u64 ualen);
extern u64 sys_listen(u64 fd, u64 backlog);
extern u64 sys_accept(u64 fd, u64 uaddr, u64 ualen);
extern u64 sys_chdir(u64 upath);
extern u64 sys_getcwd(u64 buf, u64 len);
extern u64 sys_getdents64(u64 fd, u64 buf, u64 len);

/* sysproc.c */
extern u64 sys_brk(u64 newbrk);
extern u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off);
extern u64 sys_nice(u64 inc);
extern u64 sys_munmap(u64 addr, u64 len);
extern u64 sys_chmod(u64 path, u64 mode);
extern u64 sys_gettid(void);
extern u64 sys_futex(u64 uaddr, u64 op, u64 val, u64 timeout, u64 uaddr2, u64 val3);

/* proc.c */
extern void proc_record_exit(u32 pid, u32 ppid, u64 code);
extern void proc_reparent_exits(u32 old_ppid, u32 new_ppid);
extern u64 sys_wait4(u64 pid, u64 status, u64 options, u64 ruspace);
extern u64 sys_kill(u64 pid, u64 sig);
extern u64 sys_tty_setfg(u64 pgid);
extern u64 sys_tty_getfg(u64 fd);
extern u64 sys_setpgid(u64 pid, u64 pgid);
extern u64 sys_setsid(void);
extern u64 sys_getpgid(u64 pid);
extern u64 sys_getsid(u64 pid);
extern u64 sys_rt_sigaction(u64 sig, u64 act, u64 old, u64 setsize);
extern u64 sys_shutdown(u64 fd, u64 how);
extern u64 sys_getsockname(u64 fd, u64 uaddr, u64 ualen);
extern u64 sys_getpeername(u64 fd, u64 uaddr, u64 ualen);
extern u64 sys_setsockopt(u64 fd, u64 level, u64 optname, u64 optval, u64 optlen);
extern u64 sys_getsockopt(u64 fd, u64 level, u64 optname, u64 optval, u64 optlen);
extern u64 sys_select(u64 nfds, u64 readfds, u64 writefds, u64 exceptfds, u64 timeout);
extern u64 sys_poll(u64 ufds, u64 nfds, u64 timeout_ms);
extern u64 sys_ioctl(u64 fd, u64 req, u64 arg);
extern u32 sched_signal_send(u32 pid, u32 sig);
extern void sched_check_signals(void);
extern void sched_check_signals_frame(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi);
extern u32 sched_sigreturn(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi);

/* uproc.c */
extern void uproc_note_exit(u32 pid, u64 code);
extern u64 sys_execve(u64 path_u, u64 argv_u, u64 envp_u, u64 frame_p);

/* Frame built by syscall_entry in arch/x86_64/sysentry.asm (128 bytes). */
struct syscall_frame {
    u64 rax;                        /* syscall number                 */
    u64 r9;
    u64 r8;
    u64 r10;                        /* 4th argument (RCX is clobbered) */
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 rbp;
    u64 rbx;
    u64 rip;                        /* user return RIP (from RCX)     */
    u64 rflags;                     /* user RFLAGS (from R11)         */
    u64 pad;                        /* 16-byte alignment              */
};

enum {
    SC_READ  = 0,
    SC_WRITE = 1,
    SC_OPEN  = 2,
    SC_CLOSE = 3,
    SC_DUP2  = 33,
    SC_PIPE  = 22,
    SC_LSEEK = 8,
    SC_MMAP  = 9,
    SC_MUNMAP = 11,
    SC_BRK   = 12,
    SC_GETCWD = 79,
    SC_CHDIR = 80,
    SC_MKDIR = 83,
    SC_FUTEX = 202,
    SC_GETDENTS = 217,
    SC_YIELD = 24,
    SC_GETPID = 39,
    SC_CHMOD = 90,
    SC_GETTID = 186,
    SC_FORK  = 57,
    SC_EXECVE = 59,
    SC_EXIT  = 60,
    SC_WAIT4 = 61,
    SC_RMDIR = 84,
    SC_UNLINK = 87,
    SC_EXIT_GROUP = 231,
    SC_SYNC = 162,
    SC_FSYNC = 74,
    SC_SOCKET = 41,
    SC_CONNECT = 42,
    SC_SENDTO = 44,
    SC_RECVFROM = 45,
    SC_BIND = 49,
    SC_LISTEN = 50,
    SC_ACCEPT = 43,
    SC_KILL = 62,
    SC_SETPGID = 109,
    SC_GETPGID = 121,
    SC_SETSID = 112,
    SC_GETSID = 124,
    SC_SHUTDOWN = 48,
    SC_GETSOCKNAME = 51,
    SC_GETPEERNAME = 52,
    SC_SETSOCKOPT = 54,
    SC_GETSOCKOPT = 55,
    SC_SELECT = 23,
    SC_POLL = 7,
    SC_IOCTL = 16,
    SC_PING = 500,
    SC_TTY_SETFG = 501,   /* tcsetpgrp */
    SC_TTY_GETFG = 502,   /* tcgetpgrp */
    SC_RT_SIGACTION = 13,
    SC_RT_SIGRETURN = 15,
    SC_NICE = 89
};

enum {
    MSR_EFER   = 0xC0000080,
    MSR_STAR   = 0xC0000081,
    MSR_LSTAR  = 0xC0000082,
    MSR_SFMASK = 0xC0000084
};

static u64 syscall_count;

/* Program the SYSCALL/SYSRET MSRs. GDT layout is STAR-compatible:
 * SYSCALL loads CS=0x08/SS=0x10 (kernel), SYSRET loads CS=(0x10+16)|3=0x23
 * and SS=(0x10+8)|3=0x1B (user). SFMASK clears IF+DF on entry so the stack
 * switch window cannot be interrupted. */
void syscall_init(void) {
    msr_write(MSR_STAR, ((u64)0x10 << 48) | ((u64)0x08 << 32));
    msr_write(MSR_LSTAR, (u64)(&syscall_entry));
    msr_write(MSR_SFMASK, 0x600);
    msr_write(MSR_EFER, msr_read(MSR_EFER) | 1);    /* EFER.SCE: enable SYSCALL */
}

static u64 sys_fork(struct syscall_frame *f) {
    u64 parent_as = sched_current_as_h();
    if (parent_as == 0) {
        return (u64)(-1);               /* kernel threads cannot fork */
    }
    u64 child_as = mem.as_fork_h(parent_as);
    if (child_as == 0) {
        return (u64)(-1);
    }
    u32 child_pid = sched_alloc_pid();
    u32 slot = sched_clone_user((u64)f, child_as, child_pid);
    if (slot == 0xFFFFFFFF) {
        mem.as_destroy_h(child_as);
        return (u64)(-1);
    }
    return child_pid;                   /* parent sees the child pid */
}

/* Terminate the current user process (exit syscall or fatal fault path).
 * Tears down the address space and never returns. */
void proc_terminate(u64 code) {
    u32 pid = sched_current_pid();
    u32 ppid = sched_current_ppid();
    kprintf("[UPROC] pid %u exited (code %u)\n", (u64)pid, code);
    uproc_note_exit(pid, code);

    /* Retain an exit record for the parent; orphans move under pid 1. The
     * exit records are keyed by the *child's* ppid, so reparent records whose
     * owner is the exiting pid (not by the parent of the exiting process). */
    proc_record_exit(pid, ppid, code);
    /* POSIX: notify parent with SIGCHLD (ignored by default). */
    if (ppid != 0) {
        sched_signal_send(ppid, 17);
    }
    /* Wake a parent blocked in wait4, if any. */
    sched_wakeup(sched_wait4_chan(ppid));
    if (pid != 1) {
        sched_reparent_children(pid, 1);
        proc_reparent_exits(pid, 1);
        /* Orphans moved under init: wake pid 1 in case it is waiting. */
        sched_wakeup(sched_wait4_chan(1));
    }

    /* Close all open files, then leave the doomed address space before its
     * page tables are freed. The DEAD TCB slot stays until wait4 reaps it. */
    sched_fd_close_all();
    u64 as_h = sched_current_as_h();
    mem.as_activate_h(0);
    if (as_h != 0) {
        mem.as_destroy_h(as_h);
    }
    kthread_exit();                     /* frees the kernel stack, switches away */
    for (;;) {                          /* unreachable */
    }
}

static u64 syscall_dispatch_inner(struct syscall_frame *f) {
    if (f->rax == SC_READ) {
        return sys_read_fd(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_WRITE) {
        return sys_write_fd(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_OPEN) {
        return sys_open(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_CLOSE) {
        return sys_close(f->rdi);
    }
    if (f->rax == SC_DUP2) {
        return sys_dup2(f->rdi, f->rsi);
    }
    if (f->rax == SC_PIPE) {
        return sys_pipe(f->rdi);
    }
    if (f->rax == SC_SYNC) {
        return sys_sync();
    }
    if (f->rax == SC_FSYNC) {
        return sys_fsync(f->rdi);
    }
    if (f->rax == SC_PING) {
        return sys_ping(f->rdi);
    }
    if (f->rax == SC_SOCKET) {
        return sys_socket(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_BIND) {
        return sys_bind(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_CONNECT) {
        return sys_connect(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_SENDTO) {
        return sys_sendto(f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    }
    if (f->rax == SC_RECVFROM) {
        return sys_recvfrom(f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    }
    if (f->rax == SC_LISTEN) {
        return sys_listen(f->rdi, f->rsi);
    }
    if (f->rax == SC_ACCEPT) {
        return sys_accept(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_LSEEK) {
        return sys_lseek(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_MMAP) {
        return sys_mmap(f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    }
    if (f->rax == SC_MUNMAP) {
        return sys_munmap(f->rdi, f->rsi);
    }
    if (f->rax == SC_CHMOD) {
        return sys_chmod(f->rdi, f->rsi);
    }
    if (f->rax == SC_GETTID) {
        return sys_gettid();
    }
    if (f->rax == SC_FUTEX) {
        return sys_futex(f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    }
    if (f->rax == SC_UNLINK) {
        return sys_unlink(f->rdi);
    }
    if (f->rax == SC_RMDIR) {
        return sys_rmdir(f->rdi);
    }
    if (f->rax == SC_BRK) {
        return sys_brk(f->rdi);
    }
    if (f->rax == SC_GETCWD) {
        return sys_getcwd(f->rdi, f->rsi);
    }
    if (f->rax == SC_CHDIR) {
        return sys_chdir(f->rdi);
    }
    if (f->rax == SC_MKDIR) {
        return sys_mkdir(f->rdi);
    }
    if (f->rax == SC_GETDENTS) {
        return sys_getdents64(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_YIELD) {
        sched_yield();
        return 0;
    }
    if (f->rax == SC_NICE) {
        return sys_nice(f->rdi);
    }
    if (f->rax == SC_GETPID) {
        return (u64)sched_current_pid();
    }
    if (f->rax == SC_FORK) {
        return sys_fork(f);
    }
    if (f->rax == SC_EXECVE) {
        return sys_execve(f->rdi, f->rsi, f->rdx, (u64)f);
    }
    if (f->rax == SC_WAIT4) {
        return sys_wait4(f->rdi, f->rsi, f->rdx, f->r10);
    }
    if (f->rax == SC_KILL) {
        return sys_kill(f->rdi, f->rsi);
    }
    if (f->rax == SC_SETPGID) {
        return sys_setpgid(f->rdi, f->rsi);
    }
    if (f->rax == SC_GETPGID) {
        return sys_getpgid(f->rdi);
    }
    if (f->rax == SC_SETSID) {
        return sys_setsid();
    }
    if (f->rax == SC_GETSID) {
        return sys_getsid(f->rdi);
    }
    if (f->rax == SC_SHUTDOWN) {
        return sys_shutdown(f->rdi, f->rsi);
    }
    if (f->rax == SC_GETSOCKNAME) {
        return sys_getsockname(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_GETPEERNAME) {
        return sys_getpeername(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_TTY_SETFG) {
        return sys_tty_setfg(f->rdi);
    }
    if (f->rax == SC_TTY_GETFG) {
        return sys_tty_getfg(f->rdi);
    }
    if (f->rax == SC_SETSOCKOPT) {
        return sys_setsockopt(f->rdi, f->rsi, f->rdx, f->r10, f->r8);
    }
    if (f->rax == SC_GETSOCKOPT) {
        return sys_getsockopt(f->rdi, f->rsi, f->rdx, f->r10, f->r8);
    }
    if (f->rax == SC_SELECT) {
        return sys_select(f->rdi, f->rsi, f->rdx, f->r10, f->r8);
    }
    if (f->rax == SC_POLL) {
        return sys_poll(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_IOCTL) {
        return sys_ioctl(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_RT_SIGACTION) {
        return sys_rt_sigaction(f->rdi, f->rsi, f->rdx, f->r10);
    }
    if (f->rax == SC_RT_SIGRETURN) {
        if (!sched_sigreturn(&f->rip, &f->rflags, &f->pad, &f->rdi)) {
            return (u64)(-1);
        }
        return 0;
    }
    if (f->rax == SC_EXIT) {
        proc_terminate(f->rdi);
    }
    if (f->rax == SC_EXIT_GROUP) {
        proc_terminate(f->rdi);
    }
    kprintf("[SYSCALL] pid %u: unknown syscall nr=%u, rejected\n",
            (u64)sched_current_pid(), f->rax);
    return (u64)(-1);
}

u64 syscall_dispatch(struct syscall_frame *f) {
    u64 r;
    syscall_count++;
    r = syscall_dispatch_inner(f);
    /* Deliver custom handler frames or default-terminate fatal signals. */
    sched_check_signals_frame(&f->rip, &f->rflags, &f->pad, &f->rdi);
    return r;
}

u64 syscall_count_total(void) {
    return syscall_count;
}
