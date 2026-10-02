package kernel;
import arch;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern u64 pmm_alloc_pages(u32 order);
extern void pmm_free_pages(u64 paddr, u32 order);
extern void switch_context(u64 *prev_rsp, u64 next_rsp);

/* New-kernel-thread landing point in arch/x86_64/switch.asm. */
extern void thread_trampoline(void);

/* Ring-3 entry trampolines in arch/x86_64/sysentry.asm. */
extern void user_iret_trampoline(void);
extern void fork_ret_trampoline(void);

/* Kernel stack top used by the SYSCALL entry to leave the user stack behind.
 * Read directly by sysentry.asm; refreshed by schedule_tail() on every
 * context switch. */
u64 syscall_kstack_top;

/* Kernel thread control block. Kernel threads share the bootstrap address
 * space (as_h = 0); user threads own a ring-3 address space created by the
 * process layer and are entered through user_iret_trampoline. */

enum {
    TCB_FREE     = 0,
    TCB_RUNNABLE = 1,
    TCB_DEAD     = 2,

    MAX_THREADS  = 32,
    STACK_ORDER  = 2,                 /* 4 frames = 16 KiB per kernel stack  */
    STACK_BYTES  = 16384
};

struct tcb {
    u64 rsp;            /* Saved kernel stack pointer (0 = never started) */
    u64 stack_phys;    /* Buddy block backing the kernel stack            */
    u64 kstack_top;    /* Kernel stack top (HHDM address, 16-aligned)     */
    u64 entry;         /* Thread entry function                           */
    u64 arg;           /* Single u64 argument                             */
    u64 as_h;          /* Address space handle (0 = kernel space)         */
    u32 state;
    u32 id;
    u32 pid;           /* User process id (0 for kernel threads)          */
    u32 is_user;       /* Runs at ring 3 between syscalls/interrupts      */
};

static struct tcb threads[MAX_THREADS];
static u32 current;
static u32 active;                   /* Scheduler online flag              */
static volatile u64 sched_ticks;
static u32 next_pid = 1;             /* User process id allocator          */
static volatile u64 user_mode_ticks; /* APIC ticks taken while in ring 3   */

/* ---------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------- */

void sched_init(void) {
    u32 i;
    for (i = 0; i < MAX_THREADS; i++) {
        threads[i].state = TCB_FREE;
        threads[i].rsp = 0;
        threads[i].stack_phys = 0;
        threads[i].kstack_top = 0;
        threads[i].entry = 0;
        threads[i].arg = 0;
        threads[i].as_h = 0;
        threads[i].pid = 0;
        threads[i].is_user = 0;
        threads[i].id = i;
    }
    /* Slot 0 is the boot/init context (kmain). Its RSP is captured lazily
     * on the first switch away. */
    threads[0].state = TCB_RUNNABLE;
    threads[0].id = 0;
    current = 0;
    sched_ticks = 0;
    user_mode_ticks = 0;
    /* Until the first user thread runs, SYSCALL/trap entries use the boot
     * RSP0 stack programmed by gdt_init(). */
    syscall_kstack_top = arch.gdt_get_rsp0();
    active = 1;
}

/* Find the lowest-indexed RUNNABLE thread after `from` in round-robin order,
 * wrapping around. Slot 0 is the boot/idle context: it is selected only when
 * no real kernel thread is runnable, so created threads are never starved by
 * the idle loop and user tests do not pay a 10 ms idle quantum per round.
 * Returns from if nothing else is runnable. */
static u32 pick_next(u32 from) {
    u32 step;
    for (step = 1; step <= MAX_THREADS; step++) {
        u32 idx = (from + step) % MAX_THREADS;
        if (idx == 0) {
            continue;
        }
        if (threads[idx].state == TCB_RUNNABLE) {
            return idx;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Thread creation
 * ------------------------------------------------------------------------- */

static u32 alloc_slot(void) {
    u32 i;
    for (i = 1; i < MAX_THREADS; i++) {
        if (threads[i].state == TCB_FREE) {
            return i;
        }
    }
    return 0xFFFFFFFF;
}

u32 kthread_create(u64 entry, u64 arg) {
    u32 i = alloc_slot();
    if (i == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }

    u64 stack_phys = pmm_alloc_pages(STACK_ORDER);
    if (stack_phys == 0) {
        return 0xFFFFFFFF;
    }
    /* Kernel stacks are accessed through the HHDM direct map. */
    u64 top = 0xFFFF800000000000ULL + stack_phys + STACK_BYTES;

    /* Synthesize the frame switch_context pops on first resume:
     * [r15][r14][r13][r12=entry][rbx=arg][rbp=0][ret=trampoline] */
    u64 *frame = (u64 *)(top - 56);
    frame[0] = 0;                              /* r15 */
    frame[1] = 0;                              /* r14 */
    frame[2] = 0;                              /* r13 */
    frame[3] = entry;                          /* r12 */
    frame[4] = arg;                            /* rbx */
    frame[5] = 0;                              /* rbp */
    frame[6] = (u64)(&thread_trampoline);      /* return address */

    threads[i].rsp = top - 56;
    threads[i].stack_phys = stack_phys;
    threads[i].kstack_top = top;
    threads[i].entry = entry;
    threads[i].arg = arg;
    threads[i].as_h = 0;
    threads[i].pid = 0;
    threads[i].is_user = 0;
    threads[i].state = TCB_RUNNABLE;
    return i;
}

/* Create a ring-3 user thread: first resume lands in user_iret_trampoline,
 * which IRETs to entry_rip with the given user stack. CR3 is switched to the
 * process address space by schedule_tail() before the trampoline runs. */
u32 kthread_create_user(u64 entry_rip, u64 user_rsp, u64 as_h, u32 pid) {
    u32 i = alloc_slot();
    if (i == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }

    u64 stack_phys = pmm_alloc_pages(STACK_ORDER);
    if (stack_phys == 0) {
        return 0xFFFFFFFF;
    }
    u64 top = 0xFFFF800000000000ULL + stack_phys + STACK_BYTES;

    u64 *frame = (u64 *)(top - 56);
    frame[0] = 0;                                   /* r15 */
    frame[1] = 0;                                   /* r14 */
    frame[2] = 0;                                   /* r13 */
    frame[3] = entry_rip;                           /* r12 = user RIP */
    frame[4] = user_rsp;                            /* rbx = user RSP */
    frame[5] = 0;                                   /* rbp */
    frame[6] = (u64)(&user_iret_trampoline);        /* return address */

    threads[i].rsp = top - 56;
    threads[i].stack_phys = stack_phys;
    threads[i].kstack_top = top;
    threads[i].entry = entry_rip;
    threads[i].arg = 0;
    threads[i].as_h = as_h;
    threads[i].pid = pid;
    threads[i].is_user = 1;
    threads[i].state = TCB_RUNNABLE;
    return i;
}

/* fork(): clone the currently running user thread. The parent's 128-byte
 * syscall frame is copied onto the child's kernel stack; the child first
 * resumes in fork_ret_trampoline, which returns 0 to user space. */
u32 sched_clone_user(u64 frame_base, u64 child_as_h, u32 child_pid) {
    u32 i = alloc_slot();
    if (i == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }

    u64 stack_phys = pmm_alloc_pages(STACK_ORDER);
    if (stack_phys == 0) {
        return 0xFFFFFFFF;
    }
    u64 top = 0xFFFF800000000000ULL + stack_phys + STACK_BYTES;

    u64 *dst = (u64 *)(top - 128);
    u64 *src = (u64 *)frame_base;
    u32 k;
    for (k = 0; k < 16; k++) {
        dst[k] = src[k];
    }

    /* switch_context frame below the copied syscall frame: r12 = frame base */
    u64 *frame = (u64 *)(top - 128 - 56);
    frame[0] = 0;                                   /* r15 */
    frame[1] = 0;                                   /* r14 */
    frame[2] = 0;                                   /* r13 */
    frame[3] = top - 128;                           /* r12 = frame base */
    frame[4] = 0;                                   /* rbx */
    frame[5] = 0;                                   /* rbp */
    frame[6] = (u64)(&fork_ret_trampoline);         /* return address */

    threads[i].rsp = top - 128 - 56;
    threads[i].stack_phys = stack_phys;
    threads[i].kstack_top = top;
    threads[i].entry = 0;
    threads[i].arg = 0;
    threads[i].as_h = child_as_h;
    threads[i].pid = child_pid;
    threads[i].is_user = 1;
    threads[i].state = TCB_RUNNABLE;
    return i;
}

/* ---------------------------------------------------------------------------
 * Scheduler entry points
 * ------------------------------------------------------------------------- */

/* Common tail of every context switch: install the target thread's
 * ring-transition stack (TSS RSP0 for user-mode traps, SYSCALL entry stack)
 * and address space, then hand the CPU over. Both kernel stacks and kernel
 * text live in the shared higher half, so switching CR3 mid-path is safe. */
static void schedule_tail(u32 prev, u32 next) {
    u64 top = threads[next].kstack_top;
    if (top == 0) {
        top = arch.gdt_get_rsp0();      /* Slot 0 (boot/idle context) */
    }
    arch.gdt_set_rsp0(top);
    syscall_kstack_top = top;
    if (threads[next].as_h != threads[prev].as_h) {
        mem.as_activate_h(threads[next].as_h);
    }
    switch_context(&threads[prev].rsp, threads[next].rsp);
}

static void sched_switch_to(u32 next) {
    if (next == current) {
        return;
    }
    u32 prev = current;
    current = next;
    schedule_tail(prev, next);
}

/* Voluntary yield from a running thread. The context switch itself must run
 * with interrupts disabled: a timer tick landing halfway through a stack
 * swap would nest another switch and corrupt saved RSP values. */
void sched_yield(void) {
    if (!active) {
        return;
    }
    arch.cpu_cli();
    sched_switch_to(pick_next(current));
    arch.cpu_sti();
}

/* Called from the Local APIC timer ISR (vector 32) with interrupts off in the
 * interrupt gate. EOI first so the APIC keeps firing after we switch stacks. */
void sched_tick(void) {
    arch.lapic_irq();                    /* EOI + raw tick counter bump */
    if (!active) {
        return;
    }
    sched_ticks++;
    sched_switch_to(pick_next(current));
}

/* Terminate the current thread. Does not return. */
void kthread_exit(void) {
    /* No preemption after this point: the stack we stand on is about to be
     * returned to the buddy allocator. */
    arch.cpu_cli();

    u32 self = current;
    threads[self].state = TCB_DEAD;
    u64 sp = threads[self].stack_phys;
    threads[self].stack_phys = 0;
    threads[self].rsp = 0;
    if (sp != 0) {
        pmm_free_pages(sp, STACK_ORDER);
    }

    u32 next = pick_next(self);
    if (next == self) {
        for (;;) {
            arch.cpu_sti();
            arch.cpu_halt();
            arch.cpu_cli();
        }
    }
    current = next;
    schedule_tail(self, next);
    for (;;) {
        arch.cpu_sti();
        arch.cpu_halt();
        arch.cpu_cli();
    }
}

/* ---------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

u64 sched_tick_count(void) {
    return sched_ticks;
}

u32 sched_current_id(void) {
    return current;
}

u32 sched_thread_state(u32 id) {
    if (id >= MAX_THREADS) return TCB_FREE;
    return threads[id].state;
}

u32 sched_max_threads(void) {
    return MAX_THREADS;
}

/* ---------------------------------------------------------------------------
 * User-process support
 * ------------------------------------------------------------------------- */

u32 sched_alloc_pid(void) {
    return next_pid++;
}

u32 sched_current_pid(void) {
    return threads[current].pid;
}

u64 sched_current_as_h(void) {
    return threads[current].as_h;
}

/* Called from the timer ISR path when a tick was taken while the interrupted
 * context was ring 3 (CS & 3): proves ring-3 threads are truly preemptible. */
void sched_note_user_tick(void) {
    user_mode_ticks++;
}

u64 sched_user_tick_count(void) {
    return user_mode_ticks;
}
