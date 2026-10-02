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

/* sysfile.c */
extern u64 sys_open(u64 upath, u64 flags, u64 mode);
extern u64 sys_close(u64 fd);
extern u64 sys_read_fd(u64 fd, u64 buf, u64 len);
extern u64 sys_write_fd(u64 fd, u64 buf, u64 len);
extern u64 sys_lseek(u64 fd, u64 off, u64 whence);
extern u64 sys_mkdir(u64 upath);
extern u64 sys_chdir(u64 upath);
extern u64 sys_getcwd(u64 buf, u64 len);
extern u64 sys_getdents64(u64 fd, u64 buf, u64 len);

/* sysproc.c */
extern u64 sys_brk(u64 newbrk);
extern u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off);

/* proc.c */
extern void proc_record_exit(u32 pid, u32 ppid, u64 code);
extern void proc_reparent_exits(u32 old_ppid, u32 new_ppid);
extern u64 sys_wait4(u64 pid, u64 status, u64 options, u64 ruspace);

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
    SC_LSEEK = 8,
    SC_MMAP  = 9,
    SC_BRK   = 12,
    SC_GETCWD = 79,
    SC_CHDIR = 80,
    SC_MKDIR = 83,
    SC_YIELD = 24,
    SC_GETPID = 39,
    SC_GETDENTS = 217,
    SC_FORK  = 57,
    SC_EXECVE = 59,
    SC_EXIT  = 60,
    SC_WAIT4 = 61
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
    if (pid != 1) {
        sched_reparent_children(pid, 1);
        proc_reparent_exits(pid, 1);
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

u64 syscall_dispatch(struct syscall_frame *f) {
    syscall_count++;
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
    if (f->rax == SC_LSEEK) {
        return sys_lseek(f->rdi, f->rsi, f->rdx);
    }
    if (f->rax == SC_MMAP) {
        return sys_mmap(f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
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
    if (f->rax == SC_EXIT) {
        proc_terminate(f->rdi);
    }
    kprintf("[SYSCALL] pid %u: unknown syscall nr=%u, rejected\n",
            (u64)sched_current_pid(), f->rax);
    return (u64)(-1);
}

u64 syscall_count_total(void) {
    return syscall_count;
}
