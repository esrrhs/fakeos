package kernel;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t s32;
typedef types.int64_t s64;

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
extern u32 sched_clone_thread(u64 fn, u64 user_stack, u64 arg, u32 child_tid);
extern u32 sched_is_secondary_thread(void);
extern u32 sched_current_tid(void);
extern void sched_set_fs_base(u64 base);
extern u64 sched_get_fs_base(void);
extern void sched_set_clear_child_tid(u64 addr);
extern void sched_do_clear_child_tid(void);
extern void sched_replace_as(u64 new_as_h);
extern void sched_yield(void);
extern u32 sched_fd_close_all(void);
extern void sched_process_close_all_fds(u32 pid);
extern void sched_fd_unpin_self(void);
extern u32 sched_current_task_cancelled(void);
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
extern u64 sys_rename(u64 uold, u64 unew);
extern u64 sys_unlink(u64 upath);
extern u64 sys_link(u64 uold, u64 unew);
extern u64 sys_symlink(u64 utarget, u64 ulinkpath);
extern u64 sys_readlink(u64 upath, u64 ubuf, u64 bufsiz);
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
extern u64 sys_ftruncate(u64 fd, u64 length);
extern u64 sys_stat(u64 upath, u64 ustat);
extern u64 sys_lstat(u64 upath, u64 ustat);
extern u64 sys_fstat(u64 fd, u64 ustat);
extern u64 sys_newfstatat(u64 dfd, u64 upath, u64 ustat, u64 flags);
extern u64 sys_utimensat(u64 dfd, u64 upath, u64 utimes, u64 flags);
extern u64 sys_statfs(u64 upath, u64 ustat);

/* sysproc.c */
extern u64 sys_brk(u64 newbrk);
extern u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off);
extern u64 sys_mprotect(u64 addr, u64 len, u64 prot);
extern u64 sys_nice(u64 inc);

/* natcap.c */
extern s64 sys_nat_channel_create(u64 send_out, u64 recv_out);
extern s64 sys_nat_send(u64 cap, u64 value, u64 block);
extern s64 sys_nat_recv(u64 cap, u64 value_out, u64 block);
extern s64 sys_nat_dup(u64 cap, u64 rights);
extern s64 sys_nat_close(u64 cap);
extern s64 sys_nat_mutex_create(u64 out);
extern s64 sys_nat_mutex_lock(u64 cap, u64 block);
extern s64 sys_nat_mutex_unlock(u64 cap);
extern s64 sys_nat_mutex_consistent(u64 cap);
extern s64 sys_nat_mutex_stat(u64 cap, u64 out);
extern s64 sys_nat_send_timed(u64 cap, u64 value, u64 abs_deadline_ns);
extern s64 sys_nat_recv_timed(u64 cap, u64 value_out, u64 abs_deadline_ns);
extern s64 sys_nat_mutex_lock_timed(u64 cap, u64 abs_deadline_ns);
extern s64 sys_nat_chan_send_msg(u64 cap, u64 desc_u, u64 abs_deadline_ns);
extern s64 sys_nat_chan_recv_msg(u64 cap, u64 desc_u, u64 abs_deadline_ns);
extern s64 sys_nat_wait_any(u64 items_u, u64 count, u64 abs_deadline_ns);
extern s64 sys_nat_process_spawn(u64 desc_u, u64 cap_out_u);
extern s64 sys_nat_task_spawn(u64 entry, u64 stack, u64 arg, u64 cap_out_u);
extern s64 sys_nat_exec_wait(u64 cap, u64 result_out_u, u64 abs_deadline_ns);
extern s64 sys_nat_exec_cancel(u64 cap);
extern void native_exec_obj_complete(u32 kind, u64 target_id, u64 exit_val);
extern void nat_process_exit(u32 pid);
extern void native_mutex_owner_died_key(u64 key);
extern u64 sched_current_owner_key(void);

/* net.c raw rings */
extern u64 sys_raw_create(void);
extern u64 sys_raw_map(u64 handle, u64 uva);
extern u64 sys_raw_tx_kick(u64 handle, u64 max);
extern u64 sys_raw_close(u64 handle);
/* epoll.c: eventfd(2) + epoll(2)-lite */
extern u64 sys_eventfd2(u64 initval, u64 flags);
extern u64 sys_epoll_create1(u64 flags);
extern u64 sys_epoll_ctl(u64 epfd, u64 op, u64 fd, u64 ev_u);
extern u64 sys_epoll_wait(u64 epfd, u64 events_u, u64 maxevents, u64 timeout_ms);
extern u64 sys_munmap(u64 addr, u64 len);
extern u64 sys_chmod(u64 path, u64 mode);
extern u64 sys_umask(u64 mask);
extern u64 sys_chown(u64 path, u64 uid, u64 gid);
extern u64 sys_getuid(void);
extern u64 sys_geteuid(void);
extern u64 sys_getgid(void);
extern u64 sys_getegid(void);
extern u64 sys_setuid(u64 uid);
extern u64 sys_setgid(u64 gid);
extern u64 sys_setreuid(u64 ruid, u64 euid);
extern u64 sys_setregid(u64 rgid, u64 egid);
extern u64 sys_getgroups(u64 size, u64 list);
extern u64 sys_setgroups(u64 size, u64 list);
extern u64 sys_gettid(void);
extern u64 sys_futex(u64 uaddr, u64 op, u64 val, u64 timeout, u64 uaddr2, u64 val3);
extern u64 sys_set_tid_address(u64 tidptr);
extern u64 sys_getppid(void);
extern u64 sys_uname(u64 ubuf);
extern u64 sys_nanosleep(u64 ureq, u64 urem);
extern u64 sys_clock_gettime(u64 clockid, u64 utp);

/* proc.c */
extern u32 proc_record_exit(u32 pid, u32 ppid, u64 code);
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
extern void sched_check_signals_frame(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi,
                                      u64 *f_rax);
extern u32 sched_sigreturn(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi,
                           u64 *f_rax);

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
    SC_MPROTECT = 10,
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
    SC_CHOWN = 92,
    SC_UMASK = 95,
    SC_GETUID = 102,
    SC_GETGID = 104,
    SC_SETUID = 105,
    SC_SETGID = 106,
    SC_GETEUID = 107,
    SC_GETEGID = 108,
    SC_SETREUID = 113,
    SC_SETREGID = 114,
    SC_GETGROUPS = 115,
    SC_SETGROUPS = 116,
    SC_GETTID = 186,
    SC_CLONE = 56,
    SC_FORK  = 57,
    SC_EXECVE = 59,
    SC_EXIT  = 60,
    SC_WAIT4 = 61,
    SC_RMDIR = 84,
    SC_RENAME = 82,
    SC_UNLINK = 87,
    SC_LINK = 86,
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
    SC_SYMLINK = 88,
    SC_READLINK = 89,
    SC_NICE = 503,            /* fakeos-specific; Linux has no nice(2) nr */
    /* Native capability namespace (process-relative handles over kernel
     * native objects). fakeos-specific numbers, no Linux ABI collision. */
    SC_NAT_CHANNEL_CREATE = 504,
    SC_NAT_SEND = 505,
    SC_NAT_RECV = 506,
    SC_NAT_DUP = 507,
    SC_NAT_CLOSE = 508,
    /* Native robust mutex (architecture section 6). */
    SC_NAT_MUTEX_CREATE = 513,
    SC_NAT_MUTEX_LOCK = 514,
    SC_NAT_MUTEX_UNLOCK = 515,
    SC_NAT_MUTEX_CONSISTENT = 516,
    SC_NAT_MUTEX_STAT = 517,
    SC_NAT_SEND_TIMED = 518,
    SC_NAT_RECV_TIMED = 519,
    SC_NAT_MUTEX_LOCK_TIMED = 520,
    SC_NAT_CHAN_SEND_MSG = 521,
    SC_NAT_CHAN_RECV_MSG = 522,
    SC_NAT_WAIT_ANY = 523,
    SC_NAT_PROCESS_SPAWN = 524,
    SC_NAT_TASK_SPAWN = 525,
    SC_NAT_TASK_EXIT = 526,
    SC_NAT_EXEC_WAIT = 527,
    SC_NAT_EXEC_CANCEL = 528,
    /* Zero-copy raw device rings (AF_XDP analogue). */
    SC_RAW_CREATE = 509,
    SC_RAW_MAP = 510,
    SC_RAW_TX_KICK = 511,
    SC_RAW_CLOSE = 512,
    /* eventfd(2) + epoll(2)-lite (modern readiness multiplexing).
     * Linux-native numbers; the ABI matches upstream exactly. */
    SC_EPOLL_WAIT = 232,
    SC_EPOLL_CTL = 233,
    SC_EVENTFD2 = 290,
    SC_EPOLL_CREATE1 = 291,
    SC_FTRUNCATE = 77,
    SC_STAT = 4,
    SC_FSTAT = 5,
    SC_LSTAT = 6,
    SC_NEWFSTATAT = 262,
    SC_ARCH_PRCTL = 158,
    SC_SET_TID_ADDRESS = 218,
    SC_GETPPID = 110,
    SC_UNAME = 63,
    SC_NANOSLEEP = 35,
    SC_CLOCK_GETTIME = 228,
    SC_STATFS = 137,
    SC_UTIMENSAT = 280
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

/* arch_prctl(2): ARCH_SET_FS / ARCH_GET_FS for per-thread TLS (FS base). */
static u64 sys_arch_prctl(u64 code, u64 addr) {
    enum {
        ARCH_SET_FS = 0x1002,
        ARCH_GET_FS = 0x1003
    };
    u64 as;
    if (code == ARCH_SET_FS) {
        sched_set_fs_base(addr);
        return 0;
    }
    if (code == ARCH_GET_FS) {
        as = sched_current_as_h();
        if (as == 0 || addr == 0) {
            return (u64)(-1);
        }
        if (!mem.as_user_range_ok(as, addr, 8)) {
            return (u64)(-1);
        }
        *(volatile u64 *)addr = sched_get_fs_base();
        return 0;
    }
    return (u64)(-1);
}

enum {
    CLONE_VM      = 0x00000100,
    CLONE_FS      = 0x00000200,
    CLONE_FILES   = 0x00000400,
    CLONE_SIGHAND = 0x00000800,
    CLONE_THREAD  = 0x00010000,
    CLONE_THREAD_REQ = CLONE_VM | CLONE_THREAD
};

/* clone(2) basics: CLONE_VM|CLONE_THREAD starts fn(arg) on user_stack in a
 * new tid sharing the caller's address space. Parent returns the child tid.
 * Fakeos ABI: clone(flags, stack, fn, arg) — not the Linux tls/tidptr shape. */
static u64 sys_clone(u64 flags, u64 stack, u64 fn, u64 arg) {
    u32 child_tid;
    u32 slot;
    u64 as_h;
    if ((flags & (u64)CLONE_THREAD_REQ) != (u64)CLONE_THREAD_REQ) {
        return (u64)(-1);               /* only same-AS threads for now */
    }
    if (stack == 0 || fn == 0) {
        return (u64)(-1);
    }
    as_h = sched_current_as_h();
    if (as_h == 0) {
        return (u64)(-1);
    }
    child_tid = sched_alloc_pid();      /* tid namespace shares pid allocator */
    if (!child_tid) return (u64)-1;
    slot = sched_clone_thread(fn, stack, arg, child_tid);
    if (slot == 0xFFFFFFFF) {
        return (u64)(-1);
    }
    return (u64)child_tid;
}

extern u32 sched_live_process_count(void);
extern u32 sched_fork_proc_limit(void);

/* P0-9 fork admission: a deterministic process-count limit (pool size
 * minus kernel-thread headroom) checked BEFORE any allocation —
 * exhaustion fails with clean EAGAIN instead of depending on which
 * resource happens to run out first. */
static u64 fork_exclusive(struct syscall_frame *f) {
    u64 parent_as = sched_current_as_h();
    if (parent_as == 0) {
        return (u64)(-1);               /* kernel threads cannot fork */
    }
    if (sched_live_process_count() + 1 > sched_fork_proc_limit()) {
        return (u64)-11;                /* EAGAIN: process limit */
    }
    u64 child_as = mem.as_fork_h(parent_as);
    if (child_as == 0) {
        return (u64)(-1);               /* ENOMEM: AS clone failed */
    }
    u32 child_pid = sched_alloc_pid();
    if (!child_pid) {mem.as_destroy_h(child_as);return (u64)-12;}
    u32 slot = sched_clone_user((u64)f, child_as, child_pid);
    if (slot == 0xFFFFFFFF) {
        mem.as_destroy_h(child_as);
        return (u64)-12;
    }
    return child_pid;                   /* parent sees the child pid */
}

extern u32 sched_process_image_begin(void);
extern void sched_process_image_end(void);
extern void sched_process_group_exit(u64 code);
extern u64 sched_process_leave(u64 code);
extern u32 sched_process_exit_ready(void);
extern u32 sched_process_exit_ready_ex(u32 *native_spawned_out);
static u64 sys_fork(struct syscall_frame *f) {
    u64 rc;
    if(!sched_process_image_begin())return (u64)-16;
    rc=fork_exclusive(f);sched_process_image_end();return rc;
}
/* Raw task exit and native process/task exit. */
static void proc_exit_task(u64 code) {
    u32 pid=sched_current_pid();
    u32 tid=sched_current_tid();
    u32 ppid;
    u64 as;
    sched_fd_unpin_self();
    native_mutex_owner_died_key(sched_current_owner_key());
    sched_do_clear_child_tid();
    native_exec_obj_complete(3, (u64)tid, code);
    as=sched_process_leave(code);
    if(as) {
        u32 is_nat_proc = 0;
        sched_process_close_all_fds(pid);
        nat_process_exit(pid);
        mem.as_release_h(as);
        code=sched_process_exit_ready_ex(&is_nat_proc);
        native_exec_obj_complete(4, (u64)pid, code);
        kprintf("[UPROC] pid %u exited (code %u)\n",(u64)pid,code);
        uproc_note_exit(pid,code);
        if (!is_nat_proc) {
            ppid=proc_record_exit(pid,sched_current_ppid(),code);
            if(ppid)sched_signal_send(ppid,17);
            sched_wakeup(sched_wait4_chan(ppid));
        }
        if(pid!=1){
            sched_reparent_children(pid,1);proc_reparent_exits(pid,1);
            sched_wakeup(sched_wait4_chan(1));
        }
    }
    kthread_exit();
    for(;;){}
}
void proc_exit_task_cancelled(void) {
    proc_exit_task((u64)-4);
}
void proc_terminate(u64 code) {
    sched_process_group_exit(code);
    sched_signal_send(sched_current_pid(),9);
    proc_exit_task(code);
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
    if (f->rax == SC_MPROTECT) {
        return sys_mprotect(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_MUNMAP) {
        return sys_munmap(f->rdi, f->rsi);
    }
    if (f->rax == SC_FTRUNCATE) {
        return sys_ftruncate(f->rdi, f->rsi);
    }
    if (f->rax == SC_STAT) {
        return sys_stat(f->rdi, f->rsi);
    }
    if (f->rax == SC_LSTAT) {
        return sys_lstat(f->rdi, f->rsi);
    }
    if (f->rax == SC_FSTAT) {
        return sys_fstat(f->rdi, f->rsi);
    }
    if (f->rax == SC_NEWFSTATAT) {
        return sys_newfstatat(f->rdi, f->rsi, f->rdx, f->r10);
    }
    if (f->rax == SC_UTIMENSAT) {
        return sys_utimensat(f->rdi, f->rsi, f->rdx, f->r10);
    }
    if (f->rax == SC_STATFS) {
        return sys_statfs(f->rdi, f->rsi);
    }
    if (f->rax == SC_CHMOD) {
        return sys_chmod(f->rdi, f->rsi);
    }
    if (f->rax == SC_CHOWN) {
        return sys_chown(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_UMASK) {
        return sys_umask(f->rdi);
    }
    if (f->rax == SC_GETUID) {
        return sys_getuid();
    }
    if (f->rax == SC_GETEUID) {
        return sys_geteuid();
    }
    if (f->rax == SC_GETGID) {
        return sys_getgid();
    }
    if (f->rax == SC_GETEGID) {
        return sys_getegid();
    }
    if (f->rax == SC_SETUID) {
        return sys_setuid(f->rdi);
    }
    if (f->rax == SC_SETGID) {
        return sys_setgid(f->rdi);
    }
    if (f->rax == SC_SETREUID) {
        return sys_setreuid(f->rdi, f->rsi);
    }
    if (f->rax == SC_SETREGID) {
        return sys_setregid(f->rdi, f->rsi);
    }
    if (f->rax == SC_GETGROUPS) {
        return sys_getgroups(f->rdi, f->rsi);
    }
    if (f->rax == SC_SETGROUPS) {
        return sys_setgroups(f->rdi, f->rsi);
    }
    if (f->rax == SC_GETTID) {
        return sys_gettid();
    }
    if (f->rax == SC_ARCH_PRCTL) {
        return sys_arch_prctl(f->rdi, f->rsi);
    }
    if (f->rax == SC_SET_TID_ADDRESS) {
        return sys_set_tid_address(f->rdi);
    }
    if (f->rax == SC_FUTEX) {
        return sys_futex(f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    }
    if (f->rax == SC_RENAME) {
        return sys_rename(f->rdi, f->rsi);
    }
    if (f->rax == SC_UNLINK) {
        return sys_unlink(f->rdi);
    }
    if (f->rax == SC_LINK) {
        return sys_link(f->rdi, f->rsi);
    }
    if (f->rax == SC_SYMLINK) {
        return sys_symlink(f->rdi, f->rsi);
    }
    if (f->rax == SC_READLINK) {
        return sys_readlink(f->rdi, f->rsi, f->rdx);
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
    if (f->rax == SC_NAT_CHANNEL_CREATE) {
        return sys_nat_channel_create(f->rdi, f->rsi);
    }
    if (f->rax == SC_NAT_SEND) {
        return sys_nat_send(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_NAT_RECV) {
        return sys_nat_recv(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_NAT_DUP) {
        return sys_nat_dup(f->rdi, f->rsi);
    }
    if (f->rax == SC_NAT_CLOSE) {
        return sys_nat_close(f->rdi);
    }
    if (f->rax == SC_NAT_MUTEX_CREATE) {
        return sys_nat_mutex_create(f->rdi);
    }
    if (f->rax == SC_NAT_MUTEX_LOCK) {
        return sys_nat_mutex_lock(f->rdi, f->rsi);
    }
    if (f->rax == SC_NAT_MUTEX_UNLOCK) {
        return sys_nat_mutex_unlock(f->rdi);
    }
    if (f->rax == SC_NAT_MUTEX_CONSISTENT) {
        return sys_nat_mutex_consistent(f->rdi);
    }
    if (f->rax == SC_NAT_MUTEX_STAT) {
        return sys_nat_mutex_stat(f->rdi, f->rsi);
    }
    if (f->rax == SC_NAT_SEND_TIMED) {
        return sys_nat_send_timed(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_NAT_RECV_TIMED) {
        return sys_nat_recv_timed(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_NAT_MUTEX_LOCK_TIMED) {
        return sys_nat_mutex_lock_timed(f->rdi, f->rsi);
    }
    if (f->rax == SC_NAT_CHAN_SEND_MSG) {
        return sys_nat_chan_send_msg(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_NAT_CHAN_RECV_MSG) {
        return sys_nat_chan_recv_msg(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_NAT_WAIT_ANY) {
        return sys_nat_wait_any(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_NAT_PROCESS_SPAWN) {
        return sys_nat_process_spawn(f->rdi, f->rsi);
    }
    if (f->rax == SC_NAT_TASK_SPAWN) {
        return sys_nat_task_spawn(f->rdi, f->rsi, f->rdx, f->r10);
    }
    if (f->rax == SC_NAT_TASK_EXIT) {
        proc_exit_task(f->rdi);
    }
    if (f->rax == SC_NAT_EXEC_WAIT) {
        return sys_nat_exec_wait(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_NAT_EXEC_CANCEL) {
        return sys_nat_exec_cancel(f->rdi);
    }
    if (f->rax == SC_RAW_CREATE) {
        return sys_raw_create();
    }
    if (f->rax == SC_RAW_MAP) {
        return sys_raw_map(f->rdi, f->rsi);
    }
    if (f->rax == SC_RAW_TX_KICK) {
        return sys_raw_tx_kick(f->rdi, f->rsi);
    }
    if (f->rax == SC_RAW_CLOSE) {
        return sys_raw_close(f->rdi);
    }
    if (f->rax == SC_EVENTFD2) {
        return sys_eventfd2(f->rdi, f->rsi);
    }
    if (f->rax == SC_EPOLL_CREATE1) {
        return sys_epoll_create1(f->rdi);
    }
    if (f->rax == SC_EPOLL_CTL) {
        return sys_epoll_ctl(f->rdi, f->rsi, f->rdx, f->r10);
    }
    if (f->rax == SC_EPOLL_WAIT) {
        return sys_epoll_wait(f->rdi, f->rsi, f->rdx, f->r10);
    }
    if (f->rax == SC_GETPID) {
        return (u64)sched_current_pid();
    }
    if (f->rax == SC_GETPPID) {
        return sys_getppid();
    }
    if (f->rax == SC_UNAME) {
        return sys_uname(f->rdi);
    }
    if (f->rax == SC_NANOSLEEP) {
        return sys_nanosleep(f->rdi, f->rsi);
    }
    if (f->rax == SC_CLOCK_GETTIME) {
        return sys_clock_gettime(f->rdi, f->rsi);
    }
    if (f->rax == SC_CLONE) {
        return sys_clone(f->rdi, f->rsi, f->rdx, f->r10);
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
        u64 restored = 0;
        if (!sched_sigreturn(&f->rip, &f->rflags, &f->pad, &f->rdi, &restored)) {
            return (u64)(-1);
        }
        /* Restored RAX is the interrupted syscall retval (or user RAX). */
        return restored;
    }
    if (f->rax == SC_EXIT) {
        proc_exit_task(f->rdi);
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
    if (sched_current_task_cancelled()) {
        proc_exit_task((u64)-4);
    }
    r = syscall_dispatch_inner(f);
    sched_fd_unpin_self();
    if (sched_current_task_cancelled()) {
        proc_exit_task((u64)-4);
    }
    /* Deliver custom handler frames or default-terminate fatal signals. */
    sched_check_signals_frame(&f->rip, &f->rflags, &f->pad, &f->rdi, &r);
    return r;
}

u64 syscall_count_total(void) {
    return syscall_count;
}
