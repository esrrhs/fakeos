package kernel;
import drivers;
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
extern void kthread_exit(void);

/* uproc.c */
extern void uproc_note_exit(u32 pid, u64 code);

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
    SC_WRITE = 1,
    SC_YIELD = 24,
    SC_GETPID = 39,
    SC_FORK  = 57,
    SC_EXIT  = 60
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

static u64 sys_write(u64 fd, u64 buf, u64 len) {
    if (fd != 1 && fd != 2) {
        return (u64)(-1);
    }
    if (len == 0) {
        return 0;
    }
    /* Validate the buffer against the caller's VMAs before dereferencing;
     * demand-paged holes inside a VMA fault in transparently on read. */
    if (!mem.as_user_range_ok(sched_current_as_h(), buf, len)) {
        return (u64)(-1);
    }
    volatile char *p = (volatile char *)buf;
    u64 i;
    for (i = 0; i < len; i++) {
        drivers.uart_putc(p[i]);
    }
    return len;
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
    kprintf("[UPROC] pid %u exited (code %u)\n", (u64)pid, code);
    uproc_note_exit(pid, code);

    /* Leave the doomed address space before its page tables are freed. */
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
    if (f->rax == SC_WRITE) {
        return sys_write(f->rdi, f->rsi, f->rdx);
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
