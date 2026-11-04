package kernel;
import arch;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t s32;
typedef types.int64_t s64;

/* pipe.c / net.c (same package) */
extern u32 pipe_fh_is(u32 fh);
extern void pipe_retain(u32 fh);
extern u32 pipe_close(u32 fh);
extern u32 sock_fh_is(u32 fh);
extern void sock_retain(u32 fh);
extern u32 sock_close(u32 fh);

/* fs / Ext2 (externs avoid import fs re-parsing large Ext2 into sched). */
extern u32 fs_console_fh(void);
extern u32 fs_file_retain(u32 fh);
extern u32 fs_close(u32 fh);
extern u32 ext2_fh_is(u32 fh);
extern void ext2_file_retain(u32 fh);
extern u32 ext2_close(u32 fh);

extern u64 pmm_alloc_pages(u32 order);
extern void pmm_free_pages(u64 paddr, u32 order);
extern void switch_context(u64 *prev_rsp, u64 next_rsp);
extern void msr_write(u32 msr, u64 value);

enum { MSR_FS_BASE = 0xC0000100 };

/* New-kernel-thread landing point in arch/x86_64/switch.asm. */
extern void thread_trampoline(void);

/* Ring-3 entry trampolines in arch/x86_64/sysentry.asm. */
extern void user_iret_trampoline(void);
extern void fork_ret_trampoline(void);

/* Per-CPU SYSCALL conduit (GS_BASE points here on each CPU):
 *   [0] kstack top  — sysentry loads RSP from [gs:0]
 *   [1] user RSP    — sysentry stashes user RSP at [gs:8]
 * Required once user threads can run on APs after cross-CPU steal. */
enum { MSR_GS_BASE = 0xC0000101 };
u64 syscall_pcpu[8][2];

/* Kernel thread control block. Kernel threads share the bootstrap address
 * space (as_h = 0); user threads own a ring-3 address space created by the
 * process layer and are entered through user_iret_trampoline. */

enum {
    TCB_FREE     = 0,
    TCB_RUNNABLE = 1,
    TCB_DEAD     = 2,
    TCB_SLEEPING = 3,
    TCB_STOPPED  = 4,

    MAX_THREADS  = 32,
    STACK_ORDER  = 2,                 /* 4 frames = 16 KiB per kernel stack  */
    STACK_BYTES  = 16384,
    SCHED_CPU_NONE = 0xFFFFFFFF,

    FD_MAX       = 16,
    FD_EMPTY     = 0xFFFFFFFF,
    CWD_LEN      = 64
};

struct tcb {
    u64 rsp;            /* Saved kernel stack pointer (0 = never started) */
    u64 stack_phys;    /* Buddy block backing the kernel stack            */
    u64 kstack_top;    /* Kernel stack top (HHDM address, 16-aligned)     */
    u64 entry;         /* Thread entry function                           */
    u64 arg;           /* Single u64 argument                             */
    u64 as_h;          /* Address space handle (0 = kernel space)         */
    u64 wait_chan;     /* Sleep channel (0 = not waiting on a waitqueue)  */
    u64 fs_base;       /* MSR_FS_BASE (arch_prctl TLS); restored on switch */
    u64 clear_child_tid; /* set_tid_address / CLEARTID: kernel stores 0 + futex wake on exit */
    u32 state;
    u32 id;
    u32 pid;           /* User process id (0 for kernel threads)          */
    u32 tid;           /* Thread id (equals pid for single-threaded)      */
    u32 ppid;          /* Parent process id (0 = kernel / no parent)      */
    u32 is_user;       /* Runs at ring 3 between syscalls/interrupts      */
    u32 pending_sigs;  /* Bitmask: bit N set => signal N pending          */
    u32 sig_ignore;    /* Bitmask: bit N set => ignore signal N           */
    u64 sig_handler[32]; /* 0=DFL, 1=IGN, else user handler VA            */
    u64 sig_restorer[32]; /* user rt_sigreturn trampoline (may be 0)      */
    u64 sig_flags[32];   /* sa_flags (SA_RESTART=0x10000000, ...)         */
    u32 stop_sig;      /* Last stop signal (SIGTSTP/SIGSTOP)             */
    u32 stop_notify;   /* 1 => parent may reap via wait4 WUNTRACED       */
    u32 cont_notify;   /* 1 => parent may reap via wait4 WCONTINUED      */
    u32 pgid;          /* Process group id (setpgid / kill -pgid)        */
    u32 sid;           /* Session id (setsid); 0 = none                  */
    s32 nice;          /* nice(-20..19); weight derived for CFS           */
    u32 weight;        /* load weight (nice 0 => 1024)                    */
    u64 vruntime;      /* CFS virtual runtime (nice-0 units)              */
    u32 rq_cpu;        /* Home runqueue CPU (SCHED_CPU_NONE if unbound)   */
    u32 on_cpu;        /* CPU currently executing this TCB, or NONE       */
    u32 fd[FD_MAX];    /* Per-process open-file handles (VFS layer)       */
    char cwd[CWD_LEN]; /* Current working directory, NUL terminated       */
};

/* Simple sleep-mutex: wait channel is the mutex's own address. */
struct kmutex {
    u32 locked;
};

static struct tcb threads[MAX_THREADS];
enum {
    SCHED_MAX_CPUS = 8,
    SCHED_IPI_VECTOR = 34   /* fixed IPI: reschedule / signal kick */
};
static u32 cpu_current[SCHED_MAX_CPUS];
static volatile u32 idle_slot[SCHED_MAX_CPUS];
static u32 idle_mark[MAX_THREADS];   /* 1 => per-CPU idle TCB (not CFS-picked) */
static volatile u32 sched_run_lock;
static volatile u32 ap_sched_go;
static volatile u32 ap_in_sched[SCHED_MAX_CPUS];
static volatile u64 ap_idle_wakeups[SCHED_MAX_CPUS];
static volatile u64 sched_steal_count[SCHED_MAX_CPUS]; /* steals onto this CPU */
static volatile u64 sched_migrate_user;                /* user steals total   */
static volatile u64 sched_migrate_kernel;              /* kernel steals total */
static volatile u64 sched_balance_remote;              /* enqueues onto remote RQ */
static volatile u64 sched_ipi_resched;                 /* resched IPIs sent */
static u32 sched_bsp_cpu;            /* Logical CPU index of the BSP       */
static u32 active;                   /* Scheduler online flag              */
static volatile u64 sched_ticks;
static u32 next_pid = 1;             /* User process id allocator          */
static volatile u64 user_mode_ticks; /* APIC ticks taken while in ring 3   */
static u64 cfs_min_vruntime;         /* min vruntime among runnable tasks  */

extern void kprintf(const char *fmt, ...);
extern void spin_lock(volatile u32 *lock);
extern void spin_unlock(volatile u32 *lock);
extern u64 cpu_irq_save(void);
extern void cpu_irq_restore(u64 flags);
extern u32 smp_cpuid(void);
extern u32 smp_cpu_count(void);
extern u32 smp_cpu_online(u32 cpu);
extern u64 smp_cpu_stack_top(u32 cpu);
extern u32 smp_cpu_apic_id(u32 cpu);
extern void lapic_send_ipi(u32 apic_id, u32 icr_low);
extern void idt_load(void);
extern void gdt_flush(void);
extern void gdt_load_cpu_tss(u32 cpu);
extern void cpu_enable_sse(void);
extern void lapic_ap_init(void);
extern void lapic_ap_timer_enable(void);
extern u64 lapic_get_cpu_ticks(u32 cpu);
extern void cpu_pause(void);
extern void cpu_halt(void);
extern void cpu_sti(void);
extern void cpu_cli(void);

/* Per-CPU current index via LAPIC id. IRQs off or sched_run_lock held at callers. */
static u32 sched_cur(void) {
    u32 c = smp_cpuid();
    if (c >= SCHED_MAX_CPUS) {
        c = 0;
    }
    return cpu_current[c];
}

static void sched_set_cur(u32 id) {
    u32 c = smp_cpuid();
    if (c >= SCHED_MAX_CPUS) {
        c = 0;
    }
    cpu_current[c] = id;
}

static u32 sched_my_idle(void) {
    u32 c = smp_cpuid();
    if (c >= SCHED_MAX_CPUS) {
        c = 0;
    }
    return idle_slot[c];
}

static u32 tcb_is_idle(u32 id) {
    if (id >= MAX_THREADS) {
        return 1;
    }
    return idle_mark[id];
}

static u32 sched_cpu_index(void) {
    u32 c = smp_cpuid();
    if (c >= SCHED_MAX_CPUS) {
        c = 0;
    }
    return c;
}

/* Point GS at this CPU's SYSCALL conduit (kstack + user-RSP scratch). */
static void sched_load_pcpu(u32 cpu) {
    if (cpu >= SCHED_MAX_CPUS) {
        cpu = 0;
    }
    msr_write((u32)MSR_GS_BASE, (u64)(&syscall_pcpu[cpu][0]));
}

static void sched_set_kstack_cpu(u32 cpu, u64 top) {
    if (cpu >= SCHED_MAX_CPUS) {
        cpu = 0;
    }
    syscall_pcpu[cpu][0] = top;
    sched_load_pcpu(cpu);
}

/* True when `cpu` is executing its idle TCB (or has no current). */
static u32 rq_cpu_is_idle(u32 cpu) {
    u32 cur;
    if (cpu >= SCHED_MAX_CPUS) {
        return 0;
    }
    cur = cpu_current[cpu];
    if (cur >= MAX_THREADS) {
        return 1;
    }
    return idle_mark[cur];
}

/* Runnable non-idle threads queued on `cpu` (includes the running one). */
static u32 rq_load(u32 cpu) {
    u32 i;
    u32 n = 0;
    for (i = 0; i < MAX_THREADS; i++) {
        if (idle_mark[i]) {
            continue;
        }
        if (threads[i].state != TCB_RUNNABLE) {
            continue;
        }
        if (threads[i].rq_cpu != cpu) {
            continue;
        }
        n = n + 1;
    }
    return n;
}

/* Kick a remote CPU out of HLT / into pick_next via vector-34 IPI. */
static void sched_kick_cpu(u32 cpu) {
    u32 apic;
    if (cpu >= SCHED_MAX_CPUS || cpu == sched_cpu_index()) {
        return;
    }
    if (cpu != sched_bsp_cpu && ap_in_sched[cpu] == 0) {
        return;
    }
    apic = smp_cpu_apic_id(cpu);
    if (apic == 0xFFFFFFFFu) {
        return;
    }
    lapic_send_ipi(apic, (u32)SCHED_IPI_VECTOR);
    sched_ipi_resched = sched_ipi_resched + 1;
}

/* Load-aware CPU pick: prefer idle CPUs, else lowest runnable load.
 * Ties keep the local CPU. APs not yet in the scheduler are skipped. */
static u32 rq_select_cpu(void) {
    u32 n = smp_cpu_count();
    u32 local = sched_cpu_index();
    u32 best = local;
    u32 best_score = 0xFFFFFFFFu;
    u32 c;
    if (n <= 1) {
        return local;
    }
    if (n > SCHED_MAX_CPUS) {
        n = SCHED_MAX_CPUS;
    }
    for (c = 0; c < n; c++) {
        u32 load;
        u32 score;
        if (c != sched_bsp_cpu && ap_in_sched[c] == 0) {
            continue;
        }
        load = rq_load(c);
        score = load * 2;
        if (rq_cpu_is_idle(c) == 0) {
            score = score + 1;
        }
        if (score < best_score || (score == best_score && c == local)) {
            best_score = score;
            best = c;
        }
    }
    return best;
}

/* Enqueue a newly-runnable (or created) thread onto `cpu`'s runqueue. */
static void rq_enqueue(u32 id, u32 cpu) {
    if (id >= MAX_THREADS) {
        return;
    }
    if (cpu >= SCHED_MAX_CPUS) {
        cpu = 0;
    }
    threads[id].rq_cpu = cpu;
}

/* Place a new runnable thread with load-aware balance; IPI idle remotes.
 * Caller must set state=RUNNABLE first so a kicked idle CPU can see it.
 * rq_cpu is cleared before the load walk so this thread is not self-counted. */
static void rq_enqueue_balanced(u32 id) {
    u32 cpu;
    u32 local;
    if (id >= MAX_THREADS) {
        return;
    }
    local = sched_cpu_index();
    threads[id].rq_cpu = SCHED_CPU_NONE;
    cpu = rq_select_cpu();
    rq_enqueue(id, cpu);
    if (cpu != local) {
        sched_balance_remote = sched_balance_remote + 1;
        if (rq_cpu_is_idle(cpu)) {
            sched_kick_cpu(cpu);
        }
    }
}

/* After a sleeper becomes RUNNABLE, kick its home RQ if that CPU is idle. */
static void rq_kick_idle_home(u32 id) {
    u32 cpu;
    if (id >= MAX_THREADS) {
        return;
    }
    cpu = threads[id].rq_cpu;
    if (cpu >= SCHED_MAX_CPUS) {
        return;
    }
    if (rq_cpu_is_idle(cpu)) {
        sched_kick_cpu(cpu);
    }
}

/* Best CFS pick on one runqueue. want_user: 0=kernel only, 1=user only, 2=any.
 * Skips threads running on a foreign CPU (on_cpu set and != cpu). */
static u32 rq_pick_best(u32 cpu, u32 want_user) {
    u32 i;
    u32 best = SCHED_CPU_NONE;
    u64 best_vr = 0;
    for (i = 0; i < MAX_THREADS; i++) {
        if (idle_mark[i]) {
            continue;
        }
        if (threads[i].state != TCB_RUNNABLE) {
            continue;
        }
        if (threads[i].rq_cpu != cpu) {
            continue;
        }
        if (threads[i].on_cpu != SCHED_CPU_NONE && threads[i].on_cpu != cpu) {
            continue;
        }
        if (want_user == 0 && threads[i].is_user != 0) {
            continue;
        }
        if (want_user == 1 && threads[i].is_user == 0) {
            continue;
        }
        if (best == SCHED_CPU_NONE || threads[i].vruntime < best_vr) {
            best = i;
            best_vr = threads[i].vruntime;
        }
    }
    return best;
}

/* Steal one runnable task from a remote RQ onto `cpu`. Prefer kernel, then
 * user. Only takes threads that are not currently executing (on_cpu NONE). */
static u32 rq_steal(u32 cpu) {
    u32 i;
    u32 best;
    u32 n = smp_cpu_count();
    u32 pass;

    if (n > SCHED_MAX_CPUS) {
        n = SCHED_MAX_CPUS;
    }
    for (pass = 0; pass < 2; pass++) {
        best = SCHED_CPU_NONE;
        {
            u64 best_vr = 0;
            for (i = 0; i < MAX_THREADS; i++) {
                u32 rq;
                if (idle_mark[i]) {
                    continue;
                }
                if (threads[i].state != TCB_RUNNABLE) {
                    continue;
                }
                if (threads[i].on_cpu != SCHED_CPU_NONE) {
                    continue;
                }
                rq = threads[i].rq_cpu;
                if (rq == cpu || rq >= SCHED_MAX_CPUS) {
                    continue;
                }
                if (pass == 0 && threads[i].is_user != 0) {
                    continue;
                }
                if (pass == 1 && threads[i].is_user == 0) {
                    continue;
                }
                if (best == SCHED_CPU_NONE || threads[i].vruntime < best_vr) {
                    best = i;
                    best_vr = threads[i].vruntime;
                }
            }
        }
        if (best != SCHED_CPU_NONE) {
            threads[best].rq_cpu = cpu;
            sched_steal_count[cpu] = sched_steal_count[cpu] + 1;
            if (pass == 0) {
                sched_migrate_kernel = sched_migrate_kernel + 1;
            } else {
                sched_migrate_user = sched_migrate_user + 1;
            }
            return best;
        }
    }
    return SCHED_CPU_NONE;
}

enum {
    NICE_WIDTH     = 40,             /* nice -20..19                      */
    NICE0_WEIGHT   = 1024,           /* CFS weight at nice 0              */
    VRUNTIME_SLICE = 1024            /* charge per tick/yield at nice 0   */
};

/* Approximate Linux nice->weight (each step ~1.25x). */
static u32 nice_to_weight(s32 nice) {
    s32 n;
    u32 w;
    if (nice < -20) {
        nice = -20;
    }
    if (nice > 19) {
        nice = 19;
    }
    w = NICE0_WEIGHT;
    if (nice > 0) {
        for (n = 0; n < nice; n++) {
            w = (w * 4u) / 5u;
            if (w < 15u) {
                w = 15u;
                break;
            }
        }
    } else if (nice < 0) {
        for (n = 0; n > nice; n--) {
            w = (w * 5u) / 4u;
            if (w > 80000u) {
                w = 80000u;
                break;
            }
        }
    }
    return w;
}

static void cfs_set_nice_fields(struct tcb *t, s32 nice) {
    if (nice < -20) {
        nice = -20;
    }
    if (nice > 19) {
        nice = 19;
    }
    t->nice = nice;
    t->weight = nice_to_weight(nice);
}

static void cfs_charge(u32 id, u64 units) {
    u64 w;
    if (id >= MAX_THREADS || tcb_is_idle(id)) {
        return;
    }
    if (threads[id].state == TCB_FREE || threads[id].state == TCB_DEAD) {
        return;
    }
    w = threads[id].weight;
    if (w == 0) {
        w = NICE0_WEIGHT;
    }
    threads[id].vruntime = threads[id].vruntime + (units * (u64)NICE0_WEIGHT) / w;
}

static void cfs_place_entity(u32 id) {
    if (id >= MAX_THREADS || tcb_is_idle(id)) {
        return;
    }
    if (threads[id].vruntime < cfs_min_vruntime) {
        threads[id].vruntime = cfs_min_vruntime;
    }
}

static void cfs_update_min(void) {
    u32 i;
    u64 minv = 0xFFFFFFFFFFFFFFFFULL;
    u32 found = 0;
    for (i = 1; i < MAX_THREADS; i++) {
        if (idle_mark[i]) {
            continue;
        }
        if (threads[i].state == TCB_RUNNABLE) {
            if (!found || threads[i].vruntime < minv) {
                minv = threads[i].vruntime;
                found = 1;
            }
        }
    }
    if (found) {
        cfs_min_vruntime = minv;
    }
}

/* ---------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------- */

u32 sched_is_active(void) {
    return active;
}

void sched_init(void) {
    u32 i;
    for (i = 0; i < MAX_THREADS; i++) {
        u32 j;
        threads[i].state = TCB_FREE;
        threads[i].rsp = 0;
        threads[i].stack_phys = 0;
        threads[i].kstack_top = 0;
        threads[i].entry = 0;
        threads[i].arg = 0;
        threads[i].as_h = 0;
        threads[i].wait_chan = 0;
        threads[i].fs_base = 0;
        threads[i].clear_child_tid = 0;
        threads[i].pid = 0;
        threads[i].tid = 0;
        threads[i].ppid = 0;
        threads[i].is_user = 0;
        threads[i].pending_sigs = 0;
        threads[i].stop_sig = 0;
        threads[i].stop_notify = 0;
        threads[i].cont_notify = 0;
        threads[i].pgid = 0;
        threads[i].sid = 0;
        threads[i].nice = 0;
        threads[i].weight = NICE0_WEIGHT;
        threads[i].vruntime = 0;
        threads[i].rq_cpu = SCHED_CPU_NONE;
        threads[i].on_cpu = SCHED_CPU_NONE;
        threads[i].sig_ignore = (1u << 17); /* SIGCHLD ignored by default */
        {
            u32 s;
            for (s = 0; s < 32; s++) {
                threads[i].sig_handler[s] = 0;
                threads[i].sig_restorer[s] = 0;
                threads[i].sig_flags[s] = 0;
            }
        }
        for (j = 0; j < FD_MAX; j++) {
            threads[i].fd[j] = FD_EMPTY;
        }
        threads[i].cwd[0] = '/';
        threads[i].cwd[1] = 0;
        threads[i].id = i;
        idle_mark[i] = 0;
    }
    /* Slot 0 is the boot/init context (kmain). Its RSP is captured lazily
     * on the first switch away. */
    threads[0].state = TCB_RUNNABLE;
    threads[0].id = 0;
    idle_mark[0] = 1;
    for (i = 0; i < SCHED_MAX_CPUS; i++) {
        cpu_current[i] = 0;
        idle_slot[i] = 0xFFFFFFFF;
        ap_in_sched[i] = 0;
        ap_idle_wakeups[i] = 0;
        sched_steal_count[i] = 0;
        syscall_pcpu[i][0] = 0;
        syscall_pcpu[i][1] = 0;
    }
    sched_migrate_user = 0;
    sched_migrate_kernel = 0;
    sched_balance_remote = 0;
    sched_ipi_resched = 0;
    sched_bsp_cpu = smp_cpuid();
    if (sched_bsp_cpu >= SCHED_MAX_CPUS) {
        sched_bsp_cpu = 0;
    }
    idle_slot[sched_bsp_cpu] = 0;
    cpu_current[sched_bsp_cpu] = 0;
    threads[0].rq_cpu = sched_bsp_cpu;
    threads[0].on_cpu = sched_bsp_cpu;
    sched_set_cur(0);
    sched_run_lock = 0;
    ap_sched_go = 0;
    sched_ticks = 0;
    user_mode_ticks = 0;
    cfs_min_vruntime = 0;
    /* Until the first user thread runs, SYSCALL/trap entries use the boot
     * RSP0 stack programmed by gdt_init(). GS points at this CPU's conduit. */
    sched_set_kstack_cpu(sched_bsp_cpu, arch.gdt_get_rsp0());
    active = 1;
}

/* CFS-lite with per-CPU runqueues: pick lowest-vruntime runnable on this CPU's
 * RQ. If the local RQ is empty, steal from remote RQs — kernel threads first,
 * then user — and migrate the victim onto this CPU. Idle TCBs are never
 * CFS-picked. */
static u32 pick_next(u32 from) {
    u32 best;
    u32 cpu = sched_cpu_index();
    u32 my_idle;
    (void)from;
    my_idle = idle_slot[cpu];
    if (my_idle >= MAX_THREADS) {
        my_idle = 0;
    }
    best = rq_pick_best(cpu, 2);
    if (best != SCHED_CPU_NONE) {
        return best;
    }
    best = rq_steal(cpu);
    if (best != SCHED_CPU_NONE) {
        return best;
    }
    return my_idle;
}

/* ---------------------------------------------------------------------------
 * Thread creation
 * ------------------------------------------------------------------------- */

/* Per-process descriptor table helpers. User processes start with fd 0/1/2
 * bound to the permanent console open-file; forked children inherit the whole
 * table sharing the same open-file descriptions (fs layer refcounted). */
static void fd_table_clear(struct tcb *t) {
    u32 i;
    for (i = 0; i < FD_MAX; i++) {
        t->fd[i] = FD_EMPTY;
    }
}

static void fd_table_install_console(struct tcb *t) {
    fd_table_clear(t);
    t->fd[0] = fs_console_fh();
    t->fd[1] = fs_console_fh();
    t->fd[2] = fs_console_fh();
}

static void cwd_set_root(struct tcb *t) {
    t->cwd[0] = '/';
    t->cwd[1] = 0;
}

static void cwd_copy_from(struct tcb *dst, const char *src) {
    u32 i = 0;
    while (src[i] != 0 && i < CWD_LEN - 1) {
        dst->cwd[i] = src[i];
        i++;
    }
    dst->cwd[i] = 0;
}

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
    threads[i].wait_chan = 0;
    threads[i].fs_base = 0;
    threads[i].pid = 0;
    threads[i].tid = 0;
    threads[i].ppid = 0;
    threads[i].is_user = 0;
    threads[i].pending_sigs = 0;
    threads[i].sig_ignore = 0;
    fd_table_clear(&threads[i]);
    cwd_set_root(&threads[i]);
    cfs_set_nice_fields(&threads[i], 0);
    threads[i].vruntime = cfs_min_vruntime;
    threads[i].on_cpu = SCHED_CPU_NONE;
    threads[i].state = TCB_RUNNABLE;
    rq_enqueue_balanced(i);
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
    threads[i].wait_chan = 0;
    threads[i].fs_base = 0;
    threads[i].pid = pid;
    threads[i].tid = pid;              /* leader: tid == pid */
    threads[i].ppid = 0;
    threads[i].pgid = pid;             /* new session/pgrp for pid 1 etc. */
    threads[i].sid = pid;
    threads[i].stop_sig = 0;
    threads[i].stop_notify = 0;
    threads[i].cont_notify = 0;
    threads[i].is_user = 1;
    threads[i].pending_sigs = 0;
    threads[i].sig_ignore = (1u << 17);
    {
        u32 s;
        for (s = 0; s < 32; s++) {
            threads[i].sig_handler[s] = 0;
            threads[i].sig_restorer[s] = 0;
            threads[i].sig_flags[s] = 0;
        }
    }
    fd_table_install_console(&threads[i]);
    cwd_set_root(&threads[i]);
    cfs_set_nice_fields(&threads[i], 0);
    threads[i].vruntime = cfs_min_vruntime;
    threads[i].on_cpu = SCHED_CPU_NONE;
    threads[i].state = TCB_RUNNABLE;
    rq_enqueue_balanced(i);
    return i;
}

/* fork(): clone the currently running user thread. The parent's complete
 * syscall trap frame is copied onto the child's kernel stack: the 128-byte
 * GPR frame plus the 264-byte user-RSP pad and XMM save area the epilogue
 * reads on its way back to user space. The child first resumes in
 * fork_ret_trampoline, which returns 0 to user space. */
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

    /* Trap frame: 15 pushed GPRs (120) + user-RSP pad (8) + XMM area (256),
     * exactly the region syscall_entry lays out below the stack top. */
    enum { FRAME_QWORDS = (120 + 8 + 256) / 8 };
    u64 fbase = top - 384;
    u64 *dst = (u64 *)(fbase);
    u64 *src = (u64 *)frame_base;
    u32 k;
    for (k = 0; k < FRAME_QWORDS; k++) {
        dst[k] = src[k];
    }

    /* switch_context frame below the copied syscall frame: r12 = frame base */
    u64 *frame = (u64 *)(fbase - 56);
    frame[0] = 0;                                   /* r15 */
    frame[1] = 0;                                   /* r14 */
    frame[2] = 0;                                   /* r13 */
    frame[3] = fbase;                               /* r12 = frame base */
    frame[4] = 0;                                   /* rbx */
    frame[5] = 0;                                   /* rbp */
    frame[6] = (u64)(&fork_ret_trampoline);         /* return address */

    threads[i].rsp = fbase - 56;
    threads[i].stack_phys = stack_phys;
    threads[i].kstack_top = top;
    threads[i].entry = 0;
    threads[i].arg = 0;
    threads[i].as_h = child_as_h;
    threads[i].wait_chan = 0;
    threads[i].fs_base = threads[sched_cur()].fs_base; /* fork inherits TLS base */
    threads[i].clear_child_tid = 0; /* Linux: child does not inherit CLEARTID */
    threads[i].pid = child_pid;
    threads[i].tid = child_pid;        /* new process leader */
    threads[i].ppid = threads[sched_cur()].pid;
    threads[i].pgid = threads[sched_cur()].pgid;
    threads[i].sid = threads[sched_cur()].sid;
    threads[i].stop_sig = 0;
    threads[i].stop_notify = 0;
    threads[i].cont_notify = 0;
    threads[i].is_user = 1;
    {
        u32 f;
        for (f = 0; f < FD_MAX; f++) {
            u32 fh = threads[sched_cur()].fd[f];
            threads[i].fd[f] = fh;
            if (fh != FD_EMPTY) {
                /* Shared open-file description; console retain is a no-op. */
                if (ext2_fh_is(fh)) {
                    ext2_file_retain(fh);
                } else if (pipe_fh_is(fh)) {
                    pipe_retain(fh);
                } else if (sock_fh_is(fh)) {
                    sock_retain(fh);
                } else {
                    fs_file_retain(fh);
                }
            }
        }
    }
    cwd_copy_from(&threads[i], threads[sched_cur()].cwd);
    cfs_set_nice_fields(&threads[i], threads[sched_cur()].nice);
    threads[i].vruntime = cfs_min_vruntime;
    threads[i].pending_sigs = 0;
    threads[i].sig_ignore = threads[sched_cur()].sig_ignore;
    {
        u32 s;
        for (s = 0; s < 32; s++) {
            threads[i].sig_handler[s] = threads[sched_cur()].sig_handler[s];
            threads[i].sig_flags[s] = threads[sched_cur()].sig_flags[s];
            threads[i].sig_restorer[s] = threads[sched_cur()].sig_restorer[s];
        }
    }
    threads[i].on_cpu = SCHED_CPU_NONE;
    threads[i].state = TCB_RUNNABLE;
    rq_enqueue_balanced(i);
    return i;
}

/* clone(CLONE_VM|CLONE_THREAD): new user thread sharing the caller's address
 * space. Lands in user_iret_trampoline with R12=fn, RBX=user_stack, R13=arg
 * (loaded into RDI before IRET). Same pid/pgid/sid; fresh tid. FD table is
 * copied with retain (CLONE_FILES-lite: shared open-file descriptions). */
u32 sched_clone_thread(u64 fn, u64 user_stack, u64 arg, u32 child_tid) {
    u32 i = alloc_slot();
    u32 f;
    u32 s;
    if (i == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }
    if (threads[sched_cur()].as_h == 0 || threads[sched_cur()].is_user == 0) {
        return 0xFFFFFFFF;
    }
    if (fn == 0 || user_stack == 0) {
        return 0xFFFFFFFF;
    }

    u64 stack_phys = pmm_alloc_pages(STACK_ORDER);
    if (stack_phys == 0) {
        return 0xFFFFFFFF;
    }
    u64 top = 0xFFFF800000000000ULL + stack_phys + STACK_BYTES;

    /* switch_context frame: r12=fn, rbx=stack, r13=arg -> user_iret. */
    u64 *frame = (u64 *)(top - 56);
    frame[0] = 0;                                   /* r15 */
    frame[1] = 0;                                   /* r14 */
    frame[2] = arg;                                 /* r13 = start arg */
    frame[3] = fn;                                  /* r12 = start RIP */
    frame[4] = user_stack;                          /* rbx = user RSP */
    frame[5] = 0;                                   /* rbp */
    frame[6] = (u64)(&user_iret_trampoline);        /* return address */

    threads[i].rsp = top - 56;
    threads[i].stack_phys = stack_phys;
    threads[i].kstack_top = top;
    threads[i].entry = fn;
    threads[i].arg = arg;
    threads[i].as_h = threads[sched_cur()].as_h;
    threads[i].wait_chan = 0;
    threads[i].fs_base = 0;            /* child installs TLS via arch_prctl */
    threads[i].clear_child_tid = 0;    /* child registers via set_tid_address */
    threads[i].pid = threads[sched_cur()].pid;
    threads[i].tid = child_tid;
    threads[i].ppid = threads[sched_cur()].ppid;
    threads[i].pgid = threads[sched_cur()].pgid;
    threads[i].sid = threads[sched_cur()].sid;
    threads[i].stop_sig = 0;
    threads[i].stop_notify = 0;
    threads[i].cont_notify = 0;
    threads[i].is_user = 1;
    for (f = 0; f < FD_MAX; f++) {
        u32 fh = threads[sched_cur()].fd[f];
        threads[i].fd[f] = fh;
        if (fh != FD_EMPTY) {
            if (ext2_fh_is(fh)) {
                ext2_file_retain(fh);
            } else if (pipe_fh_is(fh)) {
                pipe_retain(fh);
            } else if (sock_fh_is(fh)) {
                sock_retain(fh);
            } else {
                fs_file_retain(fh);
            }
        }
    }
    cwd_copy_from(&threads[i], threads[sched_cur()].cwd);
    cfs_set_nice_fields(&threads[i], threads[sched_cur()].nice);
    threads[i].vruntime = cfs_min_vruntime;
    threads[i].pending_sigs = 0;
    threads[i].sig_ignore = threads[sched_cur()].sig_ignore;
    for (s = 0; s < 32; s++) {
        threads[i].sig_handler[s] = threads[sched_cur()].sig_handler[s];
        threads[i].sig_flags[s] = threads[sched_cur()].sig_flags[s];
        threads[i].sig_restorer[s] = threads[sched_cur()].sig_restorer[s];
    }
    threads[i].on_cpu = SCHED_CPU_NONE;
    threads[i].state = TCB_RUNNABLE;
    rq_enqueue_balanced(i);
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
    u32 cpu = sched_cpu_index();
    u64 top = threads[next].kstack_top;
    if (top == 0) {
        top = arch.gdt_get_rsp0();      /* Slot 0 (boot/idle context) */
    }
    arch.gdt_set_rsp0(top);
    sched_set_kstack_cpu(cpu, top);
    if (threads[next].as_h != threads[prev].as_h) {
        mem.as_activate_h(threads[next].as_h);
    }
    /* Per-thread TLS: install next FS base before the stack switch so the
     * resumed thread sees its arch_prctl value (0 clears FS). */
    msr_write((u32)MSR_FS_BASE, threads[next].fs_base);
    /* Drop the run lock before the stack swap so a freshly trampolined
     * thread is not born holding sched_run_lock. */
    spin_unlock(&sched_run_lock);
    switch_context(&threads[prev].rsp, threads[next].rsp);
}

/* Caller must hold sched_run_lock (IRQs off). On return the lock is released. */
static void sched_switch_to(u32 next) {
    u32 cpu = sched_cpu_index();
    u32 prev;
    if (next == sched_cur()) {
        spin_unlock(&sched_run_lock);
        return;
    }
    prev = sched_cur();
    threads[prev].on_cpu = SCHED_CPU_NONE;
    threads[next].on_cpu = cpu;
    threads[next].rq_cpu = cpu;
    sched_set_cur(next);
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
    spin_lock(&sched_run_lock);
    cfs_charge(sched_cur(), VRUNTIME_SLICE);
    cfs_update_min();
    sched_switch_to(pick_next(sched_cur()));
    arch.cpu_sti();
}

/* Block the current thread on `chan` until sched_wakeup(chan). Interrupts
 * are taken around the switch; the caller does not hold a lock across the
 * sleep (classic Mesa-style: re-check the wait condition after return). */
void sched_sleep(u64 chan) {
    if (!active || chan == 0) {
        return;
    }
    arch.cpu_cli();
    spin_lock(&sched_run_lock);
    threads[sched_cur()].wait_chan = chan;
    threads[sched_cur()].state = TCB_SLEEPING;
    sched_switch_to(pick_next(sched_cur()));
    /* Woken: state is RUNNABLE and wait_chan cleared by wakeup. */
    arch.cpu_sti();
}

/* Like sched_sleep but IRQs are already off and stay off across the return
 * (used by wait4 to close the lost-wakeup race under a condition check). */
void sched_sleep_cli(u64 chan) {
    if (!active || chan == 0) {
        return;
    }
    spin_lock(&sched_run_lock);
    threads[sched_cur()].wait_chan = chan;
    threads[sched_cur()].state = TCB_SLEEPING;
    sched_switch_to(pick_next(sched_cur()));
}

/* Make every sleeper on `chan` runnable. Safe to call with IRQs off or on. */
void sched_wakeup(u64 chan) {
    u32 i;
    u64 flags;
    u32 kick_mask;
    if (chan == 0) {
        return;
    }
    kick_mask = 0;
    flags = cpu_irq_save();
    spin_lock(&sched_run_lock);
    for (i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == TCB_SLEEPING && threads[i].wait_chan == chan) {
            u32 cpu;
            threads[i].wait_chan = 0;
            cfs_place_entity(i);
            threads[i].state = TCB_RUNNABLE;
            cpu = threads[i].rq_cpu;
            if (cpu < SCHED_MAX_CPUS && rq_cpu_is_idle(cpu)) {
                kick_mask = kick_mask | (1u << cpu);
            }
        }
    }
    cfs_update_min();
    spin_unlock(&sched_run_lock);
    if (kick_mask != 0) {
        for (i = 0; i < SCHED_MAX_CPUS; i++) {
            if ((kick_mask & (1u << i)) != 0) {
                sched_kick_cpu(i);
            }
        }
    }
    cpu_irq_restore(flags);
}

/* Wake up to `n` sleepers on `chan` (futex FUTEX_WAKE). Returns woken count. */
u32 sched_wakeup_n(u64 chan, u32 n) {
    u32 i;
    u32 woke = 0;
    u64 flags;
    u32 kick_mask;
    if (chan == 0 || n == 0) {
        return 0;
    }
    kick_mask = 0;
    flags = cpu_irq_save();
    spin_lock(&sched_run_lock);
    for (i = 0; i < MAX_THREADS && woke < n; i++) {
        if (threads[i].state == TCB_SLEEPING && threads[i].wait_chan == chan) {
            u32 cpu;
            threads[i].wait_chan = 0;
            cfs_place_entity(i);
            threads[i].state = TCB_RUNNABLE;
            cpu = threads[i].rq_cpu;
            if (cpu < SCHED_MAX_CPUS && rq_cpu_is_idle(cpu)) {
                kick_mask = kick_mask | (1u << cpu);
            }
            woke++;
        }
    }
    if (woke != 0) {
        cfs_update_min();
    }
    spin_unlock(&sched_run_lock);
    if (kick_mask != 0) {
        for (i = 0; i < SCHED_MAX_CPUS; i++) {
            if ((kick_mask & (1u << i)) != 0) {
                sched_kick_cpu(i);
            }
        }
    }
    cpu_irq_restore(flags);
    return woke;
}

/* Channel used by wait4 so an exiting child can wake its parent. */
u64 sched_wait4_chan(u32 parent_pid) {
    return 0x5774000000000000ULL | (u64)parent_pid;
}

void kmutex_init(struct kmutex *m) {
    m->locked = 0;
}

void kmutex_lock(struct kmutex *m) {
    arch.cpu_cli();
    spin_lock(&sched_run_lock);
    while (m->locked) {
        threads[sched_cur()].wait_chan = (u64)m;
        threads[sched_cur()].state = TCB_SLEEPING;
        sched_switch_to(pick_next(sched_cur()));
        spin_lock(&sched_run_lock);
    }
    m->locked = 1;
    spin_unlock(&sched_run_lock);
    arch.cpu_sti();
}

void kmutex_unlock(struct kmutex *m) {
    arch.cpu_cli();
    spin_lock(&sched_run_lock);
    m->locked = 0;
    {
        u32 i;
        u32 kick_mask = 0;
        u64 chan = (u64)m;
        for (i = 0; i < MAX_THREADS; i++) {
            if (threads[i].state == TCB_SLEEPING && threads[i].wait_chan == chan) {
                u32 cpu;
                threads[i].wait_chan = 0;
                cfs_place_entity(i);
                threads[i].state = TCB_RUNNABLE;
                cpu = threads[i].rq_cpu;
                if (cpu < SCHED_MAX_CPUS && rq_cpu_is_idle(cpu)) {
                    kick_mask = kick_mask | (1u << cpu);
                }
            }
        }
        cfs_update_min();
        spin_unlock(&sched_run_lock);
        if (kick_mask != 0) {
            for (i = 0; i < SCHED_MAX_CPUS; i++) {
                if ((kick_mask & (1u << i)) != 0) {
                    sched_kick_cpu(i);
                }
            }
        }
    }
    arch.cpu_sti();
}

/* Called from the Local APIC timer ISR (vector 32) with interrupts off in the
 * interrupt gate. EOI first so the APIC keeps firing after we switch stacks. */
void sched_tick(void) {
    arch.lapic_irq();                    /* EOI + raw tick counter bump */
    if (!active) {
        return;
    }
    spin_lock(&sched_run_lock);
    sched_ticks++;
    cfs_charge(sched_cur(), VRUNTIME_SLICE);
    cfs_update_min();
    sched_switch_to(pick_next(sched_cur()));
}

/* Terminate the sched_cur() thread. Does not return. */
void kthread_exit(void) {
    /* No preemption after this point: the stack we stand on is about to be
     * returned to the buddy allocator. */
    arch.cpu_cli();
    spin_lock(&sched_run_lock);

    u32 self = sched_cur();
    u32 free_slot = 0;
    /* Secondary CLONE_THREAD exits are not wait4-able: free the TCB now so
     * the shared pid stays owned by the still-live group leader. */
    if (threads[self].is_user != 0 && threads[self].pid != 0
        && threads[self].tid != threads[self].pid) {
        free_slot = 1;
    }
    threads[self].state = free_slot ? TCB_FREE : TCB_DEAD;
    if (free_slot) {
        threads[self].pid = 0;
        threads[self].tid = 0;
        threads[self].ppid = 0;
        threads[self].is_user = 0;
    }
    u64 sp = threads[self].stack_phys;
    threads[self].stack_phys = 0;
    threads[self].rsp = 0;
    if (sp != 0) {
        pmm_free_pages(sp, STACK_ORDER);
    }

    u32 next = pick_next(self);
    if (next == self) {
        threads[self].on_cpu = SCHED_CPU_NONE;
        spin_unlock(&sched_run_lock);
        for (;;) {
            arch.cpu_sti();
            arch.cpu_halt();
            arch.cpu_cli();
        }
    }
    {
        u32 cpu = sched_cpu_index();
        threads[self].on_cpu = SCHED_CPU_NONE;
        threads[next].on_cpu = cpu;
        threads[next].rq_cpu = cpu;
        sched_set_cur(next);
    }
    schedule_tail(self, next);
    for (;;) {
        arch.cpu_sti();
        arch.cpu_halt();
        arch.cpu_cli();
    }
}

/* nice(2): add `inc` to the calling thread's nice (-20..19). Returns new nice
 * or (u32)-1 on range error after clamp attempt. Also used by selftests via
 * sched_set_nice. */
s32 sched_get_nice(u32 id) {
    if (id >= MAX_THREADS || threads[id].state == TCB_FREE) {
        return 0;
    }
    return threads[id].nice;
}

u32 sched_set_nice(u32 id, s32 nice) {
    if (id >= MAX_THREADS || threads[id].state == TCB_FREE
        || threads[id].state == TCB_DEAD) {
        return 0;
    }
    cfs_set_nice_fields(&threads[id], nice);
    return 1;
}

u64 sched_vruntime(u32 id) {
    if (id >= MAX_THREADS) {
        return 0;
    }
    return threads[id].vruntime;
}

/* int nice(int inc): Linux nr 89. */
u64 sys_nice(u64 inc) {
    s32 cur = threads[sched_cur()].nice;
    s64 next = (s64)cur + (s64)(s32)inc;
    if (next < -20) {
        next = -20;
    }
    if (next > 19) {
        next = 19;
    }
    cfs_set_nice_fields(&threads[sched_cur()], (s32)next);
    return (u64)(s64)threads[sched_cur()].nice;
}

/* ---------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

u64 sched_tick_count(void) {
    return sched_ticks;
}

/* arch_prctl TLS: SET_FS updates TCB + live MSR; GET_FS reads TCB. */
void sched_set_fs_base(u64 base) {
    threads[sched_cur()].fs_base = base;
    msr_write((u32)MSR_FS_BASE, base);
}

u64 sched_get_fs_base(void) {
    return threads[sched_cur()].fs_base;
}

/* set_tid_address(2): remember user VA cleared+woken on this thread's exit. */
void sched_set_clear_child_tid(u64 addr) {
    threads[sched_cur()].clear_child_tid = addr;
}

u64 sched_get_clear_child_tid(void) {
    return threads[sched_cur()].clear_child_tid;
}

/* CLONE_CHILD_CLEARTID-style: *ctid = 0 and FUTEX_WAKE. Call while the
 * doomed thread's AS is still active (before as_activate_h(0)). */
void sched_do_clear_child_tid(void) {
    u64 addr;
    u64 as;
    addr = threads[sched_cur()].clear_child_tid;
    threads[sched_cur()].clear_child_tid = 0;
    if (addr == 0 || (addr & 3ULL) != 0) {
        return;
    }
    as = threads[sched_cur()].as_h;
    if (as == 0 || !mem.as_user_range_ok(as, addr, 4)) {
        return;
    }
    *(volatile u32 *)addr = 0;
    sched_wakeup_n(addr, 0x7fffffff);
}

u32 sched_current_id(void) {
    return sched_cur();
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
    return threads[sched_cur()].pid;
}

u32 sched_current_tid(void) {
    return threads[sched_cur()].tid;
}

/* Live user threads that share `pid` (CLONE_THREAD group). */
u32 sched_count_live_pid(u32 pid) {
    u32 i;
    u32 n = 0;
    if (pid == 0) {
        return 0;
    }
    for (i = 1; i < MAX_THREADS; i++) {
        if (threads[i].pid == pid
            && threads[i].state != TCB_FREE
            && threads[i].state != TCB_DEAD) {
            n++;
        }
    }
    return n;
}

/* True when this TCB is a non-leader thread (tid != pid). */
u32 sched_is_secondary_thread(void) {
    if (threads[sched_cur()].pid == 0) {
        return 0;
    }
    return threads[sched_cur()].tid != threads[sched_cur()].pid ? 1 : 0;
}

u64 sched_current_as_h(void) {
    return threads[sched_cur()].as_h;
}

/* execve() swaps the address space of the running thread in place. CR3 is
 * switched by the caller; subsequent context switches use the new handle. */
void sched_replace_as(u64 new_as_h) {
    threads[sched_cur()].as_h = new_as_h;
}

/* Called from the timer ISR path when a tick was taken while the interrupted
 * context was ring 3 (CS & 3): proves ring-3 threads are truly preemptible. */
void sched_note_user_tick(void) {
    user_mode_ticks++;
}

u64 sched_user_tick_count(void) {
    return user_mode_ticks;
}

/* ---------------------------------------------------------------------------
 * Per-process file descriptors and working directory
 * ------------------------------------------------------------------------- */

u32 sched_fd_get(u32 idx) {
    if (idx >= FD_MAX) {
        return FD_EMPTY;
    }
    return threads[sched_cur()].fd[idx];
}

void sched_fd_set(u32 idx, u32 fh) {
    if (idx < FD_MAX) {
        threads[sched_cur()].fd[idx] = fh;
    }
}

/* Lowest empty descriptor; POSIX allocates the smallest available number. */
u32 sched_fd_alloc(void) {
    u32 i;
    for (i = 0; i < FD_MAX; i++) {
        if (threads[sched_cur()].fd[i] == FD_EMPTY) {
            return i;
        }
    }
    return FD_EMPTY;
}

/* Close every descriptor of the running process; called from proc_terminate
 * before the address space is torn down. */
void sched_fd_close_all(void) {
    u32 i;
    for (i = 0; i < FD_MAX; i++) {
        if (threads[sched_cur()].fd[i] != FD_EMPTY) {
            u32 fh = threads[sched_cur()].fd[i];
            if (ext2_fh_is(fh)) {
                ext2_close(fh);
            } else if (pipe_fh_is(fh)) {
                pipe_close(fh);
            } else if (sock_fh_is(fh)) {
                sock_close(fh);
            } else {
                fs_close(fh);
            }
            threads[sched_cur()].fd[i] = FD_EMPTY;
        }
    }
}

const char *sched_cwd(void) {
    return threads[sched_cur()].cwd;
}

void sched_cwd_set(const char *kpath) {
    cwd_copy_from(&threads[sched_cur()], kpath);
}

u32 sched_current_ppid(void) {
    return threads[sched_cur()].ppid;
}

/* True when a live (neither exited nor freed) child TCB of ppid exists. The
 * exit record alone is enough for wait once the child is DEAD. */
u32 sched_has_live_child(u32 ppid) {
    u32 i;
    for (i = 1; i < MAX_THREADS; i++) {
        if (threads[i].pid != 0 && threads[i].ppid == ppid
            && threads[i].state != TCB_DEAD && threads[i].state != TCB_FREE) {
            return 1;
        }
    }
    return 0;
}

/* Exact-pid variant for wait4(pid): distinguishes "this specific child is
 * still running" from "caller has some unrelated children". */
u32 sched_is_live_child(u32 ppid, u32 pid) {
    u32 i;
    for (i = 1; i < MAX_THREADS; i++) {
        if (threads[i].pid == pid && threads[i].ppid == ppid
            && threads[i].state != TCB_DEAD && threads[i].state != TCB_FREE) {
            return 1;
        }
    }
    return 0;
}

/* Release the DEAD slot of a successfully waited-for pid. The kernel stack
 * was already freed in kthread_exit; only the slot identity remains. */
u32 sched_reap_dead(u32 pid) {
    u32 i;
    for (i = 1; i < MAX_THREADS; i++) {
        if (threads[i].pid == pid && threads[i].state == TCB_DEAD) {
            threads[i].state = TCB_FREE;
            threads[i].pid = 0;
            threads[i].tid = 0;
            threads[i].ppid = 0;
            threads[i].pgid = 0;
            threads[i].sid = 0;
            threads[i].stop_notify = 0;
            threads[i].cont_notify = 0;
            threads[i].is_user = 0;
            threads[i].pending_sigs = 0;
            threads[i].sig_ignore = 0;
            threads[i].stop_sig = 0;
            threads[i].stop_notify = 0;
            return 1;
        }
    }
    return 0;
}

/* Reparent live children when their parent exits; orphans join pid 1. */
void sched_reparent_children(u32 old_ppid, u32 new_ppid) {
    u32 i;
    for (i = 1; i < MAX_THREADS; i++) {
        if (threads[i].pid != 0 && threads[i].ppid == old_ppid) {
            threads[i].ppid = new_ppid;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Minimal POSIX-ish signals (default actions only).
 * SIGINT=2 SIGKILL=9 SIGTERM=15 SIGCHLD=17
 * SIGCONT=18 SIGSTOP=19 SIGTSTP=20 SIGTTIN=21 SIGTTOU=22
 * ------------------------------------------------------------------------- */

enum {
    SIGINT_N  = 2,
    SIGKILL_N = 9,
    SIGTERM_N = 15,
    SIGCHLD_N = 17,
    SIGCONT_N = 18,
    SIGSTOP_N = 19,
    SIGTSTP_N = 20,
    SIGTTIN_N = 21,
    SIGTTOU_N = 22
};

extern void proc_terminate(u64 code);


/* sigaction: SIG_DFL(0) / SIG_IGN(1) / custom handler VA.
 * Optional restorer is the user trampoline invoked on handler return
 * (typically libc's rt_sigreturn stub). SIGKILL/SIGSTOP immutable. */
u32 sched_sig_get(u32 sig, u64 *handler_out) {
    if (sig == 0 || sig > 31) {
        return (u32)-1;
    }
    if (handler_out) {
        *handler_out = threads[sched_cur()].sig_handler[sig];
    }
    return 0;
}

u32 sched_sig_get_flags(u32 sig, u64 *flags_out) {
    if (sig == 0 || sig > 31) {
        return (u32)-1;
    }
    if (flags_out) {
        *flags_out = threads[sched_cur()].sig_flags[sig];
    }
    return 0;
}

u32 sched_sig_ignored(u32 sig) {
    if (sig == 0 || sig > 31) {
        return 0;
    }
    if (threads[sched_cur()].sig_handler[sig] == 1) {
        return 1;
    }
    if ((threads[sched_cur()].sig_ignore & (1u << sig)) != 0) {
        return 1;
    }
    return 0;
}

enum { SA_RESTART_F = 0x10000000u };

u32 sched_sigaction(u32 sig, u64 handler, u64 restorer, u64 flags, u64 *old_out,
                    u64 *old_flags_out) {
    u64 old = 0;
    u64 oldf = 0;
    if (sig == 0 || sig > 31 || sig == SIGKILL_N || sig == 19) {
        return (u32)-1;
    }
    old = threads[sched_cur()].sig_handler[sig];
    oldf = threads[sched_cur()].sig_flags[sig];
    if (old_out) {
        *old_out = old;
    }
    if (old_flags_out) {
        *old_flags_out = oldf;
    }
    threads[sched_cur()].sig_handler[sig] = handler;
    threads[sched_cur()].sig_restorer[sig] = restorer;
    threads[sched_cur()].sig_flags[sig] = flags;
    if (handler == 1) {
        threads[sched_cur()].sig_ignore |= (1u << sig);
    } else {
        threads[sched_cur()].sig_ignore &= ~(1u << sig);
    }
    return 0;
}

/* True when a pending signal should abort an interruptible sleep with EINTR.
 * Custom handlers with SA_RESTART leave the syscall blocked (handler runs
 * when the call eventually returns); without SA_RESTART they interrupt. */
u32 sched_has_pending_fatal(void);

u32 sched_should_eintr(void) {
    u32 pend;
    u32 ign;
    u32 sig;
    if (!threads[sched_cur()].is_user) {
        return 0;
    }
    if (sched_has_pending_fatal()) {
        return 1;
    }
    pend = threads[sched_cur()].pending_sigs;
    ign = threads[sched_cur()].sig_ignore;
    pend &= ~ign;
    for (sig = 1; sig <= 31; sig++) {
        u64 h;
        if ((pend & (1u << sig)) == 0) continue;
        if (sig == SIGKILL_N) return 1;
        h = threads[sched_cur()].sig_handler[sig];
        if (h > 1) {
            if ((threads[sched_cur()].sig_flags[sig] & SA_RESTART_F) == 0) {
                return 1;
            }
            continue; /* SA_RESTART: keep sleeping */
        }
        /* Default action that stops a wait/read: INT/TERM already covered by
         * has_pending_fatal; other defaults are ignored for EINTR. */
    }
    return 0;
}

u32 sched_signal_send(u32 pid, u32 sig) {
    u32 i;
    u32 found = 0;
    if (sig > 31) {
        return 0;
    }
    for (i = 0; i < MAX_THREADS; i++) {
        if (threads[i].pid == pid && threads[i].state != TCB_FREE
            && threads[i].state != TCB_DEAD) {
            found = 1;
            if (sig == 0) {
                continue;               /* existence probe */
            }
            /* SIGCONT: resume stopped tasks even when ignored; notify waiters. */
            if (sig == SIGCONT_N) {
                if (threads[i].state == TCB_STOPPED) {
                    threads[i].state = TCB_RUNNABLE;
                    threads[i].wait_chan = 0;
                    threads[i].cont_notify = 1;
                    rq_kick_idle_home(i);
                    if (threads[i].ppid != 0) {
                        sched_wakeup(sched_wait4_chan(threads[i].ppid));
                    }
                }
                threads[i].pending_sigs &= ~((1u << SIGSTOP_N) | (1u << SIGTSTP_N)
                                             | (1u << SIGTTIN_N) | (1u << SIGTTOU_N));
            }
            threads[i].pending_sigs |= (1u << sig);
            /* Wake sleepers so they can observe stop/kill/cont. */
            if (threads[i].state == TCB_SLEEPING) {
                threads[i].wait_chan = 0;
                threads[i].state = TCB_RUNNABLE;
                rq_kick_idle_home(i);
            }
            /* KILL must not leave a process stuck STOPPED. */
            if (sig == SIGKILL_N && threads[i].state == TCB_STOPPED) {
                threads[i].state = TCB_RUNNABLE;
                rq_kick_idle_home(i);
            }
            /* Running on a remote CPU: IPI so stop/handler/kill is prompt. */
            if (sig != 0 && threads[i].on_cpu != SCHED_CPU_NONE
                && threads[i].on_cpu != sched_cpu_index()) {
                u32 apic = smp_cpu_apic_id(threads[i].on_cpu);
                if (apic != 0xFFFFFFFFu) {
                    /* Fixed delivery, vector SCHED_IPI_VECTOR. */
                    lapic_send_ipi(apic, (u32)SCHED_IPI_VECTOR);
                    sched_ipi_resched = sched_ipi_resched + 1;
                }
            } else if (sig == SIGCONT_N && threads[i].state == TCB_RUNNABLE) {
                rq_kick_idle_home(i);
            }
        }
    }
    return found;
}

/* Stop the sched_cur() process and switch away until SIGCONT. */
void sched_stop_self(u32 sig) {
    u32 ppid = threads[sched_cur()].ppid;
    threads[sched_cur()].state = TCB_STOPPED;
    threads[sched_cur()].wait_chan = 0;
    threads[sched_cur()].stop_sig = sig;
    threads[sched_cur()].stop_notify = 1;
    threads[sched_cur()].pending_sigs &= ~((1u << SIGSTOP_N) | (1u << SIGTSTP_N)
                                       | (1u << SIGTTIN_N) | (1u << SIGTTOU_N));
    if (ppid != 0) {
        sched_wakeup(sched_wait4_chan(ppid));
    }
    sched_switch_to(pick_next(sched_cur()));
}

/* Consume one stopped child notification for wait4(WUNTRACED). */
u32 sched_take_stopped(u32 ppid, u32 want_pid, u32 *out_pid, u32 *out_sig) {
    u32 i;
    for (i = 1; i < MAX_THREADS; i++) {
        if (threads[i].state != TCB_STOPPED) continue;
        if (threads[i].ppid != ppid) continue;
        if (want_pid != (u32)(-1) && threads[i].pid != want_pid) continue;
        if (!threads[i].stop_notify) continue;
        threads[i].stop_notify = 0;
        if (out_pid) *out_pid = threads[i].pid;
        if (out_sig) *out_sig = threads[i].stop_sig;
        return 1;
    }
    return 0;
}

u32 sched_stopped_exists(u32 ppid, u32 want_pid) {
    u32 i;
    for (i = 1; i < MAX_THREADS; i++) {
        if (threads[i].state != TCB_STOPPED) continue;
        if (threads[i].ppid != ppid) continue;
        if (want_pid != (u32)(-1) && threads[i].pid != want_pid) continue;
        if (threads[i].stop_notify) return 1;
    }
    return 0;
}

u32 sched_pid_exists(u32 pid) {
    return sched_signal_send(pid, 0);
}

u32 sched_current_pgid(void) {
    return threads[sched_cur()].pgid;
}

u32 sched_current_sid(void) {
    return threads[sched_cur()].sid;
}

u32 sched_get_pgid(u32 pid) {
    u32 i;
    u32 want = pid;
    if (want == 0) {
        want = threads[sched_cur()].pid;
    }
    for (i = 0; i < MAX_THREADS; i++) {
        if (threads[i].pid == want && threads[i].state != TCB_FREE
            && threads[i].state != TCB_DEAD) {
            return threads[i].pgid;
        }
    }
    return 0;
}

u32 sched_get_sid(u32 pid) {
    u32 i;
    u32 want = pid;
    if (want == 0) {
        want = threads[sched_cur()].pid;
    }
    for (i = 0; i < MAX_THREADS; i++) {
        if (threads[i].pid == want && threads[i].state != TCB_FREE
            && threads[i].state != TCB_DEAD) {
            return threads[i].sid;
        }
    }
    return 0;
}

/* Broadcast a signal to every live member of process group pgid.
 * Returns the number of matching processes (0 => no such pgrp). */
u32 sched_signal_pgrp(u32 pgid, u32 sig) {
    u32 i;
    u32 n = 0;
    if (pgid == 0 || sig > 31) {
        return 0;
    }
    for (i = 0; i < MAX_THREADS; i++) {
        if (threads[i].pgid == pgid && threads[i].pid != 0
            && threads[i].state != TCB_FREE && threads[i].state != TCB_DEAD) {
            if (sched_signal_send(threads[i].pid, sig)) {
                n++;
            }
        }
    }
    return n;
}

/* setpgid(pid, pgid): pid/pgid 0 mean "sched_cur()".
 * Caller may only move itself or a direct child in the same session.
 * New pgid must be caller's pid (create group) or an existing group in the
 * same session. Returns 0 on success, 1 on error. */
u32 sched_setpgid(u32 pid, u32 pgid) {
    u32 i;
    u32 target;
    u32 new_pg;
    u32 caller = threads[sched_cur()].pid;
    u32 caller_sid = threads[sched_cur()].sid;
    u32 slot = (u32)-1;
    u32 found_group = 0;

    if (caller == 0) {
        return 1;
    }
    target = (pid == 0) ? caller : pid;
    new_pg = (pgid == 0) ? target : pgid;

    for (i = 0; i < MAX_THREADS; i++) {
        if (threads[i].pid == target && threads[i].state != TCB_FREE
            && threads[i].state != TCB_DEAD) {
            slot = i;
            break;
        }
    }
    if (slot == (u32)-1) {
        return 1;
    }
    /* Only self or direct child. */
    if (threads[slot].pid != caller && threads[slot].ppid != caller) {
        return 1;
    }
    /* Session leaders cannot leave their pgrp (POSIX). */
    if (threads[slot].sid == threads[slot].pid && new_pg != threads[slot].pid) {
        return 1;
    }
    if (threads[slot].sid != caller_sid) {
        return 1;
    }
    if (new_pg != threads[slot].pid) {
        for (i = 0; i < MAX_THREADS; i++) {
            if (threads[i].pgid == new_pg && threads[i].sid == caller_sid
                && threads[i].state != TCB_FREE && threads[i].state != TCB_DEAD) {
                found_group = 1;
                break;
            }
        }
        if (!found_group) {
            return 1;
        }
    }
    threads[slot].pgid = new_pg;
    return 0;
}

/* setsid(): become session + process-group leader if not already a leader. */
u32 sched_setsid(void) {
    u32 pid = threads[sched_cur()].pid;
    if (pid == 0) {
        return 0;
    }
    /* POSIX: fail if already a process-group leader. */
    if (threads[sched_cur()].pgid == pid) {
        return 0;
    }
    threads[sched_cur()].sid = pid;
    threads[sched_cur()].pgid = pid;
    return pid;
}

/* Consume one continued-child notification for wait4(WCONTINUED). */
u32 sched_take_continued(u32 ppid, u32 want_pid, u32 *out_pid) {
    u32 i;
    for (i = 1; i < MAX_THREADS; i++) {
        if (!threads[i].cont_notify) continue;
        if (threads[i].ppid != ppid) continue;
        if (want_pid != (u32)(-1) && threads[i].pid != want_pid) continue;
        if (threads[i].state == TCB_FREE || threads[i].state == TCB_DEAD) continue;
        threads[i].cont_notify = 0;
        if (out_pid) *out_pid = threads[i].pid;
        return 1;
    }
    return 0;
}

u32 sched_continued_exists(u32 ppid, u32 want_pid) {
    u32 i;
    for (i = 1; i < MAX_THREADS; i++) {
        if (!threads[i].cont_notify) continue;
        if (threads[i].ppid != ppid) continue;
        if (want_pid != (u32)(-1) && threads[i].pid != want_pid) continue;
        if (threads[i].state == TCB_FREE || threads[i].state == TCB_DEAD) continue;
        return 1;
    }
    return 0;
}

/* Minimal user signal frame pushed onto the interrupted stack:
 *   [0] restorer RIP (handler returns here)
 *   [1] saved user RIP
 *   [2] saved RFLAGS
 *   [3] saved user RSP (pre-frame)
 *   [4] saved RDI
 *   [5] signo
 *   [6] saved RAX (syscall retval or interrupted user RAX)
 * Handler is entered with RDI=signo; returning through restorer runs
 * rt_sigreturn which restores RIP/RFLAGS/RSP/RDI/RAX. */
enum { SIGFRAME_QWORDS = 7 };

static u32 sig_deliver_frame(u32 sig, u64 *frame_rip, u64 *frame_rflags,
                             u64 *frame_rsp, u64 *frame_rdi, u64 *frame_rax) {
    u64 handler = threads[sched_cur()].sig_handler[sig];
    u64 restorer = threads[sched_cur()].sig_restorer[sig];
    u64 as = threads[sched_cur()].as_h;
    u64 usp;
    u64 *sf;
    if (handler <= 1 || restorer == 0 || as == 0) {
        return 0;
    }
    if (frame_rip == 0 || frame_rflags == 0 || frame_rsp == 0 || frame_rdi == 0) {
        return 0;
    }
    usp = *frame_rsp;
    /* Align so handler entry has RSP % 16 == 8 (SysV "after call"). */
    usp &= ~0xFULL;
    usp -= 8;
    usp -= SIGFRAME_QWORDS * 8;
    if (!mem.as_user_range_ok(as, usp, SIGFRAME_QWORDS * 8 + 8)) {
        return 0;
    }
    sf = (u64 *)usp;
    sf[0] = restorer;
    sf[1] = *frame_rip;
    sf[2] = *frame_rflags;
    sf[3] = *frame_rsp;
    sf[4] = *frame_rdi;
    sf[5] = (u64)sig;
    sf[6] = frame_rax ? *frame_rax : 0;
    *frame_rip = handler;
    *frame_rdi = (u64)sig;
    *frame_rsp = usp;
    threads[sched_cur()].pending_sigs &= ~(1u << sig);
    return 1;
}

/* Deliver pending signals. When `f_rip` is non-NULL we may redirect the
 * interrupted user/syscall frame to a handler; otherwise default actions. */
void sched_check_signals_frame(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi,
                               u64 *f_rax) {
    u32 pend;
    u32 ign;
    u32 sig;
    if (!threads[sched_cur()].is_user || threads[sched_cur()].pid == 0) {
        return;
    }
    pend = threads[sched_cur()].pending_sigs;
    ign = threads[sched_cur()].sig_ignore;
    if (pend & (1u << SIGKILL_N)) {
        threads[sched_cur()].pending_sigs = 0;
        proc_terminate(128 + SIGKILL_N);
    }
    threads[sched_cur()].pending_sigs &= ~ign;
    pend = threads[sched_cur()].pending_sigs;

    /* Prefer custom handlers; default INT/TERM terminate; STOP/TSTP stop. */
    for (sig = 1; sig <= 31; sig++) {
        u64 h;
        if ((pend & (1u << sig)) == 0) continue;
        if (sig == SIGKILL_N) continue;
        h = threads[sched_cur()].sig_handler[sig];

        /* SIGSTOP cannot be caught or ignored. */
        if (sig == SIGSTOP_N) {
            threads[sched_cur()].pending_sigs &= ~(1u << sig);
            sched_stop_self(SIGSTOP_N);
            pend = threads[sched_cur()].pending_sigs;
            continue;
        }
        /* SIGTSTP/TTIN/TTOU: custom handler if framed; else default stop. */
        if (sig == SIGTSTP_N || sig == SIGTTIN_N || sig == SIGTTOU_N) {
            if (h > 1 && f_rip != 0) {
                if (sig_deliver_frame(sig, f_rip, f_rflags, f_rsp, f_rdi, f_rax)) {
                    return;
                }
            }
            if (h == 1) {
                threads[sched_cur()].pending_sigs &= ~(1u << sig);
                pend = threads[sched_cur()].pending_sigs;
                continue;
            }
            threads[sched_cur()].pending_sigs &= ~(1u << sig);
            sched_stop_self(sig);
            pend = threads[sched_cur()].pending_sigs;
            continue;
        }
        /* SIGCONT: resume already done in signal_send; optional handler. */
        if (sig == SIGCONT_N) {
            if (h > 1 && f_rip != 0) {
                if (sig_deliver_frame(sig, f_rip, f_rflags, f_rsp, f_rdi, f_rax)) {
                    return;
                }
            }
            threads[sched_cur()].pending_sigs &= ~(1u << sig);
            pend = threads[sched_cur()].pending_sigs;
            continue;
        }

        if (h > 1 && f_rip != 0) {
            if (sig_deliver_frame(sig, f_rip, f_rflags, f_rsp, f_rdi, f_rax)) {
                return; /* one at a time */
            }
        }
        if (sig == SIGINT_N || sig == SIGTERM_N) {
            threads[sched_cur()].pending_sigs = 0;
            proc_terminate(128 + sig);
        }
        /* Drop unhandled / non-fatal bits. */
        threads[sched_cur()].pending_sigs &= ~(1u << sig);
        pend = threads[sched_cur()].pending_sigs;
    }
}

void sched_check_signals(void) {
    sched_check_signals_frame(0, 0, 0, 0, 0);
}

/* Restore user context from the sigframe at sched_cur() user RSP (rt_sigreturn).
 * On success writes restored RAX through f_rax (syscall visible return). */
u32 sched_sigreturn(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi,
                    u64 *f_rax) {
    u64 as = threads[sched_cur()].as_h;
    u64 usp;
    u64 *sf;
    if (as == 0 || f_rsp == 0) return 0;
    usp = *f_rsp;
    /* On entry to rt_sigreturn, RSP still points at the frame (ret popped restorer).
     * After `ret` from handler, RSP = frame+8 (past restorer). We accept either. */
    if (!mem.as_user_range_ok(as, usp, 8)) return 0;
    /* Prefer frame at RSP-8 if restorer was popped. */
    {
        u64 cand = usp - 8;
        if (mem.as_user_range_ok(as, cand, SIGFRAME_QWORDS * 8)) {
            sf = (u64 *)cand;
            /* Heuristic: signo at [5] in 1..31 */
            if (sf[5] >= 1 && sf[5] <= 31) {
                usp = cand;
            } else {
                sf = (u64 *)usp;
            }
        } else {
            sf = (u64 *)usp;
        }
    }
    if (!mem.as_user_range_ok(as, usp, SIGFRAME_QWORDS * 8)) return 0;
    sf = (u64 *)usp;
    *f_rip = sf[1];
    *f_rflags = sf[2];
    *f_rsp = sf[3];
    *f_rdi = sf[4];
    if (f_rax) {
        *f_rax = sf[6];
    }
    return 1;
}

u32 sched_has_pending_fatal(void) {
    u32 pend;
    u32 ign;
    u32 sig;
    if (!threads[sched_cur()].is_user) {
        return 0;
    }
    pend = threads[sched_cur()].pending_sigs;
    if (pend & (1u << SIGKILL_N)) {
        return 1;
    }
    ign = threads[sched_cur()].sig_ignore;
    pend &= ~ign;
    /* Custom handlers are not fatal — leave them for syscall-exit delivery. */
    for (sig = 1; sig <= 31; sig++) {
        if ((pend & (1u << sig)) == 0) continue;
        if (threads[sched_cur()].sig_handler[sig] > 1) {
            pend &= ~(1u << sig);
        }
    }
    if (pend & ((1u << SIGINT_N) | (1u << SIGTERM_N))) {
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * SMP: release parked APs into per-CPU idle + local APIC timer + steal.
 *
 * APs load GDT/IDT, LTR per-CPU TSS, enable LAPIC timer, install GS for the
 * SYSCALL conduit, then idle. Timer, post-halt pick_next, and vector-34
 * resched IPIs (from load-aware enqueue / wakeup) claim local RQ work or
 * steal from remote runqueues — kernel first, then user.
 * ------------------------------------------------------------------------- */

u32 sched_ap_waiting(void) {
    return ap_sched_go == 0 ? 1 : 0;
}

u64 sched_steal_total(void) {
    u32 i;
    u64 s = 0;
    for (i = 0; i < SCHED_MAX_CPUS; i++) {
        s = s + sched_steal_count[i];
    }
    return s;
}

u64 sched_steal_on_cpu(u32 cpu) {
    if (cpu >= SCHED_MAX_CPUS) {
        return 0;
    }
    return sched_steal_count[cpu];
}

u64 sched_steal_kernel_total(void) {
    return sched_migrate_kernel;
}

u64 sched_steal_user_total(void) {
    return sched_migrate_user;
}

u64 sched_balance_remote_total(void) {
    return sched_balance_remote;
}

u64 sched_ipi_resched_total(void) {
    return sched_ipi_resched;
}

/* Vector-34 reschedule: if this CPU is idle, pick work now (balance/steal).
 * Signal kicks to busy user threads skip this (caller checks CPL). */
void sched_resched_from_ipi(void) {
    if (!active) {
        return;
    }
    spin_lock(&sched_run_lock);
    if (idle_mark[sched_cur()] == 0) {
        spin_unlock(&sched_run_lock);
        return;
    }
    sched_switch_to(pick_next(sched_cur()));
}

void sched_ap_enter(u32 cpu) {
    u32 slot;
    u64 ticks0;
    if (cpu == sched_bsp_cpu || cpu >= SCHED_MAX_CPUS) {
        for (;;) {
            arch.cpu_cli();
            arch.cpu_halt();
        }
    }
    slot = idle_slot[cpu];
    if (slot >= MAX_THREADS || !idle_mark[slot]) {
        kprintf("[SMP] AP%u missing idle TCB\n", (u64)cpu);
        for (;;) {
            arch.cpu_cli();
            arch.cpu_halt();
        }
    }

    /* Leave the SIPI trampoline GDT (CS=0x18) for the kernel GDT (CS=0x08)
     * before any IDT gate (selector 0x08) can fire. LTR this CPU's TSS so
     * #DF IST1 / future user RSP0 are valid before the timer is unmasked. */
    gdt_flush();
    idt_load();
    gdt_load_cpu_tss(cpu);
    cpu_enable_sse();
    lapic_ap_init();
    lapic_ap_timer_enable();

    cpu_current[cpu] = slot;
    threads[slot].rq_cpu = cpu;
    threads[slot].on_cpu = cpu;
    threads[slot].state = TCB_RUNNABLE;
    /* SYSCALL conduit + TSS RSP0 before any stolen user thread can trap. */
    {
        u64 top = threads[slot].kstack_top;
        if (top == 0) {
            top = arch.gdt_get_rsp0();
        }
        arch.gdt_set_rsp0(top);
        sched_set_kstack_cpu(cpu, top);
    }
    ap_in_sched[cpu] = 1;
    ticks0 = lapic_get_cpu_ticks(cpu);
    kprintf("[SMP] AP%u entered scheduler (idle slot %u, TSS+timer+steal)
",
            (u64)cpu, (u64)slot);

    /* Idle with IRQs on: LAPIC timer runs sched_tick (may steal); after halt
     * also try pick_next so work enqueued mid-tick is claimed promptly. */
    for (;;) {
        cpu_sti();
        cpu_halt();
        cpu_cli();
        ap_idle_wakeups[cpu] = ap_idle_wakeups[cpu] + 1;
        spin_lock(&sched_run_lock);
        sched_switch_to(pick_next(sched_cur()));
        (void)ticks0;
    }
}

/* Prepare per-AP idle TCBs (reuse SIPI stacks) and release parked APs. */
u32 sched_smp_start(void) {
    u32 n = smp_cpu_count();
    u32 cpu;
    u32 started = 0;
    u32 wait;

    if (n <= 1) {
        kprintf("[SMP] sched: single-CPU, no AP attach\n");
        return 0;
    }

    for (cpu = 0; cpu < n && cpu < SCHED_MAX_CPUS; cpu++) {
        u32 slot;
        u64 top;
        if (cpu == sched_bsp_cpu) {
            continue;
        }
        if (!smp_cpu_online(cpu)) {
            continue;
        }
        slot = alloc_slot();
        if (slot == 0xFFFFFFFF) {
            kprintf("[SMP] sched: no TCB for AP%u\n", (u64)cpu);
            continue;
        }
        top = smp_cpu_stack_top(cpu);
        threads[slot].rsp = 0;
        threads[slot].stack_phys = 0;   /* AP static stack; do not free */
        threads[slot].kstack_top = top;
        threads[slot].entry = 0;
        threads[slot].arg = 0;
        threads[slot].as_h = 0;
        threads[slot].wait_chan = 0;
        threads[slot].fs_base = 0;
        threads[slot].pid = 0;
        threads[slot].tid = 0;
        threads[slot].ppid = 0;
        threads[slot].is_user = 0;
        threads[slot].pending_sigs = 0;
        threads[slot].sig_ignore = 0;
        fd_table_clear(&threads[slot]);
        cwd_set_root(&threads[slot]);
        cfs_set_nice_fields(&threads[slot], 0);
        threads[slot].vruntime = 0;
        threads[slot].rq_cpu = cpu;
        threads[slot].on_cpu = SCHED_CPU_NONE;
        threads[slot].state = TCB_RUNNABLE;
        idle_mark[slot] = 1;
        idle_slot[cpu] = slot;
        cpu_current[cpu] = slot;
        started++;
        kprintf("[SMP] sched: AP%u idle TCB slot %u stack %p\n",
                (u64)cpu, (u64)slot, top);
    }

    ap_sched_go = 1;
    for (wait = 0; wait < 500; wait++) {
        u32 ready = 1;
        for (cpu = 0; cpu < n && cpu < SCHED_MAX_CPUS; cpu++) {
            if (cpu == sched_bsp_cpu) {
                continue;
            }
            if (smp_cpu_online(cpu) && idle_slot[cpu] < MAX_THREADS
                && ap_in_sched[cpu] == 0) {
                ready = 0;
                break;
            }
        }
        if (ready) {
            break;
        }
        arch.pit_busy_wait_ms(1);
    }

    {
        u32 attached = 0;
        for (cpu = 0; cpu < n && cpu < SCHED_MAX_CPUS; cpu++) {
            if (cpu != sched_bsp_cpu && ap_in_sched[cpu]) {
                attached++;
            }
        }
        kprintf("[SMP] sched: %u AP(s) in idle\n",
                (u64)attached);

        /* Give AP timers ~150 ms at 100 Hz, then require real per-CPU ticks.
         * CLI on BSP so a tick cannot preempt kmain out of this probe. */
        if (attached > 0) {
            u32 ok = 1;
            kprintf("[SMP] sched: probing AP timers...\n");
            arch.cpu_cli();
            arch.pit_busy_wait_ms(150);
            for (cpu = 0; cpu < n && cpu < SCHED_MAX_CPUS; cpu++) {
                u64 ct;
                u64 wu;
                if (cpu == sched_bsp_cpu || !ap_in_sched[cpu]) {
                    continue;
                }
                ct = lapic_get_cpu_ticks(cpu);
                wu = ap_idle_wakeups[cpu];
                kprintf("[SMP] AP%u timer ticks=%u wakeups=%u\n",
                        (u64)cpu, ct, wu);
                if (ct < 5) {
                    ok = 0;
                }
            }
            if (ok) {
                kprintf("[PASS] AP LAPIC timer + per-CPU TSS/IST\n");
            } else {
                kprintf("[FAIL] AP LAPIC timer + per-CPU TSS/IST\n");
            }
            arch.cpu_sti();
        }
        return attached;
    }
}
