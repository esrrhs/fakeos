package kernel;
import arch;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t s32;
typedef types.int64_t s64;

/* pipe.c / net.c / epoll.c (same package) */
extern u32 pipe_fh_is(u32 fh);
extern void pipe_retain(u32 fh);
extern u32 pipe_close(u32 fh);
extern u32 sock_fh_is(u32 fh);
extern void sock_retain(u32 fh);
extern u32 sock_close(u32 fh);
extern u32 eventfd_fh_is(u32 fh);
extern u32 epoll_fh_is(u32 fh);
extern u32 eventfd_retain(u32 fhzing);
extern u32 epoll_retain(u32 fh);
extern u32 eventfd_close(u32 fh);
extern u32 epoll_close(u32 fh);

/* fs / Ext2 (externs avoid import fs re-parsing large Ext2 into sched). */
extern u32 fs_console_fh(void);
extern u32 fs_file_retain(u32 fh);
extern u32 fs_close(u32 fh);
extern u32 ext2_fh_is(u32 fh);
extern void ext2_file_retain(u32 fh);
extern u32 ext2_close(u32 fh);

extern u64 pmm_alloc_pages(u32 order);
extern void pmm_free_pages(u64 paddr, u32 order);
extern void switch_context(u64 *prev_rsp, u64 next_rsp, u64 prev_id);
extern void msr_write(u32 msr, u64 value);
extern void syscall_init(void);

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
    TCB_RETIRED  = 5, /* non-waitable exit, freed after stack handoff */
    TCB_RESERVED = 6, /* creator owns the slot, not runnable yet */

    MAX_THREADS  = 64,
    STACK_ORDER  = 2,                 /* 4 frames = 16 KiB per kernel stack  */
    STACK_BYTES  = 16384,
    SCHED_CPU_NONE = 0xFFFFFFFF,

    /* Interruptible park-admission registration (Go gopark/sema discipline).
     * A user task registers its park intent while still on-CPU; producers
     * latch a notification instead of losing the wakeup. Commit results use
     * the same values across compilation units (see callers of
     * sched_park_commit / sched_sleep_unlock_spin_intr). */
    PARK_NONE        = 0, /* no registration (default)                       */
    PARK_PREPARING   = 1, /* intent registered; producers may latch          */
    PARK_PARKED      = 0, /* commit: slept and was woken                     */
    PARK_WOKEN       = 1, /* commit: notification latched, never slept       */
    PARK_INTERRUPTED = 2, /* commit: refused, pending cancellation           */
    PARK_TIMED_OUT   = 3, /* commit: deadline expired before wakeup          */

    FD_MAX       = 16,
    FD_RESERVED  = 0xFFFFFFFEu,
    FD_EMPTY     = 0xFFFFFFFFu,
    CWD_LEN      = 512
};

struct tcb {
    u64 rsp;            /* Saved kernel stack pointer (0 = never started) */
    u64 stack_phys;    /* Buddy block backing the kernel stack            */
    u64 kstack_top;    /* Kernel stack top (HHDM address, 16-aligned)     */
    u64 entry;         /* Thread entry function                           */
    u64 arg;           /* Single u64 argument                             */
    u64 as_h;          /* Address space handle (0 = kernel space)         */
    u64 wait_chan;     /* Sleep channel (0 = not waiting on a waitqueue)  */
    u64 timer_chan;    /* Per-task timer channel (no shared wake floods)  */
    u64 deadline_ns;   /* Monotonic ns deadline (0 = none)                */
    u64 deadline_ticks;/* Tick deadline (0 = none)                        */
    u32 timed_out;     /* 1 => woken by deadline expiration               */
    u32 park_state;    /* PARK_NONE / PARK_PREPARING (pre-commit intent)  */
    u32 park_notify;   /* Producer latched a wake while PARK_PREPARING    */
    u64 fs_base;       /* MSR_FS_BASE (arch_prctl TLS); restored on switch */
    u64 clear_child_tid; /* set_tid_address / CLEARTID: kernel stores 0 + futex wake on exit */
    u32 pinned_fh;     /* In-flight syscall fd pin (prevents close UAF)   */
    u32 task_cancelled;/* 1 => task-scoped cancellation requested         */
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
    u32 cfs_left;      /* Intrusive CFS rbtree (vruntime order)           */
    u32 cfs_right;
    u32 cfs_parent;
    u32 cfs_color;     /* 0=red, 1=black                                   */
    u32 cfs_on_rq;     /* 1 => linked in cfs_root[rq_cpu]                 */
    u32 tg;            /* task_group id (0 = root)                        */
};

/* Pointer-based operations are deliberate: current fakecc miscompiles
 * indexed-struct-member compound updates. Do not replace these helpers with
 * threads[index].field |= / &=; see docs/evidence/native-process-foundation-20261117. */
static void sched_bitset_or(u32 *word, u32 mask) { *word = *word | mask; }
static void sched_bitset_and(u32 *word, u32 mask) { *word = *word & mask; }

/* Sleeping locks: wait channel is the object's own address.
 * kmutex  — binary sleep-mutex (owner tracked; unlock wakes one)
 * ksem    — counting semaphore (up wakes one)
 * wait_queue_head — typed Mesa waitqueue over sched_sleep/wakeup */
struct kmutex {
    u32 locked;
    u32 owner;          /* TCB id while held; 0xFFFFFFFF if free */
};

struct ksem {
    s32 count;
};

struct wait_queue_head {
    u32 pad;            /* address is the channel; pad avoids empty struct */
};

static struct tcb threads[MAX_THREADS];
enum {
    SCHED_MAX_CPUS = 8,
    SCHED_IPI_VECTOR = 34,  /* fixed IPI: reschedule / signal kick */
    TLB_IPI_VECTOR   = 35   /* fixed IPI: TLB shootdown ack               */
};
static u32 cpu_current[SCHED_MAX_CPUS];
static volatile u32 idle_slot[SCHED_MAX_CPUS];
static u32 idle_mark[MAX_THREADS];   /* 1 => per-CPU idle TCB (not CFS-picked) */
static volatile u32 sched_run_lock;

/* ---------------------------------------------------------------------------
 * run_lock instrumentation (architecture-doc step 1 for the sharding
 * decision): per-CPU acquisitions, contended acquisitions (the
 * test-and-set failed before the spin), and hold time measured with the
 * PIT-calibrated TSC. The wrapper pair replaces every direct
 * spin_lock/spin_unlock of sched_run_lock in this file — the lock is
 * never held recursively and never across sleeps, so per-CPU accounting
 * is exact. Exposed read-only for the vmcc stress and future sharding
 * work; the wrapper adds two TSC reads per acquisition (bounded slice
 * overhead, removed when the lock is actually split).
 * ------------------------------------------------------------------------- */
u64 nanosleep_chan = 0x4E414E4F534C4550ULL;   /* "NANOSLEP": nonzero chan */
static u32 rl_site[8];
extern u64 tsc_read(void);
extern u64 tsc_hz(void);
extern u64 tsc_ns(void);
extern u32 spin_test_and_set(volatile u32 *lock);

/* Long-hold attribution (P0-3 step 2): the top lock-holding sites pass an
 * explicit site id; any hold longer than 1 ms is counted PER SITE, so the
 * sharding work targets the measured offender instead of a guess. Site ids
 * are LITERALS (fakecc enum values proved unreliable in this context):
 * 1 tick, 2 yield, 3 wakeup, 4 wakeup_n, 10 alloc_slot, 12 process_attach,
 * 13 nat_acquire; 0 = unattributed. Bound: 32 sites. */
u64 rl_long_sites[32];

/* Per-CPU stats: pointer-member updates ONLY — fakecc's indexed-array
 * self-referencing updates proved unreliable (holds stayed 0 while hold_ns
 * accumulated — see docs/evidence/rl-instrument-20261121/). */
struct rl_stats {
    u64 holds;
    u64 hold_ns;
    u64 hold_max;
    u64 contends;
    u64 start;
    u32 site;
};
static struct rl_stats rl_s[8];

static void runlock_lock_at(u32 site) {
    u32 cpu = sched_cpu_index();
    struct rl_stats *s = &rl_s[cpu];
    if (spin_test_and_set(&sched_run_lock)) {
        s->contends = s->contends + 1;
        spin_lock(&sched_run_lock);        /* the wrapper's own primitive */
    }
    s->holds = s->holds + 1;
    s->site = site;
    s->start = tsc_read();
}

static void runlock_lock(void) {
    runlock_lock_at(0);
}

static void runlock_unlock(void) {
    u32 cpu = sched_cpu_index();
    struct rl_stats *s = &rl_s[cpu];
    u64 d = tsc_read() - s->start;
    s->hold_ns = s->hold_ns + d;
    if (d > s->hold_max) {
        s->hold_max = d;
    }
    if (d > 1000000u) {
        u32 si = s->site;
        if (si < 32) {
            rl_long_sites[si] = rl_long_sites[si] + 1;
        }
    }
    spin_unlock(&sched_run_lock);          /* the wrapper's own primitive */
}
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
/* Aggregate preemption events retain the scheduler/selftest contract. They
 * are NOT elapsed time: every CPU contributes one event per local IRQ. */
static volatile u64 sched_ticks;
/* One calibrated 100Hz source (the boot CPU), independent of AP count. */
static volatile u64 clock_ticks;
/* Process resources are NOT owned by a privileged leader TCB. run_lock
 * protects admission, live borrowers, resident task refs and image ownership.
 * pid!=0 reserves an object through zombie/reap; PIDs never wrap/recycle. */
/* Native capability table entry (MUST match kernel/natcap.c exactly). The
 * table lives in the process owner record — arch section 3: process
 * { native_cap_table } — shared by all tasks of the process by identity. */
struct nat_cap {
    u32 gen;                                /* 0 = virgin; NAT_GEN_MAX = retired */
    u32 rights;
    u64 handle;
};
enum { NAT_CAPS = 32, NAT_GEN_MAX = 0xFFFFu };

struct process_owner {
    u32 pid, ppid, live, resident;
    u32 exiting, code, image_busy, drained, native_spawned;
    u32 cpu_mask;
    u64 as_h;
    struct nat_cap nat_caps[NAT_CAPS];
    u32 nat_used;
    /* Process-shared descriptor, filesystem, environment & credential state */
    u32 fd[FD_MAX];
    u32 deferred_close[FD_MAX];
    u32 ndeferred;
    char cwd[CWD_LEN];
    char cmdline[128];
    u32 cmdline_len;
    char environ[256];
    u32 environ_len;
    u32 umask;
    u32 uid;
    u32 euid;
    u32 gid;
    u32 egid;
    u32 ngroups;
    u32 groups[8];
};
static struct process_owner processes[MAX_THREADS];
static struct process_owner kernel_proc_owner;
static u32 kernel_proc_inited;
void sched_process_close_all_fds(u32 pid);
static u32 next_pid = 1;             /* User process id allocator          */
static volatile u64 user_mode_ticks; /* APIC ticks taken while in ring 3   */
static u64 cfs_min_vruntime;         /* bootstrap fallback                 */
static u64 cfs_min_vr_cpu[SCHED_MAX_CPUS];
static volatile u32 rq_locks[SCHED_MAX_CPUS];
static u64 rq_holds[SCHED_MAX_CPUS];
static u64 rq_contends[SCHED_MAX_CPUS];

static void rq_lock_acquire(u32 cpu) {
    if (cpu >= SCHED_MAX_CPUS) cpu = 0;
    if (spin_test_and_set(&rq_locks[cpu])) {
        rq_contends[cpu] = rq_contends[cpu] + 1;
        spin_lock(&rq_locks[cpu]);
    }
    rq_holds[cpu] = rq_holds[cpu] + 1;
}

static void rq_lock_release(u32 cpu) {
    if (cpu >= SCHED_MAX_CPUS) cpu = 0;
    spin_unlock(&rq_locks[cpu]);
}

void sched_rq_stats(u32 cpu, u64 *holds, u64 *contends) {
    if (cpu >= SCHED_MAX_CPUS) {
        *holds = 0;
        *contends = 0;
        return;
    }
    *holds = rq_holds[cpu];
    *contends = rq_contends[cpu];
}
/* Roots must be NONE (not BSS 0): thread 0 is a valid TCB id, and virtio RX
 * may sched_wakeup -> cfs_update_min before sched_init clears the trees. */
static u32 cfs_root[SCHED_MAX_CPUS] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
};
static u32 cfs_nr[SCHED_MAX_CPUS];   /* runnable non-idle entities on RQ    */

/* CFS task_group (hierarchical fair share + nested group SE rbtree):
 * Each thread belongs to a group. Groups have a parent (TG_ROOT's parent is
 * TG_NONE). Sibling groups compete by shares on their parent's per-CPU child
 * SE red-black tree (keyed by group vruntime, then tg id). Top-level siblings
 * (parent == TG_ROOT) plus TG_ROOT itself (when it has direct tasks) live in
 * tg_se_root[]. Nested children live in parent->child_se_root[]. pick_next
 * descends leftmost nested SEs to a leaf, then picks the min-vruntime member.
 * Charge walks the parent chain. Not a full Linux cgroup yet. */
enum {
    MAX_TASK_GROUPS = 16,
    TG_ROOT         = 0,
    TG_NONE         = 0xFFFFFFFF
};

struct task_group {
    u32 used;                          /* 1 => slot allocated              */
    u32 parent;                        /* TG_NONE for root; else parent tg */
    u32 shares;                        /* cpu.shares-style weight          */
    u64 vruntime[SCHED_MAX_CPUS];      /* group entity vruntime per CPU    */
    u32 nr[SCHED_MAX_CPUS];            /* runnable direct tasks on CPU RQ  */
    u32 se_left[SCHED_MAX_CPUS];       /* intrusive group SE in parent tree*/
    u32 se_right[SCHED_MAX_CPUS];
    u32 se_parent[SCHED_MAX_CPUS];
    u32 se_color[SCHED_MAX_CPUS];       /* 0=red, 1=black                   */
    u32 se_on_rq[SCHED_MAX_CPUS];      /* 1 => linked in parent SE tree    */
    /* Nested children SE tree root; must be NONE (not BSS 0). */
    u32 child_se_root[SCHED_MAX_CPUS];
};

static struct task_group task_groups[MAX_TASK_GROUPS];
/* Roots must be NONE (not BSS 0): tg 0 is the valid root group. */
static u32 tg_se_root[SCHED_MAX_CPUS] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
};

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
extern void io_wait(void);
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
    if (cpu >= SCHED_MAX_CPUS) {
        return 0;
    }
    return cfs_nr[cpu];
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

enum {
    NICE_WIDTH     = 40,             /* nice -20..19                      */
    NICE0_WEIGHT   = 1024,           /* CFS weight at nice 0              */
    VRUNTIME_SLICE = 1024,           /* charge per tick/yield at nice 0   */
    CFS_RB_NONE    = 0xFFFFFFFF,     /* null child/parent in index tree   */
    CFS_RB_RED     = 0,
    CFS_RB_BLACK   = 1
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

/* ---- CFS task_group helpers (hierarchical fair share + group SE rb) - */

static u32 tg_valid(u32 tg) {
    if (tg >= MAX_TASK_GROUPS) {
        return 0;
    }
    return task_groups[tg].used;
}

/* SE tree root pointer for the tree that holds `tg`'s entity. Top-level
 * (parent TG_NONE/TG_ROOT) uses tg_se_root; nested uses parent child_se_root. */
static u32 *tg_se_tree_rootp(u32 tg, u32 cpu) {
    u32 p;
    if (cpu >= SCHED_MAX_CPUS) {
        return &tg_se_root[0];
    }
    if (tg >= MAX_TASK_GROUPS) {
        return &tg_se_root[cpu];
    }
    p = task_groups[tg].parent;
    if (p == TG_NONE || p == TG_ROOT) {
        return &tg_se_root[cpu];
    }
    return &task_groups[p].child_se_root[cpu];
}

static u32 tg_has_nested_runnable(u32 tg, u32 cpu) {
    if (tg >= MAX_TASK_GROUPS || cpu >= SCHED_MAX_CPUS) {
        return 0;
    }
    return task_groups[tg].child_se_root[cpu] != TG_NONE;
}

/* Group should be linked in its parent SE tree. */
static u32 tg_should_link(u32 tg, u32 cpu) {
    if (!tg_valid(tg) || cpu >= SCHED_MAX_CPUS) {
        return 0;
    }
    if (task_groups[tg].nr[cpu] > 0) {
        return 1;
    }
    /* Nested children make a mid group runnable; ROOT stays off the top tree. */
    if (tg != TG_ROOT && tg_has_nested_runnable(tg, cpu)) {
        return 1;
    }
    return 0;
}

static void tg_se_clear_node(u32 tg, u32 cpu) {
    if (tg >= MAX_TASK_GROUPS || cpu >= SCHED_MAX_CPUS) {
        return;
    }
    task_groups[tg].se_left[cpu] = TG_NONE;
    task_groups[tg].se_right[cpu] = TG_NONE;
    task_groups[tg].se_parent[cpu] = TG_NONE;
    task_groups[tg].se_color[cpu] = CFS_RB_RED;
    task_groups[tg].se_on_rq[cpu] = 0;
}

static void tg_init_root(void) {
    u32 c;
    u32 g;
    for (g = 0; g < MAX_TASK_GROUPS; g++) {
        task_groups[g].used = 0;
        task_groups[g].parent = TG_NONE;
        task_groups[g].shares = NICE0_WEIGHT;
        for (c = 0; c < SCHED_MAX_CPUS; c++) {
            task_groups[g].vruntime[c] = 0;
            task_groups[g].nr[c] = 0;
            task_groups[g].child_se_root[c] = TG_NONE;
            tg_se_clear_node(g, c);
        }
    }
    for (c = 0; c < SCHED_MAX_CPUS; c++) {
        tg_se_root[c] = TG_NONE;
    }
    task_groups[TG_ROOT].used = 1;
    task_groups[TG_ROOT].parent = TG_NONE;
    task_groups[TG_ROOT].shares = NICE0_WEIGHT;
}

/* Strict weak order for group SEs on `cpu`: vruntime, then tg id. */
static u32 tg_se_before(u32 cpu, u32 a, u32 b) {
    if (task_groups[a].vruntime[cpu] < task_groups[b].vruntime[cpu]) {
        return 1;
    }
    if (task_groups[a].vruntime[cpu] > task_groups[b].vruntime[cpu]) {
        return 0;
    }
    return a < b;
}

static u32 tg_se_leftmost_from(u32 cpu, u32 tg) {
    u32 guard = 0;
    if (cpu >= SCHED_MAX_CPUS || tg >= MAX_TASK_GROUPS) {
        return TG_NONE;
    }
    while (task_groups[tg].se_left[cpu] < MAX_TASK_GROUPS) {
        tg = task_groups[tg].se_left[cpu];
        guard = guard + 1;
        if (guard > MAX_TASK_GROUPS) {
            return TG_NONE;
        }
    }
    return tg;
}

static u32 tg_se_rq_leftmost(u32 cpu) {
    u32 root;
    if (cpu >= SCHED_MAX_CPUS) {
        return TG_NONE;
    }
    root = tg_se_root[cpu];
    if (root >= MAX_TASK_GROUPS || !task_groups[root].se_on_rq[cpu]) {
        return TG_NONE;
    }
    return tg_se_leftmost_from(cpu, root);
}

static u32 tg_se_successor(u32 cpu, u32 tg) {
    u32 p;
    u32 guard = 0;
    if (cpu >= SCHED_MAX_CPUS || tg >= MAX_TASK_GROUPS) {
        return TG_NONE;
    }
    if (task_groups[tg].se_right[cpu] < MAX_TASK_GROUPS) {
        return tg_se_leftmost_from(cpu, task_groups[tg].se_right[cpu]);
    }
    p = task_groups[tg].se_parent[cpu];
    while (p < MAX_TASK_GROUPS && tg == task_groups[p].se_right[cpu]) {
        tg = p;
        p = task_groups[p].se_parent[cpu];
        guard = guard + 1;
        if (guard > MAX_TASK_GROUPS) {
            return TG_NONE;
        }
    }
    if (p >= MAX_TASK_GROUPS) {
        return TG_NONE;
    }
    return p;
}

static void tg_se_rotate_left(u32 cpu, u32 x) {
    u32 y;
    u32 yp;
    if (cpu >= SCHED_MAX_CPUS || x >= MAX_TASK_GROUPS) {
        return;
    }
    y = task_groups[x].se_right[cpu];
    if (y >= MAX_TASK_GROUPS) {
        return;
    }
    task_groups[x].se_right[cpu] = task_groups[y].se_left[cpu];
    if (task_groups[y].se_left[cpu] != TG_NONE) {
        task_groups[task_groups[y].se_left[cpu]].se_parent[cpu] = x;
    }
    yp = task_groups[x].se_parent[cpu];
    task_groups[y].se_parent[cpu] = yp;
    if (yp == TG_NONE) {
        *tg_se_tree_rootp(x, cpu) = y;
    } else if (x == task_groups[yp].se_left[cpu]) {
        task_groups[yp].se_left[cpu] = y;
    } else {
        task_groups[yp].se_right[cpu] = y;
    }
    task_groups[y].se_left[cpu] = x;
    task_groups[x].se_parent[cpu] = y;
}

static void tg_se_rotate_right(u32 cpu, u32 x) {
    u32 y;
    u32 yp;
    if (cpu >= SCHED_MAX_CPUS || x >= MAX_TASK_GROUPS) {
        return;
    }
    y = task_groups[x].se_left[cpu];
    if (y >= MAX_TASK_GROUPS) {
        return;
    }
    task_groups[x].se_left[cpu] = task_groups[y].se_right[cpu];
    if (task_groups[y].se_right[cpu] != TG_NONE) {
        task_groups[task_groups[y].se_right[cpu]].se_parent[cpu] = x;
    }
    yp = task_groups[x].se_parent[cpu];
    task_groups[y].se_parent[cpu] = yp;
    if (yp == TG_NONE) {
        *tg_se_tree_rootp(x, cpu) = y;
    } else if (x == task_groups[yp].se_right[cpu]) {
        task_groups[yp].se_right[cpu] = y;
    } else {
        task_groups[yp].se_left[cpu] = y;
    }
    task_groups[y].se_right[cpu] = x;
    task_groups[x].se_parent[cpu] = y;
}

static void tg_se_insert_fixup(u32 cpu, u32 z) {
    while (z != TG_NONE && task_groups[z].se_parent[cpu] != TG_NONE
           && task_groups[task_groups[z].se_parent[cpu]].se_color[cpu] == CFS_RB_RED) {
        u32 p = task_groups[z].se_parent[cpu];
        u32 g = task_groups[p].se_parent[cpu];
        u32 y;
        if (g == TG_NONE) {
            break;
        }
        if (p == task_groups[g].se_left[cpu]) {
            y = task_groups[g].se_right[cpu];
            if (y != TG_NONE && task_groups[y].se_color[cpu] == CFS_RB_RED) {
                task_groups[p].se_color[cpu] = CFS_RB_BLACK;
                task_groups[y].se_color[cpu] = CFS_RB_BLACK;
                task_groups[g].se_color[cpu] = CFS_RB_RED;
                z = g;
            } else {
                if (z == task_groups[p].se_right[cpu]) {
                    z = p;
                    tg_se_rotate_left(cpu, z);
                    p = task_groups[z].se_parent[cpu];
                    g = (p == TG_NONE) ? TG_NONE : task_groups[p].se_parent[cpu];
                }
                if (p != TG_NONE) {
                    task_groups[p].se_color[cpu] = CFS_RB_BLACK;
                }
                if (g != TG_NONE) {
                    task_groups[g].se_color[cpu] = CFS_RB_RED;
                    tg_se_rotate_right(cpu, g);
                }
            }
        } else {
            y = task_groups[g].se_left[cpu];
            if (y != TG_NONE && task_groups[y].se_color[cpu] == CFS_RB_RED) {
                task_groups[p].se_color[cpu] = CFS_RB_BLACK;
                task_groups[y].se_color[cpu] = CFS_RB_BLACK;
                task_groups[g].se_color[cpu] = CFS_RB_RED;
                z = g;
            } else {
                if (z == task_groups[p].se_left[cpu]) {
                    z = p;
                    tg_se_rotate_right(cpu, z);
                    p = task_groups[z].se_parent[cpu];
                    g = (p == TG_NONE) ? TG_NONE : task_groups[p].se_parent[cpu];
                }
                if (p != TG_NONE) {
                    task_groups[p].se_color[cpu] = CFS_RB_BLACK;
                }
                if (g != TG_NONE) {
                    task_groups[g].se_color[cpu] = CFS_RB_RED;
                    tg_se_rotate_left(cpu, g);
                }
            }
        }
    }
    {
        u32 *rp = tg_se_tree_rootp(z, cpu);
        if (*rp != TG_NONE) {
            task_groups[*rp].se_color[cpu] = CFS_RB_BLACK;
        }
    }
}

static void tg_se_insert(u32 cpu, u32 z) {
    u32 y;
    u32 x;
    u32 *rootp;
    if (cpu >= SCHED_MAX_CPUS || z >= MAX_TASK_GROUPS) {
        return;
    }
    if (task_groups[z].se_on_rq[cpu]) {
        return;
    }
    rootp = tg_se_tree_rootp(z, cpu);
    task_groups[z].se_left[cpu] = TG_NONE;
    task_groups[z].se_right[cpu] = TG_NONE;
    task_groups[z].se_parent[cpu] = TG_NONE;
    task_groups[z].se_color[cpu] = CFS_RB_RED;
    y = TG_NONE;
    x = *rootp;
    while (x != TG_NONE) {
        y = x;
        if (tg_se_before(cpu, z, x)) {
            x = task_groups[x].se_left[cpu];
        } else {
            x = task_groups[x].se_right[cpu];
        }
    }
    task_groups[z].se_parent[cpu] = y;
    if (y == TG_NONE) {
        *rootp = z;
    } else if (tg_se_before(cpu, z, y)) {
        task_groups[y].se_left[cpu] = z;
    } else {
        task_groups[y].se_right[cpu] = z;
    }
    task_groups[z].se_on_rq[cpu] = 1;
    tg_se_insert_fixup(cpu, z);
}

static void tg_se_transplant(u32 cpu, u32 u, u32 v) {
    u32 up;
    if (cpu >= SCHED_MAX_CPUS || u >= MAX_TASK_GROUPS) {
        return;
    }
    up = task_groups[u].se_parent[cpu];
    if (up == TG_NONE) {
        *tg_se_tree_rootp(u, cpu) = v;
    } else if (u == task_groups[up].se_left[cpu]) {
        task_groups[up].se_left[cpu] = v;
    } else {
        task_groups[up].se_right[cpu] = v;
    }
    if (v != TG_NONE) {
        task_groups[v].se_parent[cpu] = up;
    }
}

static void tg_se_erase_fixup(u32 cpu, u32 x, u32 xp, u32 *rootp) {
    while ((x == TG_NONE || task_groups[x].se_color[cpu] == CFS_RB_BLACK)
           && x != *rootp) {
        u32 w;
        if (xp == TG_NONE) {
            break;
        }
        if (x == task_groups[xp].se_left[cpu]) {
            w = task_groups[xp].se_right[cpu];
            if (w != TG_NONE && task_groups[w].se_color[cpu] == CFS_RB_RED) {
                task_groups[w].se_color[cpu] = CFS_RB_BLACK;
                task_groups[xp].se_color[cpu] = CFS_RB_RED;
                tg_se_rotate_left(cpu, xp);
                w = task_groups[xp].se_right[cpu];
            }
            if (w == TG_NONE) {
                x = xp;
                xp = task_groups[x].se_parent[cpu];
                continue;
            }
            if ((task_groups[w].se_left[cpu] == TG_NONE
                 || task_groups[task_groups[w].se_left[cpu]].se_color[cpu] == CFS_RB_BLACK)
                && (task_groups[w].se_right[cpu] == TG_NONE
                    || task_groups[task_groups[w].se_right[cpu]].se_color[cpu] == CFS_RB_BLACK)) {
                task_groups[w].se_color[cpu] = CFS_RB_RED;
                x = xp;
                xp = task_groups[x].se_parent[cpu];
            } else {
                if (task_groups[w].se_right[cpu] == TG_NONE
                    || task_groups[task_groups[w].se_right[cpu]].se_color[cpu] == CFS_RB_BLACK) {
                    if (task_groups[w].se_left[cpu] != TG_NONE) {
                        task_groups[task_groups[w].se_left[cpu]].se_color[cpu] = CFS_RB_BLACK;
                    }
                    task_groups[w].se_color[cpu] = CFS_RB_RED;
                    tg_se_rotate_right(cpu, w);
                    w = task_groups[xp].se_right[cpu];
                }
                if (w != TG_NONE) {
                    task_groups[w].se_color[cpu] = task_groups[xp].se_color[cpu];
                    if (task_groups[w].se_right[cpu] != TG_NONE) {
                        task_groups[task_groups[w].se_right[cpu]].se_color[cpu] = CFS_RB_BLACK;
                    }
                }
                task_groups[xp].se_color[cpu] = CFS_RB_BLACK;
                tg_se_rotate_left(cpu, xp);
                x = *rootp;
                xp = TG_NONE;
            }
        } else {
            w = task_groups[xp].se_left[cpu];
            if (w != TG_NONE && task_groups[w].se_color[cpu] == CFS_RB_RED) {
                task_groups[w].se_color[cpu] = CFS_RB_BLACK;
                task_groups[xp].se_color[cpu] = CFS_RB_RED;
                tg_se_rotate_right(cpu, xp);
                w = task_groups[xp].se_left[cpu];
            }
            if (w == TG_NONE) {
                x = xp;
                xp = task_groups[x].se_parent[cpu];
                continue;
            }
            if ((task_groups[w].se_right[cpu] == TG_NONE
                 || task_groups[task_groups[w].se_right[cpu]].se_color[cpu] == CFS_RB_BLACK)
                && (task_groups[w].se_left[cpu] == TG_NONE
                    || task_groups[task_groups[w].se_left[cpu]].se_color[cpu] == CFS_RB_BLACK)) {
                task_groups[w].se_color[cpu] = CFS_RB_RED;
                x = xp;
                xp = task_groups[x].se_parent[cpu];
            } else {
                if (task_groups[w].se_left[cpu] == TG_NONE
                    || task_groups[task_groups[w].se_left[cpu]].se_color[cpu] == CFS_RB_BLACK) {
                    if (task_groups[w].se_right[cpu] != TG_NONE) {
                        task_groups[task_groups[w].se_right[cpu]].se_color[cpu] = CFS_RB_BLACK;
                    }
                    task_groups[w].se_color[cpu] = CFS_RB_RED;
                    tg_se_rotate_left(cpu, w);
                    w = task_groups[xp].se_left[cpu];
                }
                if (w != TG_NONE) {
                    task_groups[w].se_color[cpu] = task_groups[xp].se_color[cpu];
                    if (task_groups[w].se_left[cpu] != TG_NONE) {
                        task_groups[task_groups[w].se_left[cpu]].se_color[cpu] = CFS_RB_BLACK;
                    }
                }
                task_groups[xp].se_color[cpu] = CFS_RB_BLACK;
                tg_se_rotate_right(cpu, xp);
                x = *rootp;
                xp = TG_NONE;
            }
        }
    }
    if (x != TG_NONE) {
        task_groups[x].se_color[cpu] = CFS_RB_BLACK;
    }
}

static void tg_se_erase(u32 cpu, u32 z) {
    u32 y;
    u32 x;
    u32 xp;
    u32 y_orig_color;
    u32 *rootp;
    if (cpu >= SCHED_MAX_CPUS || z >= MAX_TASK_GROUPS) {
        return;
    }
    if (!task_groups[z].se_on_rq[cpu]) {
        return;
    }
    rootp = tg_se_tree_rootp(z, cpu);
    y = z;
    y_orig_color = task_groups[y].se_color[cpu];
    if (task_groups[z].se_left[cpu] == TG_NONE) {
        x = task_groups[z].se_right[cpu];
        xp = task_groups[z].se_parent[cpu];
        tg_se_transplant(cpu, z, task_groups[z].se_right[cpu]);
    } else if (task_groups[z].se_right[cpu] == TG_NONE) {
        x = task_groups[z].se_left[cpu];
        xp = task_groups[z].se_parent[cpu];
        tg_se_transplant(cpu, z, task_groups[z].se_left[cpu]);
    } else {
        y = tg_se_leftmost_from(cpu, task_groups[z].se_right[cpu]);
        y_orig_color = task_groups[y].se_color[cpu];
        x = task_groups[y].se_right[cpu];
        if (task_groups[y].se_parent[cpu] == z) {
            xp = y;
            if (x != TG_NONE) {
                task_groups[x].se_parent[cpu] = y;
            }
        } else {
            xp = task_groups[y].se_parent[cpu];
            tg_se_transplant(cpu, y, task_groups[y].se_right[cpu]);
            task_groups[y].se_right[cpu] = task_groups[z].se_right[cpu];
            if (task_groups[y].se_right[cpu] != TG_NONE) {
                task_groups[task_groups[y].se_right[cpu]].se_parent[cpu] = y;
            }
        }
        tg_se_transplant(cpu, z, y);
        task_groups[y].se_left[cpu] = task_groups[z].se_left[cpu];
        if (task_groups[y].se_left[cpu] != TG_NONE) {
            task_groups[task_groups[y].se_left[cpu]].se_parent[cpu] = y;
        }
        task_groups[y].se_color[cpu] = task_groups[z].se_color[cpu];
    }
    tg_se_clear_node(z, cpu);
    if (y_orig_color == CFS_RB_BLACK) {
        tg_se_erase_fixup(cpu, x, xp, rootp);
    }
}

/* Clamp a newly-runnable group's entity against siblings in its parent tree. */
static void tg_place_entity(u32 tg, u32 cpu) {
    u32 left;
    u64 minv;
    u32 *rootp;
    u32 root;
    if (!tg_valid(tg) || cpu >= SCHED_MAX_CPUS) {
        return;
    }
    rootp = tg_se_tree_rootp(tg, cpu);
    root = *rootp;
    if (root >= MAX_TASK_GROUPS || !task_groups[root].se_on_rq[cpu]) {
        return;
    }
    left = tg_se_leftmost_from(cpu, root);
    if (left == TG_NONE) {
        return;
    }
    minv = task_groups[left].vruntime[cpu];
    if (task_groups[tg].vruntime[cpu] < minv) {
        task_groups[tg].vruntime[cpu] = minv;
    }
}

/* Link `tg` into its parent SE tree; bubble ancestors that become runnable. */
static void tg_link_runnable(u32 tg, u32 cpu) {
    u32 guard = 0;
    while (tg_valid(tg) && guard < MAX_TASK_GROUPS) {
        guard = guard + 1;
        if (!tg_should_link(tg, cpu)) {
            break;
        }
        if (!task_groups[tg].se_on_rq[cpu]) {
            tg_place_entity(tg, cpu);
            tg_se_insert(cpu, tg);
        }
        /* Top-level entities sit in tg_se_root; no further parent to link.
         * (TG_ROOT itself only links when it has direct tasks.) */
        if (tg == TG_ROOT) {
            break;
        }
        if (task_groups[tg].parent == TG_ROOT || task_groups[tg].parent == TG_NONE) {
            break;
        }
        tg = task_groups[tg].parent;
    }
}

/* Erase idle groups and bubble: mid groups drop when child tree empties. */
static void tg_unlink_idle(u32 tg, u32 cpu) {
    u32 guard = 0;
    while (tg_valid(tg) && guard < MAX_TASK_GROUPS) {
        guard = guard + 1;
        if (tg_should_link(tg, cpu)) {
            break;
        }
        if (task_groups[tg].se_on_rq[cpu]) {
            tg_se_erase(cpu, tg);
        }
        if (tg == TG_ROOT) {
            break;
        }
        if (task_groups[tg].parent == TG_ROOT || task_groups[tg].parent == TG_NONE) {
            /* Erased from top tree; if parent is ROOT check ROOT's own link. */
            if (task_groups[tg].parent == TG_ROOT) {
                tg = TG_ROOT;
                continue;
            }
            break;
        }
        tg = task_groups[tg].parent;
    }
}

static void tg_account_enqueue(u32 id, u32 cpu) {
    u32 tg;
    if (id >= MAX_THREADS || cpu >= SCHED_MAX_CPUS) {
        return;
    }
    tg = threads[id].tg;
    if (!tg_valid(tg)) {
        threads[id].tg = TG_ROOT;
        tg = TG_ROOT;
    }
    task_groups[tg].nr[cpu] = task_groups[tg].nr[cpu] + 1;
    if (task_groups[tg].nr[cpu] == 1) {
        tg_link_runnable(tg, cpu);
    }
}

static void tg_account_dequeue(u32 id, u32 cpu) {
    u32 tg;
    if (id >= MAX_THREADS || cpu >= SCHED_MAX_CPUS) {
        return;
    }
    tg = threads[id].tg;
    if (!tg_valid(tg)) {
        return;
    }
    if (task_groups[tg].nr[cpu] > 0) {
        task_groups[tg].nr[cpu] = task_groups[tg].nr[cpu] - 1;
    }
    if (task_groups[tg].nr[cpu] == 0) {
        tg_unlink_idle(tg, cpu);
    }
}

/* Charge leaf then walk parents (nested SE accounting). Stop before charging
 * TG_ROOT for a nested descendant — ROOT only charges for direct tasks. */
static void tg_charge_one(u32 tg, u32 cpu, u64 units) {
    u64 shares;
    u32 on;
    if (!tg_valid(tg) || cpu >= SCHED_MAX_CPUS) {
        return;
    }
    shares = task_groups[tg].shares;
    if (shares == 0) {
        shares = NICE0_WEIGHT;
    }
    on = task_groups[tg].se_on_rq[cpu];
    if (on) {
        tg_se_erase(cpu, tg);
    }
    task_groups[tg].vruntime[cpu] =
        task_groups[tg].vruntime[cpu] + (units * (u64)NICE0_WEIGHT) / shares;
    if (on || tg_should_link(tg, cpu)) {
        tg_se_insert(cpu, tg);
    }
}

static void tg_charge(u32 tg, u32 cpu, u64 units) {
    u32 guard = 0;
    while (tg_valid(tg) && guard < MAX_TASK_GROUPS) {
        guard = guard + 1;
        tg_charge_one(tg, cpu, units);
        if (tg == TG_ROOT) {
            break;
        }
        {
            u32 p = task_groups[tg].parent;
            if (p == TG_NONE || p == TG_ROOT) {
                break;
            }
            tg = p;
        }
    }
}

/* Pick leftmost top-level SE, then descend nested child SE trees to a leaf. */
static u32 tg_pick_min(u32 cpu) {
    u32 tg;
    u32 guard;
    if (cpu >= SCHED_MAX_CPUS) {
        return TG_NONE;
    }
    tg = tg_se_rq_leftmost(cpu);
    guard = 0;
    while (tg != TG_NONE && tg_has_nested_runnable(tg, cpu)
           && guard < MAX_TASK_GROUPS) {
        u32 root = task_groups[tg].child_se_root[cpu];
        guard = guard + 1;
        if (root >= MAX_TASK_GROUPS || !task_groups[root].se_on_rq[cpu]) {
            break;
        }
        tg = tg_se_leftmost_from(cpu, root);
    }
    return tg;
}

static void cfs_rb_clear_node(u32 id) {
    if (id >= MAX_THREADS) {
        return;
    }
    threads[id].cfs_left = CFS_RB_NONE;
    threads[id].cfs_right = CFS_RB_NONE;
    threads[id].cfs_parent = CFS_RB_NONE;
    threads[id].cfs_color = CFS_RB_RED;
    threads[id].cfs_on_rq = 0;
}

/* Strict weak order for CFS entities: vruntime, then TCB id. */
static u32 cfs_entity_before(u32 a, u32 b) {
    if (threads[a].vruntime < threads[b].vruntime) {
        return 1;
    }
    if (threads[a].vruntime > threads[b].vruntime) {
        return 0;
    }
    return a < b;
}

static u32 cfs_rb_leftmost_from(u32 id) {
    u32 guard = 0;
    if (id >= MAX_THREADS) {
        return CFS_RB_NONE;
    }
    while (threads[id].cfs_left < MAX_THREADS) {
        id = threads[id].cfs_left;
        guard = guard + 1;
        if (guard > MAX_THREADS) {
            return CFS_RB_NONE;
        }
    }
    return id;
}

static u32 cfs_rq_leftmost(u32 cpu) {
    u32 root;
    if (cpu >= SCHED_MAX_CPUS) {
        return CFS_RB_NONE;
    }
    root = cfs_root[cpu];
    if (root >= MAX_THREADS || !threads[root].cfs_on_rq) {
        return CFS_RB_NONE;
    }
    return cfs_rb_leftmost_from(root);
}

static u32 cfs_rb_successor(u32 id) {
    u32 p;
    u32 guard = 0;
    if (id >= MAX_THREADS) {
        return CFS_RB_NONE;
    }
    if (threads[id].cfs_right < MAX_THREADS) {
        return cfs_rb_leftmost_from(threads[id].cfs_right);
    }
    p = threads[id].cfs_parent;
    while (p < MAX_THREADS && id == threads[p].cfs_right) {
        id = p;
        p = threads[p].cfs_parent;
        guard = guard + 1;
        if (guard > MAX_THREADS) {
            return CFS_RB_NONE;
        }
    }
    if (p >= MAX_THREADS) {
        return CFS_RB_NONE;
    }
    return p;
}

static void cfs_rb_rotate_left(u32 cpu, u32 x) {
    u32 y;
    u32 yp;
    if (cpu >= SCHED_MAX_CPUS || x >= MAX_THREADS) {
        return;
    }
    y = threads[x].cfs_right;
    if (y >= MAX_THREADS) {
        return;
    }
    threads[x].cfs_right = threads[y].cfs_left;
    if (threads[y].cfs_left != CFS_RB_NONE) {
        threads[threads[y].cfs_left].cfs_parent = x;
    }
    yp = threads[x].cfs_parent;
    threads[y].cfs_parent = yp;
    if (yp == CFS_RB_NONE) {
        cfs_root[cpu] = y;
    } else if (x == threads[yp].cfs_left) {
        threads[yp].cfs_left = y;
    } else {
        threads[yp].cfs_right = y;
    }
    threads[y].cfs_left = x;
    threads[x].cfs_parent = y;
}

static void cfs_rb_rotate_right(u32 cpu, u32 x) {
    u32 y;
    u32 yp;
    if (cpu >= SCHED_MAX_CPUS || x >= MAX_THREADS) {
        return;
    }
    y = threads[x].cfs_left;
    if (y >= MAX_THREADS) {
        return;
    }
    threads[x].cfs_left = threads[y].cfs_right;
    if (threads[y].cfs_right != CFS_RB_NONE) {
        threads[threads[y].cfs_right].cfs_parent = x;
    }
    yp = threads[x].cfs_parent;
    threads[y].cfs_parent = yp;
    if (yp == CFS_RB_NONE) {
        cfs_root[cpu] = y;
    } else if (x == threads[yp].cfs_right) {
        threads[yp].cfs_right = y;
    } else {
        threads[yp].cfs_left = y;
    }
    threads[y].cfs_right = x;
    threads[x].cfs_parent = y;
}

static void cfs_rb_insert_fixup(u32 cpu, u32 z) {
    while (z != CFS_RB_NONE && threads[z].cfs_parent != CFS_RB_NONE
           && threads[threads[z].cfs_parent].cfs_color == CFS_RB_RED) {
        u32 p = threads[z].cfs_parent;
        u32 g = threads[p].cfs_parent;
        u32 y;
        if (g == CFS_RB_NONE) {
            break;
        }
        if (p == threads[g].cfs_left) {
            y = threads[g].cfs_right;
            if (y != CFS_RB_NONE && threads[y].cfs_color == CFS_RB_RED) {
                threads[p].cfs_color = CFS_RB_BLACK;
                threads[y].cfs_color = CFS_RB_BLACK;
                threads[g].cfs_color = CFS_RB_RED;
                z = g;
            } else {
                if (z == threads[p].cfs_right) {
                    z = p;
                    cfs_rb_rotate_left(cpu, z);
                    p = threads[z].cfs_parent;
                    g = (p == CFS_RB_NONE) ? CFS_RB_NONE : threads[p].cfs_parent;
                }
                if (p != CFS_RB_NONE) {
                    threads[p].cfs_color = CFS_RB_BLACK;
                }
                if (g != CFS_RB_NONE) {
                    threads[g].cfs_color = CFS_RB_RED;
                    cfs_rb_rotate_right(cpu, g);
                }
            }
        } else {
            y = threads[g].cfs_left;
            if (y != CFS_RB_NONE && threads[y].cfs_color == CFS_RB_RED) {
                threads[p].cfs_color = CFS_RB_BLACK;
                threads[y].cfs_color = CFS_RB_BLACK;
                threads[g].cfs_color = CFS_RB_RED;
                z = g;
            } else {
                if (z == threads[p].cfs_left) {
                    z = p;
                    cfs_rb_rotate_right(cpu, z);
                    p = threads[z].cfs_parent;
                    g = (p == CFS_RB_NONE) ? CFS_RB_NONE : threads[p].cfs_parent;
                }
                if (p != CFS_RB_NONE) {
                    threads[p].cfs_color = CFS_RB_BLACK;
                }
                if (g != CFS_RB_NONE) {
                    threads[g].cfs_color = CFS_RB_RED;
                    cfs_rb_rotate_left(cpu, g);
                }
            }
        }
    }
    if (cfs_root[cpu] != CFS_RB_NONE) {
        threads[cfs_root[cpu]].cfs_color = CFS_RB_BLACK;
    }
}

static void cfs_rb_insert(u32 cpu, u32 z) {
    u32 y;
    u32 x;
    if (cpu >= SCHED_MAX_CPUS || z >= MAX_THREADS) {
        return;
    }
    if (threads[z].cfs_on_rq) {
        return;
    }
    threads[z].cfs_left = CFS_RB_NONE;
    threads[z].cfs_right = CFS_RB_NONE;
    threads[z].cfs_parent = CFS_RB_NONE;
    threads[z].cfs_color = CFS_RB_RED;
    y = CFS_RB_NONE;
    x = cfs_root[cpu];
    while (x != CFS_RB_NONE) {
        y = x;
        if (cfs_entity_before(z, x)) {
            x = threads[x].cfs_left;
        } else {
            x = threads[x].cfs_right;
        }
    }
    threads[z].cfs_parent = y;
    if (y == CFS_RB_NONE) {
        cfs_root[cpu] = z;
    } else if (cfs_entity_before(z, y)) {
        threads[y].cfs_left = z;
    } else {
        threads[y].cfs_right = z;
    }
    threads[z].cfs_on_rq = 1;
    cfs_nr[cpu] = cfs_nr[cpu] + 1;
    tg_account_enqueue(z, cpu);
    cfs_rb_insert_fixup(cpu, z);
}

static void cfs_rb_transplant(u32 cpu, u32 u, u32 v) {
    u32 up;
    if (cpu >= SCHED_MAX_CPUS || u >= MAX_THREADS) {
        return;
    }
    up = threads[u].cfs_parent;
    if (up == CFS_RB_NONE) {
        cfs_root[cpu] = v;
    } else if (u == threads[up].cfs_left) {
        threads[up].cfs_left = v;
    } else {
        threads[up].cfs_right = v;
    }
    if (v != CFS_RB_NONE) {
        threads[v].cfs_parent = up;
    }
}

static void cfs_rb_erase_fixup(u32 cpu, u32 x, u32 xp) {
    while ((x == CFS_RB_NONE || threads[x].cfs_color == CFS_RB_BLACK)
           && x != cfs_root[cpu]) {
        u32 w;
        if (xp == CFS_RB_NONE) {
            break;
        }
        if (x == threads[xp].cfs_left) {
            w = threads[xp].cfs_right;
            if (w != CFS_RB_NONE && threads[w].cfs_color == CFS_RB_RED) {
                threads[w].cfs_color = CFS_RB_BLACK;
                threads[xp].cfs_color = CFS_RB_RED;
                cfs_rb_rotate_left(cpu, xp);
                w = threads[xp].cfs_right;
            }
            if (w == CFS_RB_NONE) {
                x = xp;
                xp = threads[x].cfs_parent;
                continue;
            }
            if ((threads[w].cfs_left == CFS_RB_NONE
                 || threads[threads[w].cfs_left].cfs_color == CFS_RB_BLACK)
                && (threads[w].cfs_right == CFS_RB_NONE
                    || threads[threads[w].cfs_right].cfs_color == CFS_RB_BLACK)) {
                threads[w].cfs_color = CFS_RB_RED;
                x = xp;
                xp = threads[x].cfs_parent;
            } else {
                if (threads[w].cfs_right == CFS_RB_NONE
                    || threads[threads[w].cfs_right].cfs_color == CFS_RB_BLACK) {
                    if (threads[w].cfs_left != CFS_RB_NONE) {
                        threads[threads[w].cfs_left].cfs_color = CFS_RB_BLACK;
                    }
                    threads[w].cfs_color = CFS_RB_RED;
                    cfs_rb_rotate_right(cpu, w);
                    w = threads[xp].cfs_right;
                }
                if (w != CFS_RB_NONE) {
                    threads[w].cfs_color = threads[xp].cfs_color;
                    if (threads[w].cfs_right != CFS_RB_NONE) {
                        threads[threads[w].cfs_right].cfs_color = CFS_RB_BLACK;
                    }
                }
                threads[xp].cfs_color = CFS_RB_BLACK;
                cfs_rb_rotate_left(cpu, xp);
                x = cfs_root[cpu];
                xp = CFS_RB_NONE;
            }
        } else {
            w = threads[xp].cfs_left;
            if (w != CFS_RB_NONE && threads[w].cfs_color == CFS_RB_RED) {
                threads[w].cfs_color = CFS_RB_BLACK;
                threads[xp].cfs_color = CFS_RB_RED;
                cfs_rb_rotate_right(cpu, xp);
                w = threads[xp].cfs_left;
            }
            if (w == CFS_RB_NONE) {
                x = xp;
                xp = threads[x].cfs_parent;
                continue;
            }
            if ((threads[w].cfs_right == CFS_RB_NONE
                 || threads[threads[w].cfs_right].cfs_color == CFS_RB_BLACK)
                && (threads[w].cfs_left == CFS_RB_NONE
                    || threads[threads[w].cfs_left].cfs_color == CFS_RB_BLACK)) {
                threads[w].cfs_color = CFS_RB_RED;
                x = xp;
                xp = threads[x].cfs_parent;
            } else {
                if (threads[w].cfs_left == CFS_RB_NONE
                    || threads[threads[w].cfs_left].cfs_color == CFS_RB_BLACK) {
                    if (threads[w].cfs_right != CFS_RB_NONE) {
                        threads[threads[w].cfs_right].cfs_color = CFS_RB_BLACK;
                    }
                    threads[w].cfs_color = CFS_RB_RED;
                    cfs_rb_rotate_left(cpu, w);
                    w = threads[xp].cfs_left;
                }
                if (w != CFS_RB_NONE) {
                    threads[w].cfs_color = threads[xp].cfs_color;
                    if (threads[w].cfs_left != CFS_RB_NONE) {
                        threads[threads[w].cfs_left].cfs_color = CFS_RB_BLACK;
                    }
                }
                threads[xp].cfs_color = CFS_RB_BLACK;
                cfs_rb_rotate_right(cpu, xp);
                x = cfs_root[cpu];
                xp = CFS_RB_NONE;
            }
        }
    }
    if (x != CFS_RB_NONE) {
        threads[x].cfs_color = CFS_RB_BLACK;
    }
}

static void cfs_rb_erase(u32 cpu, u32 z) {
    u32 y;
    u32 x;
    u32 xp;
    u32 y_orig_color;
    if (cpu >= SCHED_MAX_CPUS || z >= MAX_THREADS) {
        return;
    }
    if (!threads[z].cfs_on_rq) {
        return;
    }
    y = z;
    y_orig_color = threads[y].cfs_color;
    if (threads[z].cfs_left == CFS_RB_NONE) {
        x = threads[z].cfs_right;
        xp = threads[z].cfs_parent;
        cfs_rb_transplant(cpu, z, threads[z].cfs_right);
    } else if (threads[z].cfs_right == CFS_RB_NONE) {
        x = threads[z].cfs_left;
        xp = threads[z].cfs_parent;
        cfs_rb_transplant(cpu, z, threads[z].cfs_left);
    } else {
        y = cfs_rb_leftmost_from(threads[z].cfs_right);
        y_orig_color = threads[y].cfs_color;
        x = threads[y].cfs_right;
        if (threads[y].cfs_parent == z) {
            xp = y;
            if (x != CFS_RB_NONE) {
                threads[x].cfs_parent = y;
            }
        } else {
            xp = threads[y].cfs_parent;
            cfs_rb_transplant(cpu, y, threads[y].cfs_right);
            threads[y].cfs_right = threads[z].cfs_right;
            if (threads[y].cfs_right != CFS_RB_NONE) {
                threads[threads[y].cfs_right].cfs_parent = y;
            }
        }
        cfs_rb_transplant(cpu, z, y);
        threads[y].cfs_left = threads[z].cfs_left;
        if (threads[y].cfs_left != CFS_RB_NONE) {
            threads[threads[y].cfs_left].cfs_parent = y;
        }
        threads[y].cfs_color = threads[z].cfs_color;
    }
    threads[z].cfs_on_rq = 0;
    threads[z].cfs_left = CFS_RB_NONE;
    threads[z].cfs_right = CFS_RB_NONE;
    threads[z].cfs_parent = CFS_RB_NONE;
    threads[z].cfs_color = CFS_RB_RED;
    if (cfs_nr[cpu] > 0) {
        cfs_nr[cpu] = cfs_nr[cpu] - 1;
    }
    tg_account_dequeue(z, cpu);
    if (y_orig_color == CFS_RB_BLACK) {
        cfs_rb_erase_fixup(cpu, x, xp);
    }
}

/* Link a runnable non-idle TCB into its home CPU CFS tree. */
static void cfs_enqueue_locked(u32 id) {
    u32 cpu;
    if (id >= MAX_THREADS || tcb_is_idle(id)) {
        return;
    }
    if (threads[id].state != TCB_RUNNABLE) {
        return;
    }
    if (threads[id].cfs_on_rq) {
        return;
    }
    cpu = threads[id].rq_cpu;
    if (cpu >= SCHED_MAX_CPUS) {
        cpu = sched_cpu_index();
        threads[id].rq_cpu = cpu;
    }
    cfs_rb_insert(cpu, id);
}

/* Unlink from whatever CPU tree currently holds this entity. */
static void cfs_dequeue_locked(u32 id) {
    u32 cpu;
    if (id >= MAX_THREADS) {
        return;
    }
    if (!threads[id].cfs_on_rq) {
        return;
    }
    cpu = threads[id].rq_cpu;
    if (cpu >= SCHED_MAX_CPUS) {
        /* Orphaned link: drop flag rather than mutating a random RQ. */
        threads[id].cfs_on_rq = 0;
        threads[id].cfs_left = CFS_RB_NONE;
        threads[id].cfs_right = CFS_RB_NONE;
        threads[id].cfs_parent = CFS_RB_NONE;
        threads[id].cfs_color = CFS_RB_RED;
        /* Best-effort: cannot attribute a CPU for tg.nr[]. */
        return;
    }
    cfs_rb_erase(cpu, id);
}

static void cfs_requeue_locked(u32 id) {
    if (id >= MAX_THREADS || !threads[id].cfs_on_rq) {
        return;
    }
    cfs_dequeue_locked(id);
    if (threads[id].state == TCB_RUNNABLE && !tcb_is_idle(id)) {
        cfs_enqueue_locked(id);
    }
}

static void cfs_migrate_locked(u32 id, u32 new_cpu) {
    if (id >= MAX_THREADS) {
        return;
    }
    if (new_cpu >= SCHED_MAX_CPUS) {
        new_cpu = 0;
    }
    if (threads[id].cfs_on_rq) {
        cfs_dequeue_locked(id);
    }
    threads[id].rq_cpu = new_cpu;
    if (threads[id].state == TCB_RUNNABLE && !tcb_is_idle(id)) {
        cfs_enqueue_locked(id);
    } else {
        threads[id].rq_cpu = new_cpu;
    }
}

static void cfs_charge(u32 id, u64 units) {
    u64 w;
    u32 cpu;
    u32 tg;
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
    if (threads[id].cfs_on_rq) {
        cfs_dequeue_locked(id);
    }
    threads[id].vruntime = threads[id].vruntime + (units * (u64)NICE0_WEIGHT) / w;
    /* Hierarchical: charge the task_group entity on this RQ as well. */
    cpu = threads[id].on_cpu;
    if (cpu >= SCHED_MAX_CPUS) {
        cpu = threads[id].rq_cpu;
    }
    if (cpu >= SCHED_MAX_CPUS) {
        cpu = sched_cpu_index();
    }
    tg = threads[id].tg;
    if (!tg_valid(tg)) {
        tg = TG_ROOT;
        threads[id].tg = TG_ROOT;
    }
    tg_charge(tg, cpu, units);
    if (threads[id].state == TCB_RUNNABLE) {
        cfs_enqueue_locked(id);
    }
}

static void cfs_place_entity_cpu(u32 cpu, u32 id) {
    if (id >= MAX_THREADS || tcb_is_idle(id) || cpu >= SCHED_MAX_CPUS) {
        return;
    }
    if (threads[id].vruntime < cfs_min_vr_cpu[cpu]) {
        threads[id].vruntime = cfs_min_vr_cpu[cpu];
    }
}

static void cfs_update_min_cpu(u32 cpu) {
    u32 id;
    if (!active || cpu >= SCHED_MAX_CPUS) {
        return;
    }
    id = cfs_rq_leftmost(cpu);
    if (id != CFS_RB_NONE) {
        cfs_min_vr_cpu[cpu] = threads[id].vruntime;
    }
}

static void cfs_update_min(void) {
    cfs_update_min_cpu(sched_cpu_index());
}

/* All process_find users hold run_lock. No externally supplied pointer is
 * accepted as a process identity; native capabilities are not exposed yet. */
static void proc_owner_init_defaults(struct process_owner *p, u32 install_console) {
    u32 f;
    u32 g;
    for (f = 0; f < FD_MAX; f++) {
        p->fd[f] = FD_EMPTY;
        p->deferred_close[f] = FD_EMPTY;
    }
    p->ndeferred = 0;
    if (install_console) {
        p->fd[0] = fs_console_fh();
        p->fd[1] = fs_console_fh();
        p->fd[2] = fs_console_fh();
    }
    p->cwd[0] = '/';
    p->cwd[1] = 0;
    p->cmdline[0] = 0;
    p->cmdline_len = 0;
    p->environ[0] = 0;
    p->environ_len = 0;
    p->umask = 0x12;
    p->uid = 0;
    p->euid = 0;
    p->gid = 0;
    p->egid = 0;
    p->ngroups = 0;
    for (g = 0; g < 8; g++) {
        p->groups[g] = 0;
    }
}

static struct process_owner *kernel_owner_ensure(void) {
    if (!kernel_proc_inited) {
        proc_owner_init_defaults(&kernel_proc_owner, 0);
        kernel_proc_inited = 1;
    }
    return &kernel_proc_owner;
}

static struct process_owner *process_find(u32 pid) {
    u32 i;
    if (!pid) return 0;
    for (i=0;i<MAX_THREADS;i++) if (processes[i].pid==pid) return &processes[i];
    return 0;
}

static struct process_owner *process_or_kernel_locked(u32 pid) {
    struct process_owner *p;
    if (pid == 0) return kernel_owner_ensure();
    p = process_find(pid);
    return p ? p : kernel_owner_ensure();
}

static u32 process_attach_ex(u32 id, u32 native_spawned, u32 install_console) {
    u64 flags=cpu_irq_save(); u32 i; u32 ok=0; u32 n;
    struct process_owner *p;
    runlock_lock();
    p=process_find(threads[id].pid);
    if (!p) {
        for(i=0;i<MAX_THREADS;i++) if(!processes[i].pid) {
            p=&processes[i];
            p->pid=threads[id].pid;p->ppid=threads[id].ppid;
            p->live=0;p->resident=0;p->exiting=0;p->code=0;
            p->image_busy=0;p->drained=0;p->native_spawned=native_spawned;
            p->cpu_mask=0;
            p->as_h=threads[id].as_h;
            for(n=0;n<NAT_CAPS;n++) {
                p->nat_caps[n].gen = 0;
                p->nat_caps[n].rights = 0;
                p->nat_caps[n].handle = 0;
            }
            p->nat_used = 0;
            proc_owner_init_defaults(p, install_console);
            break;
        }
    }
    if(p && !p->exiting && !p->image_busy && p->as_h==threads[id].as_h) {
        p->live=p->live+1;p->resident=p->resident+1;ok=1;
    }
    runlock_unlock();cpu_irq_restore(flags);
    if(!p)kprintf("[PROCESS] owner table full: admission rolled back\n");
    return ok;
}

static u32 process_attach(u32 id) {
    return process_attach_ex(id, 0, 1);
}
/* ---------------------------------------------------------------------------
 * natcap bridge: the per-process capability table lives in the owner record.
 * Acquire pins sched_run_lock and returns the table + used counter; release
 * must follow on the same CPU without any blocking in between. native.c
 * operations (nc_lock; may sleep) run strictly OUTSIDE the acquire/release
 * window so the nc_lock -> run_lock order is never inverted. */
static u64 nat_irq_flags[SCHED_MAX_CPUS];
extern s32 nat_fork_install(u32 parent, u32 child);
extern void kprintf(const char *fmt, ...);
extern void kprintf(const char *fmt, ...);
u32 sched_nat_acquire(u32 pid, struct nat_cap **caps_out, u32 **used_out) {
    u64 flags;
    u32 cpu;
    struct process_owner *p;
    cpu = sched_cpu_index();
    flags = cpu_irq_save();
    runlock_lock();
    p = process_find(pid);
    if (p) {
        nat_irq_flags[cpu] = flags;
        *caps_out = p->nat_caps;
        *used_out = &p->nat_used;
        return cpu + 1;                     /* token: release restores IRQs */
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    return 0;
}
void sched_nat_release(u32 token) {
    u32 cpu = token - 1;
    u64 flags = nat_irq_flags[cpu];
    runlock_unlock();
    cpu_irq_restore(flags);
}
/* Roll back the owner record of a never-published fork child (nat inherit
 * failed): the child holds no live tasks and its single AS reference is
 * destroyed by the fork caller. */
static void nat_rollback_owner(u32 pid) {
    u64 flags=cpu_irq_save();struct process_owner *p;
    runlock_lock();p=process_find(pid);
    if(p && !p->live && !p->exiting) {
        p->pid=0;p->ppid=0;p->as_h=0;p->image_busy=0;p->drained=0;p->native_spawned=0;
    }
    runlock_unlock();cpu_irq_restore(flags);
}

u32 sched_process_image_begin(void) {
    u64 flags=cpu_irq_save();u32 ok=0;struct process_owner *p;
    u32 seen=0,live=0,res=0,busy=0,exit=0;
    u32 pid=threads[sched_cur()].pid;
    runlock_lock();p=process_find(pid);
    if(p) {seen=1;live=p->live;res=p->resident;busy=p->image_busy;exit=p->exiting;}
    if(seen && live==1 && res==1 && !exit && !busy) {p->image_busy=1;ok=1;}
    runlock_unlock();cpu_irq_restore(flags);
    /* Fork/exec admission refused. Say WHY: the caller (the shell) can only
     * report a generic failure, and a silently refused fork used to leave the
     * shell waiting for a child that was never created. The reason must come
     * from THIS transaction: a second read after the lock is dropped can
     * report an eligibility the refusal never saw -- a peer task exiting in
     * between turns the real live=2 resident=2 into a self-contradictory 1/1. */
    if(!ok) {
        kprintf("[PROCESS] fork refused: owner present=%u live=%u resident=%u busy=%u exiting=%u pid=%u\n",
                (u64)seen,(u64)live,(u64)res,(u64)busy,(u64)exit,(u64)pid);
    }
    return ok;
}
void sched_process_image_end(void) {
    u64 flags=cpu_irq_save();struct process_owner *p;
    runlock_lock();p=process_find(threads[sched_cur()].pid);
    if(p)p->image_busy=0;
    runlock_unlock();cpu_irq_restore(flags);
}
u32 sched_process_parent(u32 pid) {
    u64 flags=cpu_irq_save();u32 parent=0;struct process_owner *p;
    runlock_lock();p=process_find(pid);if(p)parent=p->ppid;
    runlock_unlock();cpu_irq_restore(flags);return parent;
}
void sched_process_group_exit(u64 code) {
    u64 flags=cpu_irq_save();struct process_owner *p;
    runlock_lock();p=process_find(threads[sched_cur()].pid);
    if(p && !p->exiting) {p->exiting=1;p->code=(u32)code;}
    runlock_unlock();cpu_irq_restore(flags);
}
/* Caller has already cleared TID/closed its copied fds and activated kernel
 * CR3. Detach the last borrowed AS under lock; destruction may SLEEP, so its
 * sole owner reference is returned to be released OUTSIDE run_lock. */
u64 sched_process_leave(u64 code) {
    u64 flags=cpu_irq_save();u64 as=0;struct process_owner *p;
    runlock_lock();p=process_find(threads[sched_cur()].pid);
    if(p && threads[sched_cur()].as_h && p->live) {
        mem.as_activate_h(0);
        threads[sched_cur()].as_h=0;p->live=p->live-1;
        p->cpu_mask=p->cpu_mask | (1u<<sched_cpu_index());
        if(!p->live) {
            as=p->as_h;p->as_h=0;
            if(!p->exiting)p->code=(u32)code;
            p->exiting=1;
        }
    }
    runlock_unlock();cpu_irq_restore(flags);return as;
}
/* Live process count for the fork admission limit (P0-9): owner records
 * with a live task, computed under the same lock process_attach uses. */
/* Fork admission limit: keep 8 slots of headroom for kernel threads and
 * in-flight creation (P0-9). sched_max_threads() already exists above. */
u32 sched_fork_proc_limit(void) {
    return MAX_THREADS - 8;
}

/* Read-side accessor for the vmcc stats (struct copy, fakecc-safe). */
void sched_runlock_stats(u32 cpu, u64 *holds, u64 *contends, u64 *hold_ns,
                         u64 *hold_max) {
    struct rl_stats *s = &rl_s[cpu];
    *holds = s->holds;
    *contends = s->contends;
    *hold_ns = s->hold_ns;
    *hold_max = s->hold_max;
}

u32 sched_live_process_count(void) {
    u64 flags=cpu_irq_save();u32 i;u32 n=0;
    runlock_lock();
    for(i=0;i<MAX_THREADS;i++) {
        if(processes[i].pid && processes[i].live) n=n+1;
    }
    runlock_unlock();cpu_irq_restore(flags);
    return n;
}

u32 sched_process_exit_ready_ex(u32 *native_spawned_out) {
    u64 flags=cpu_irq_save();u32 code=0,mask=0,pid=0,ns=0;struct process_owner *p;
    runlock_lock();p=process_find(threads[sched_cur()].pid);
    if(p && !p->live && !p->as_h && !p->drained) {
        p->drained=1;code=p->code;mask=p->cpu_mask;pid=p->pid;ns=p->native_spawned;
        if (ns && !p->resident) p->pid = 0;
    }
    runlock_unlock();cpu_irq_restore(flags);
    if (native_spawned_out) *native_spawned_out = ns;
    kprintf("[PROCESS] AS drained pid=%u task-exit CPU-mask=0x%x\n",(u64)pid,(u64)mask);
    return code;
}

u32 sched_process_exit_ready(void) {
    return sched_process_exit_ready_ex(0);
}

/* Best CFS pick on one runqueue. want_user: 0=kernel only, 1=user only, 2=any.
 * Nested hierarchical: leftmost top SE, descend child SE trees to a leaf,
 * then the min-vruntime member of that group (task tree filtered by tg).
 * Skips threads running on a foreign CPU (on_cpu set and != cpu). */
static u32 rq_pick_best(u32 cpu, u32 want_user) {
    u32 id;
    u32 tg;
    if (cpu >= SCHED_MAX_CPUS) {
        return SCHED_CPU_NONE;
    }
    tg = tg_pick_min(cpu);
    if (tg == TG_NONE) {
        return SCHED_CPU_NONE;
    }
    id = cfs_rq_leftmost(cpu);
    while (id != CFS_RB_NONE) {
        if (threads[id].tg != tg) {
            id = cfs_rb_successor(id);
            continue;
        }
        if (threads[id].on_cpu != SCHED_CPU_NONE && threads[id].on_cpu != cpu) {
            id = cfs_rb_successor(id);
            continue;
        }
        if (want_user == 0 && threads[id].is_user != 0) {
            id = cfs_rb_successor(id);
            continue;
        }
        if (want_user == 1 && threads[id].is_user == 0) {
            id = cfs_rb_successor(id);
            continue;
        }
        return id;
    }
    /* Group claimed nr>0 but no eligible member (e.g. want_user filter):
     * fall back to any eligible task so we never idle wrongly. */
    id = cfs_rq_leftmost(cpu);
    while (id != CFS_RB_NONE) {
        if (threads[id].on_cpu != SCHED_CPU_NONE && threads[id].on_cpu != cpu) {
            id = cfs_rb_successor(id);
            continue;
        }
        if (want_user == 0 && threads[id].is_user != 0) {
            id = cfs_rb_successor(id);
            continue;
        }
        if (want_user == 1 && threads[id].is_user == 0) {
            id = cfs_rb_successor(id);
            continue;
        }
        return id;
    }
    return SCHED_CPU_NONE;
}

/* Steal one runnable task from a remote RQ onto `cpu`. Caller holds ONLY
 * rq_locks[cpu] (NOT sched_run_lock). Acquires dual RQ locks in strictly
 * ascending CPU index order (min(cpu,remote) -> max(cpu,remote)). */
static u32 rq_steal(u32 cpu) {
    u32 n = smp_cpu_count();
    u32 pass;
    u32 remote;

    if (n <= 1) return SCHED_CPU_NONE;
    if (n > SCHED_MAX_CPUS) {
        n = SCHED_MAX_CPUS;
    }
    for (pass = 0; pass < 2; pass++) {
        for (remote = 0; remote < n; remote++) {
            u32 id;
            if (remote == cpu) {
                continue;
            }
            if (cfs_nr[remote] == 0) {
                continue;
            }
            if (cpu < remote) {
                rq_lock_acquire(remote);
            } else {
                rq_lock_release(cpu);
                rq_lock_acquire(remote);
                rq_lock_acquire(cpu);
                id = rq_pick_best(cpu, 2);
                if (id != SCHED_CPU_NONE) {
                    rq_lock_release(remote);
                    return id;
                }
            }
            id = cfs_rq_leftmost(remote);
            while (id != CFS_RB_NONE) {
                if (threads[id].on_cpu != SCHED_CPU_NONE
                    || threads[id].state != TCB_RUNNABLE) {
                    id = cfs_rb_successor(id);
                    continue;
                }
                if (pass == 0 && threads[id].is_user != 0) {
                    id = cfs_rb_successor(id);
                    continue;
                }
                if (pass == 1 && threads[id].is_user == 0) {
                    id = cfs_rb_successor(id);
                    continue;
                }
                cfs_rb_erase(remote, id);
                cfs_update_min_cpu(remote);
                if (threads[id].vruntime > cfs_min_vr_cpu[remote]) {
                    threads[id].vruntime = (threads[id].vruntime - cfs_min_vr_cpu[remote])
                                         + cfs_min_vr_cpu[cpu];
                } else {
                    threads[id].vruntime = cfs_min_vr_cpu[cpu];
                }
                threads[id].rq_cpu = cpu;
                cfs_rb_insert(cpu, id);
                cfs_update_min_cpu(cpu);
                threads[id].on_cpu = cpu;
                sched_steal_count[cpu] = sched_steal_count[cpu] + 1;
                if (pass == 0) {
                    sched_migrate_kernel = sched_migrate_kernel + 1;
                } else {
                    sched_migrate_user = sched_migrate_user + 1;
                }
                rq_lock_release(remote);
                return id;
            }
            rq_lock_release(remote);
        }
    }
    return SCHED_CPU_NONE;
}

/* Enqueue a newly-runnable (or created) thread onto `cpu`'s runqueue.
 * Caller holds rq_locks[cpu]. */
static void rq_enqueue(u32 id, u32 cpu) {
    if (id >= MAX_THREADS) {
        return;
    }
    if (cpu >= SCHED_MAX_CPUS) {
        cpu = 0;
    }
    if (threads[id].cfs_on_rq) {
        cfs_dequeue_locked(id);
    }
    threads[id].rq_cpu = cpu;
    if (threads[id].state == TCB_RUNNABLE && !tcb_is_idle(id)) {
        cfs_place_entity_cpu(cpu, id);
        cfs_enqueue_locked(id);
        cfs_update_min_cpu(cpu);
    }
}

/* Shared policy, with sched_run_lock held. Takes target rq_locks[cpu]. */
static u32 rq_enqueue_balanced_locked(u32 id) {
    u32 cpu, local = sched_cpu_index();
    u32 kick = 0;
    threads[id].rq_cpu = SCHED_CPU_NONE;
    cpu = rq_select_cpu();
    rq_lock_acquire(cpu);
    rq_enqueue(id, cpu);
    if (cpu != local) {
        sched_balance_remote = sched_balance_remote + 1;
        if (rq_cpu_is_idle(cpu)) kick = 1u << cpu;
    }
    rq_lock_release(cpu);
    return kick;
}
static void rq_enqueue_balanced(u32 id) {
    u64 flags;u32 kicks,c;
    if(id>=MAX_THREADS)return;
    flags=cpu_irq_save();runlock_lock();
    kicks=rq_enqueue_balanced_locked(id);
    runlock_unlock();cpu_irq_restore(flags);
    for(c=0;c<SCHED_MAX_CPUS;c++)if(kicks & (1u<<c))sched_kick_cpu(c);
}
/* Observation only: this does not bind tasks, park APs or suppress stealing. */
u32 sched_idle_cpu_mask(void) {
    u64 flags=cpu_irq_save();u32 c,mask=0,n=smp_cpu_count();
    runlock_lock();
    for(c=0;c<n && c<SCHED_MAX_CPUS;c++) {
        if(c!=sched_bsp_cpu && !ap_in_sched[c])continue;
        if(rq_cpu_is_idle(c))mask=mask | (1u<<c);
    }
    runlock_unlock();cpu_irq_restore(flags);return mask;
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

/* Validate every per-CPU CFS tree: sorted inorder, nr match, on_rq flags. */
u32 sched_cfs_rb_check(void) {
    u32 c;
    u32 ok = 1;
    u64 flags = cpu_irq_save();
    runlock_lock();
    for (c = 0; c < SCHED_MAX_CPUS; c++) {
        u32 id;
        u32 prev = CFS_RB_NONE;
        u32 seen = 0;
        rq_lock_acquire(c);
        id = cfs_rq_leftmost(c);
        while (id != CFS_RB_NONE) {
            if (!threads[id].cfs_on_rq || threads[id].rq_cpu != c
                || threads[id].state != TCB_RUNNABLE || tcb_is_idle(id)) {
                ok = 0;
                break;
            }
            if (prev != CFS_RB_NONE && !cfs_entity_before(prev, id)) {
                ok = 0;
                break;
            }
            prev = id;
            seen = seen + 1;
            id = cfs_rb_successor(id);
        }
        if (ok != 0 && seen != cfs_nr[c]) {
            ok = 0;
        }
        rq_lock_release(c);
        if (ok == 0) {
            break;
        }
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    return ok;
}

/* Validate one SE tree rooted at `root`: sorted, on_rq nodes only. */
static u32 tg_se_check_one_tree(u32 cpu, u32 root, u32 expect_parent) {
    u32 tg;
    u32 prev = TG_NONE;
    u32 seen = 0;
    u32 g;
    u32 expect = 0;
    if (root != TG_NONE) {
        if (root >= MAX_TASK_GROUPS || !task_groups[root].se_on_rq[cpu]) {
            return 0;
        }
        tg = tg_se_leftmost_from(cpu, root);
    } else {
        tg = TG_NONE;
    }
    while (tg != TG_NONE) {
        if (!task_groups[tg].used || !task_groups[tg].se_on_rq[cpu]) {
            return 0;
        }
        if (task_groups[tg].parent != expect_parent) {
            /* Top tree also holds TG_ROOT (parent TG_NONE) when it has tasks. */
            if (!(expect_parent == TG_ROOT
                  && tg == TG_ROOT && task_groups[tg].parent == TG_NONE)) {
                return 0;
            }
        }
        if (!tg_should_link(tg, cpu)) {
            return 0;
        }
        if (prev != TG_NONE && !tg_se_before(cpu, prev, tg)) {
            return 0;
        }
        prev = tg;
        seen = seen + 1;
        tg = tg_se_successor(cpu, tg);
    }
    for (g = 0; g < MAX_TASK_GROUPS; g++) {
        u32 want = 0;
        if (!task_groups[g].used) {
            continue;
        }
        if (expect_parent == TG_ROOT) {
            if (g == TG_ROOT) {
                want = (task_groups[g].nr[cpu] > 0) ? 1 : 0;
            } else if (task_groups[g].parent == TG_ROOT) {
                want = tg_should_link(g, cpu);
            }
        } else if (task_groups[g].parent == expect_parent) {
            want = tg_should_link(g, cpu);
        }
        if (want) {
            expect = expect + 1;
            if (!task_groups[g].se_on_rq[cpu]) {
                return 0;
            }
        } else {
            u32 in_scope = 0;
            if (expect_parent == TG_ROOT) {
                if (g == TG_ROOT || task_groups[g].parent == TG_ROOT) {
                    in_scope = 1;
                }
            } else if (task_groups[g].parent == expect_parent) {
                in_scope = 1;
            }
            if (in_scope && task_groups[g].se_on_rq[cpu]) {
                return 0;
            }
        }
    }
    if (seen != expect) {
        return 0;
    }
    {
        u32 best = TG_NONE;
        u64 best_vr = 0;
        u32 left = TG_NONE;
        for (g = 0; g < MAX_TASK_GROUPS; g++) {
            u32 want = 0;
            if (!task_groups[g].used) {
                continue;
            }
            if (expect_parent == TG_ROOT) {
                if (g == TG_ROOT) {
                    want = (task_groups[g].nr[cpu] > 0) ? 1 : 0;
                } else if (task_groups[g].parent == TG_ROOT) {
                    want = tg_should_link(g, cpu);
                }
            } else if (task_groups[g].parent == expect_parent) {
                want = tg_should_link(g, cpu);
            }
            if (!want) {
                continue;
            }
            if (best == TG_NONE
                || task_groups[g].vruntime[cpu] < best_vr
                || (task_groups[g].vruntime[cpu] == best_vr && g < best)) {
                best = g;
                best_vr = task_groups[g].vruntime[cpu];
            }
        }
        if (root != TG_NONE && root < MAX_TASK_GROUPS
            && task_groups[root].se_on_rq[cpu]) {
            left = tg_se_leftmost_from(cpu, root);
        }
        if (best != left) {
            return 0;
        }
    }
    return 1;
}

/* Validate every per-CPU top + nested child SE tree. Roots stay NONE when empty. */
u32 sched_tg_se_rb_check(void) {
    u32 c;
    u32 ok = 1;
    u64 flags = cpu_irq_save();
    runlock_lock();
    for (c = 0; c < SCHED_MAX_CPUS; c++) {
        u32 g;
        rq_lock_acquire(c);
        if (tg_se_root[c] == 0 && !task_groups[0].se_on_rq[c]) {
            ok = 0;
        } else if (!tg_se_check_one_tree(c, tg_se_root[c], TG_ROOT)) {
            ok = 0;
        } else {
            for (g = 0; g < MAX_TASK_GROUPS; g++) {
                if (!task_groups[g].used) {
                    continue;
                }
                if (task_groups[g].child_se_root[c] == 0) {
                    ok = 0;
                    break;
                }
                if (!tg_se_check_one_tree(c, task_groups[g].child_se_root[c], g)) {
                    ok = 0;
                    break;
                }
            }
        }
        rq_lock_release(c);
        if (ok == 0) {
            break;
        }
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    return ok;
}


u32 sched_is_active(void) {
    return active;
}

void sched_init(void) {
    u32 i;
    proc_owner_init_defaults(&kernel_proc_owner, 0);
    kernel_proc_inited = 1;
    for (i = 0; i < MAX_THREADS; i++) {
        threads[i].state = TCB_FREE;
        threads[i].rsp = 0;
        threads[i].stack_phys = 0;
        threads[i].kstack_top = 0;
        threads[i].entry = 0;
        threads[i].arg = 0;
        threads[i].as_h = 0;
        threads[i].wait_chan = 0;
        threads[i].timer_chan = (u64)&threads[i].timer_chan;
        threads[i].deadline_ns = 0;
        threads[i].deadline_ticks = 0;
        threads[i].timed_out = 0;
        threads[i].fs_base = 0;
        threads[i].clear_child_tid = 0;
        threads[i].pinned_fh = FD_EMPTY;
        threads[i].task_cancelled = 0;
        threads[i].pid = 0;
        threads[i].tid = 0;
        threads[i].ppid = 0;
        threads[i].is_user = 0;
        threads[i].pending_sigs = 0;
        threads[i].park_state = PARK_NONE;
        threads[i].park_notify = 0;
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
        threads[i].cfs_left = CFS_RB_NONE;
        threads[i].cfs_right = CFS_RB_NONE;
        threads[i].cfs_parent = CFS_RB_NONE;
        threads[i].cfs_color = CFS_RB_RED;
        threads[i].cfs_on_rq = 0;
        threads[i].tg = TG_ROOT;
        threads[i].sig_ignore = (1u << 17); /* SIGCHLD ignored by default */
        {
            u32 s;
            for (s = 0; s < 32; s++) {
                threads[i].sig_handler[s] = 0;
                threads[i].sig_restorer[s] = 0;
                threads[i].sig_flags[s] = 0;
            }
        }
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
        cfs_root[i] = CFS_RB_NONE;
        cfs_nr[i] = 0;
        cfs_min_vr_cpu[i] = 0;
        rq_locks[i] = 0;
        rq_holds[i] = 0;
        rq_contends[i] = 0;
        tg_se_root[i] = TG_NONE;
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
    clock_ticks = 0;
    user_mode_ticks = 0;
    cfs_min_vruntime = 0;
    tg_init_root();
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

static void proc_cwd_copy(char *dst, const char *src) {
    u32 i = 0;
    while (src[i] != 0 && i < CWD_LEN - 1) {
        dst[i] = src[i];
        i = i + 1;
    }
    dst[i] = 0;
}

static void fh_retain_backend(u32 fh) {
    if (fh == FD_EMPTY || fh == FD_RESERVED) return;
    if (ext2_fh_is(fh)) {
        ext2_file_retain(fh);
    } else if (pipe_fh_is(fh)) {
        pipe_retain(fh);
    } else if (sock_fh_is(fh)) {
        sock_retain(fh);
    } else if (eventfd_fh_is(fh)) {
        eventfd_retain(fh);
    } else if (epoll_fh_is(fh)) {
        epoll_retain(fh);
    } else {
        fs_file_retain(fh);
    }
}

void sched_fh_close_backend(u32 fh) {
    if (fh == FD_EMPTY || fh == FD_RESERVED) return;
    if (ext2_fh_is(fh)) {
        ext2_close(fh);
    } else if (pipe_fh_is(fh)) {
        pipe_close(fh);
    } else if (sock_fh_is(fh)) {
        sock_close(fh);
    } else if (eventfd_fh_is(fh)) {
        eventfd_close(fh);
    } else if (epoll_fh_is(fh)) {
        epoll_close(fh);
    } else {
        fs_close(fh);
    }
}

static u32 alloc_slot(void) {
    u32 i;
    u64 flags = cpu_irq_save();
    runlock_lock_at(10);
    for (i = 1; i < MAX_THREADS; i++) {
        if (threads[i].state == TCB_FREE) {
            threads[i].state = TCB_RESERVED;
            threads[i].pid = 0;
            threads[i].is_user = 0;
            threads[i].on_cpu = SCHED_CPU_NONE;
            runlock_unlock();
            cpu_irq_restore(flags);
            return i;
        }
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    return 0xFFFFFFFF;
}

static void unreserve_slot(u32 i) {
    u64 flags = cpu_irq_save();
    runlock_lock();
    threads[i].state = TCB_FREE;
    runlock_unlock();
    cpu_irq_restore(flags);
}

static u32 kthread_prepare(u64 entry, u64 arg) {
    u32 i = alloc_slot();
    if (i == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }

    u64 stack_phys = pmm_alloc_pages(STACK_ORDER);
    if (stack_phys == 0) {
        unreserve_slot(i);
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
    threads[i].timer_chan = (u64)&threads[i].timer_chan;
    threads[i].deadline_ns = 0;
    threads[i].deadline_ticks = 0;
    threads[i].timed_out = 0;
    threads[i].fs_base = 0;
    threads[i].clear_child_tid = 0;
    threads[i].pinned_fh = FD_EMPTY;
    threads[i].task_cancelled = 0;
    threads[i].pid = 0;
    threads[i].tid = 0;
    threads[i].ppid = 0;
    threads[i].is_user = 0;
    threads[i].pending_sigs = 0;
    threads[i].sig_ignore = 0;
    cfs_set_nice_fields(&threads[i], 0);
    threads[i].vruntime = cfs_min_vruntime;
    threads[i].tg = TG_ROOT;
    threads[i].on_cpu = SCHED_CPU_NONE;
    return i;
}

u32 kthread_create(u64 entry,u64 arg) {
    u32 id=kthread_prepare(entry,arg);
    if(id==0xFFFFFFFF)return id;
    threads[id].state=TCB_RUNNABLE;rq_enqueue_balanced(id);return id;
}
/* Prepare all resources before publication; commit an entire bounded batch
 * under ONE runqueue transaction. Normal load selection/stealing/IPIs remain
 * unchanged. Failure publishes nothing and rolls back every reserved stack. */
u32 kthread_create_batch(u64 entry,u32 count) {
    u32 ids[MAX_THREADS];u32 n,i,c,kicks=0;u64 flags;
    if(!entry || !count || count>MAX_THREADS)return 0;
    for(n=0;n<count;n++) {
        ids[n]=kthread_prepare(entry,n);
        if(ids[n]==0xFFFFFFFF)break;
    }
    if(n!=count) {
        for(i=0;i<n;i++) {
            u64 sp=threads[ids[i]].stack_phys;
            threads[ids[i]].stack_phys=0;
            pmm_free_pages(sp,STACK_ORDER);unreserve_slot(ids[i]);
        }
        return 0;
    }
    flags=cpu_irq_save();runlock_lock();
    for(i=0;i<count;i++) {
        threads[ids[i]].state=TCB_RUNNABLE;
        kicks=kicks | rq_enqueue_balanced_locked(ids[i]);
    }
    runlock_unlock();cpu_irq_restore(flags);
    for(c=0;c<SCHED_MAX_CPUS;c++)if(kicks & (1u<<c))sched_kick_cpu(c);
    return 1;
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
        unreserve_slot(i);
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
    threads[i].timer_chan = (u64)&threads[i].timer_chan;
    threads[i].deadline_ns = 0;
    threads[i].deadline_ticks = 0;
    threads[i].timed_out = 0;
    threads[i].fs_base = 0;
    threads[i].clear_child_tid = 0;
    threads[i].pinned_fh = FD_EMPTY;
    threads[i].task_cancelled = 0;
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
    cfs_set_nice_fields(&threads[i], 0);
    threads[i].vruntime = cfs_min_vruntime;
    threads[i].tg = TG_ROOT;
    threads[i].on_cpu = SCHED_CPU_NONE;
    if (!process_attach(i)) {
        pmm_free_pages(stack_phys, STACK_ORDER);
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }
    threads[i].state = TCB_RUNNABLE;
    rq_enqueue_balanced(i);
    return i;
}

/* Native zero-legacy process spawn (architecture section 3): builds a fresh
 * isolated process directly from an ELF image without fork() COW cloning,
 * inherits cwd/credentials, installs only explicit bootstrap capabilities,
 * and marks native_spawned=1 so teardown leaves no POSIX wait4 zombie. */
struct sched_spawn_req {
    u64 entry_rip;
    u64 user_rsp;
    u64 as_h;
    u32 child_pid;
    u32 parent_pid;
    u32 install_console;
    u32 ncaps;
    u64 cap_handles[8];
    u32 cap_rights[8];
    const char *cmdline;
    u32 cmdline_len;
    const char *environ;
    u32 environ_len;
};

u32 sched_spawn_native_prepare(struct sched_spawn_req *req) {
    u32 i;
    u64 stack_phys;
    u64 top;
    u64 *frame;
    if (req == 0 || req->as_h == 0 || req->child_pid == 0 || req->ncaps > 8) {
        return 0xFFFFFFFF;
    }
    i = alloc_slot();
    if (i == 0xFFFFFFFF) return 0xFFFFFFFF;
    stack_phys = pmm_alloc_pages(STACK_ORDER);
    if (stack_phys == 0) {
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }
    top = 0xFFFF800000000000ULL + stack_phys + STACK_BYTES;
    frame = (u64 *)(top - 56);
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = req->entry_rip;
    frame[4] = req->user_rsp;
    frame[5] = 0;
    frame[6] = (u64)(&user_iret_trampoline);

    threads[i].rsp = top - 56;
    threads[i].stack_phys = stack_phys;
    threads[i].kstack_top = top;
    threads[i].entry = req->entry_rip;
    threads[i].arg = 0;
    threads[i].as_h = req->as_h;
    threads[i].wait_chan = 0;
    threads[i].timer_chan = (u64)&threads[i].timer_chan;
    threads[i].deadline_ns = 0;
    threads[i].deadline_ticks = 0;
    threads[i].timed_out = 0;
    threads[i].fs_base = 0;
    threads[i].clear_child_tid = 0;
    threads[i].pinned_fh = FD_EMPTY;
    threads[i].task_cancelled = 0;
    threads[i].pid = req->child_pid;
    threads[i].tid = req->child_pid;
    threads[i].ppid = 0;
    threads[i].pgid = req->child_pid;
    threads[i].sid = req->child_pid;
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
    cfs_set_nice_fields(&threads[i], 0);
    threads[i].vruntime = cfs_min_vruntime;
    threads[i].tg = TG_ROOT;
    threads[i].on_cpu = SCHED_CPU_NONE;

    if (!process_attach_ex(i, 1, req->install_console)) {
        pmm_free_pages(stack_phys, STACK_ORDER);
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }

    {
        u64 flags = cpu_irq_save();
        struct process_owner *pc;
        struct process_owner *pp;
        u32 c;
        runlock_lock();
        pc = process_find(req->child_pid);
        pp = process_find(req->parent_pid);
        if (pc) {
            if (pp) {
                proc_cwd_copy(pc->cwd, pp->cwd);
                pc->umask = pp->umask;
                pc->uid = pp->uid;
                pc->euid = pp->euid;
                pc->gid = pp->gid;
                pc->egid = pp->egid;
                pc->ngroups = pp->ngroups;
                for (c = 0; c < 8; c++) pc->groups[c] = pp->groups[c];
            }
            if (req->cmdline) {
                u32 cl = req->cmdline_len > 127 ? 127 : req->cmdline_len;
                for (c = 0; c < cl; c++) pc->cmdline[c] = req->cmdline[c];
                pc->cmdline[cl] = 0;
                pc->cmdline_len = cl;
            }
            if (req->environ) {
                u32 el = req->environ_len > 255 ? 255 : req->environ_len;
                for (c = 0; c < el; c++) pc->environ[c] = req->environ[c];
                pc->environ[el] = 0;
                pc->environ_len = el;
            }
            for (c = 0; c < req->ncaps; c++) {
                struct nat_cap *slot_p = &pc->nat_caps[c];
                if (slot_p->gen < NAT_GEN_MAX) {
                    slot_p->gen = slot_p->gen + 1;
                }
                slot_p->rights = req->cap_rights[c];
                slot_p->handle = req->cap_handles[c];
                pc->nat_used = pc->nat_used + 1;
            }
        }
        runlock_unlock();
        cpu_irq_restore(flags);
    }
    return i;
}

void sched_spawn_native_abort(u32 slot, u32 child_pid) {
    u64 flags = cpu_irq_save();
    u64 sp = 0;
    struct process_owner *p;
    runlock_lock();
    p = process_find(child_pid);
    if (p) {
        u32 c;
        for (c = 0; c < NAT_CAPS; c++) {
            if (p->nat_caps[c].rights != 0) {
                if (p->nat_caps[c].gen < NAT_GEN_MAX) {
                    p->nat_caps[c].gen = p->nat_caps[c].gen + 1;
                }
                p->nat_caps[c].rights = 0;
                p->nat_caps[c].handle = 0;
            }
        }
        p->nat_used = 0;
        p->pid = 0;
        p->ppid = 0;
        p->live = 0;
        p->resident = 0;
        p->as_h = 0;
        p->native_spawned = 0;
    }
    if (slot < MAX_THREADS) {
        sp = threads[slot].stack_phys;
        threads[slot].stack_phys = 0;
        threads[slot].as_h = 0;
        threads[slot].pid = 0;
        threads[slot].tid = 0;
        threads[slot].is_user = 0;
        threads[slot].state = TCB_FREE;
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    if (sp != 0) pmm_free_pages(sp, STACK_ORDER);
}

void sched_spawn_native_publish(u32 slot) {
    if (slot >= MAX_THREADS) return;
    threads[slot].state = TCB_RUNNABLE;
    rq_enqueue_balanced(slot);
}

/* Return bootstrap cap value installed at index `idx` of `child_pid` (for tests/spawn). */
u64 sched_native_bootstrap_cap(u32 child_pid, u32 idx) {
    u64 flags = cpu_irq_save();
    u64 out = 0;
    struct process_owner *p;
    if (idx >= NAT_CAPS) { cpu_irq_restore(flags); return 0; }
    runlock_lock();
    p = process_find(child_pid);
    if (p && p->nat_caps[idx].rights != 0) {
        out = ((u64)p->nat_caps[idx].gen << 16) | (u64)(idx + 1);
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    return out;
}

u32 sched_clone_user(u64 frame_base, u64 child_as_h, u32 child_pid) {
    u32 parent_pid = threads[sched_cur()].pid;
    u32 parent_fhs[FD_MAX];
    u32 i = alloc_slot();
    if (i == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }

    u64 stack_phys = pmm_alloc_pages(STACK_ORDER);
    if (stack_phys == 0) {
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }
    u64 top = 0xFFFF800000000000ULL + stack_phys + STACK_BYTES;

    enum { FRAME_QWORDS = (120 + 8 + 256) / 8 };
    u64 fbase = top - 384;
    u64 *dst = (u64 *)(fbase);
    u64 *src = (u64 *)frame_base;
    u32 k;
    for (k = 0; k < FRAME_QWORDS; k++) {
        dst[k] = src[k];
    }

    u64 *frame = (u64 *)(fbase - 56);
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = fbase;
    frame[4] = 0;
    frame[5] = 0;
    frame[6] = (u64)(&fork_ret_trampoline);

    threads[i].rsp = fbase - 56;
    threads[i].stack_phys = stack_phys;
    threads[i].kstack_top = top;
    threads[i].entry = 0;
    threads[i].arg = 0;
    threads[i].as_h = child_as_h;
    threads[i].wait_chan = 0;
    threads[i].timer_chan = (u64)&threads[i].timer_chan;
    threads[i].deadline_ns = 0;
    threads[i].deadline_ticks = 0;
    threads[i].timed_out = 0;
    threads[i].fs_base = threads[sched_cur()].fs_base;
    threads[i].clear_child_tid = 0;
    threads[i].pinned_fh = FD_EMPTY;
    threads[i].task_cancelled = 0;
    threads[i].pid = child_pid;
    threads[i].tid = child_pid;
    threads[i].ppid = parent_pid;
    threads[i].pgid = threads[sched_cur()].pgid;
    threads[i].sid = threads[sched_cur()].sid;
    threads[i].stop_sig = 0;
    threads[i].stop_notify = 0;
    threads[i].cont_notify = 0;
    threads[i].is_user = 1;
    cfs_set_nice_fields(&threads[i], threads[sched_cur()].nice);
    threads[i].vruntime = cfs_min_vruntime;
    threads[i].tg = threads[sched_cur()].tg;
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

    /* Attach child owner BEFORE retaining backend fds so owner-table-full
     * admission failures never leak retained pipe/sock/fs references. */
    if (!process_attach_ex(i, 0, 0)) {
        pmm_free_pages(stack_phys, STACK_ORDER);
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }

    /* Copy process-owned state and snapshot parent fds under runlock, then
     * retain open-file descriptions OUTSIDE runlock (pipe_retain may sleep). */
    {
        u64 flags = cpu_irq_save();
        struct process_owner *pp;
        struct process_owner *pc;
        u32 f;
        u32 c;
        runlock_lock();
        pp = process_or_kernel_locked(parent_pid);
        pc = process_find(child_pid);
        for (f = 0; f < FD_MAX; f++) {
            u32 fh = pp->fd[f];
            parent_fhs[f] = (fh == FD_RESERVED) ? (u32)FD_EMPTY : fh;
        }
        if (pc) {
            proc_cwd_copy(pc->cwd, pp->cwd);
            for (c = 0; c < 128; c++) pc->cmdline[c] = pp->cmdline[c];
            pc->cmdline_len = pp->cmdline_len;
            for (c = 0; c < 256; c++) pc->environ[c] = pp->environ[c];
            pc->environ_len = pp->environ_len;
            pc->umask = pp->umask;
            pc->uid = pp->uid;
            pc->euid = pp->euid;
            pc->gid = pp->gid;
            pc->egid = pp->egid;
            pc->ngroups = pp->ngroups;
            for (c = 0; c < 8; c++) pc->groups[c] = pp->groups[c];
        }
        runlock_unlock();
        cpu_irq_restore(flags);

        for (f = 0; f < FD_MAX; f++) {
            fh_retain_backend(parent_fhs[f]);
        }

        flags = cpu_irq_save();
        runlock_lock();
        pc = process_find(child_pid);
        if (pc) {
            for (f = 0; f < FD_MAX; f++) {
                pc->fd[f] = parent_fhs[f];
            }
        }
        runlock_unlock();
        cpu_irq_restore(flags);
    }

    if (nat_fork_install(parent_pid, child_pid) != 0) {
        sched_process_close_all_fds(child_pid);
        nat_rollback_owner(child_pid);
        pmm_free_pages(stack_phys, STACK_ORDER);
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }
    threads[i].state = TCB_RUNNABLE;
    rq_enqueue_balanced(i);
    return i;
}

/* clone(CLONE_VM|CLONE_THREAD) / nat_task_spawn: shares the owning process's
 * address space, capability table, descriptor table, cwd and credentials by
 * identity (zero per-thread FD/cwd/env duplication). */
u32 sched_clone_thread(u64 fn, u64 user_stack, u64 arg, u32 child_tid) {
    u32 i = alloc_slot();
    u32 s;
    if (i == 0xFFFFFFFF) {
        return 0xFFFFFFFF;
    }
    if (threads[sched_cur()].as_h == 0 || threads[sched_cur()].is_user == 0) {
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }
    if (fn == 0 || user_stack == 0) {
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }

    u64 stack_phys = pmm_alloc_pages(STACK_ORDER);
    if (stack_phys == 0) {
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }
    u64 top = 0xFFFF800000000000ULL + stack_phys + STACK_BYTES;

    u64 *frame = (u64 *)(top - 56);
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = arg;
    frame[3] = fn;
    frame[4] = user_stack;
    frame[5] = 0;
    frame[6] = (u64)(&user_iret_trampoline);

    threads[i].rsp = top - 56;
    threads[i].stack_phys = stack_phys;
    threads[i].kstack_top = top;
    threads[i].entry = fn;
    threads[i].arg = arg;
    threads[i].as_h = threads[sched_cur()].as_h;
    threads[i].wait_chan = 0;
    threads[i].timer_chan = (u64)&threads[i].timer_chan;
    threads[i].deadline_ns = 0;
    threads[i].deadline_ticks = 0;
    threads[i].timed_out = 0;
    threads[i].fs_base = 0;
    threads[i].clear_child_tid = 0;
    threads[i].pinned_fh = FD_EMPTY;
    threads[i].task_cancelled = 0;
    threads[i].pid = threads[sched_cur()].pid;
    threads[i].tid = child_tid;
    threads[i].ppid = threads[sched_cur()].ppid;
    threads[i].pgid = threads[sched_cur()].pgid;
    threads[i].sid = threads[sched_cur()].sid;
    threads[i].stop_sig = 0;
    threads[i].stop_notify = 0;
    threads[i].cont_notify = 0;
    threads[i].is_user = 1;
    cfs_set_nice_fields(&threads[i], threads[sched_cur()].nice);
    threads[i].vruntime = cfs_min_vruntime;
    threads[i].tg = threads[sched_cur()].tg;
    threads[i].pending_sigs = 0;
    threads[i].sig_ignore = threads[sched_cur()].sig_ignore;
    for (s = 0; s < 32; s++) {
        threads[i].sig_handler[s] = threads[sched_cur()].sig_handler[s];
        threads[i].sig_flags[s] = threads[sched_cur()].sig_flags[s];
        threads[i].sig_restorer[s] = threads[sched_cur()].sig_restorer[s];
    }
    threads[i].on_cpu = SCHED_CPU_NONE;
    if (!process_attach(i)) {
        pmm_free_pages(stack_phys, STACK_ORDER);
        unreserve_slot(i);
        return 0xFFFFFFFF;
    }
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
/* Called by switch_context with IRQs off, on the target stack and holding
 * ONLY rq_locks[cpu]. Stack handoff completes per-CPU; exiting task metadata
 * teardown runs AFTER rq_locks[cpu] is dropped so lock order
 * sched_run_lock -> rq_locks[cpu] is never inverted. */
void sched_finish_switch(u64 prev_id) {
    u32 cpu = sched_cpu_index();
    u32 prev = (u32)prev_id;
    u32 st = threads[prev].state;
    u64 sp = 0;
    threads[prev].on_cpu = SCHED_CPU_NONE;
    if (st == TCB_DEAD || st == TCB_RETIRED) {
        sp = threads[prev].stack_phys;
        threads[prev].stack_phys = 0;
        threads[prev].kstack_top = 0;
        threads[prev].rsp = 0;
    }
    rq_lock_release(cpu);
    if (st == TCB_DEAD || st == TCB_RETIRED) {
        if (sp != 0) pmm_free_pages(sp, STACK_ORDER);
        runlock_lock();
        {
            struct process_owner *p = process_find(threads[prev].pid);
            if (p && p->resident) {
                p->resident = p->resident - 1;
                if (p->native_spawned && p->drained && !p->live
                    && !p->resident && !p->as_h) {
                    p->pid = 0;
                }
            }
        }
        if (threads[prev].state == TCB_RETIRED) {
            threads[prev].pid = 0;
            threads[prev].tid = 0;
            threads[prev].ppid = 0;
            threads[prev].is_user = 0;
            threads[prev].pinned_fh = FD_EMPTY;
            threads[prev].task_cancelled = 0;
            threads[prev].state = TCB_FREE;
        }
        runlock_unlock();
    }
}

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
    /* Assembly invokes sched_finish_switch on the NEW stack before unlocking. */
    switch_context(&threads[prev].rsp, threads[next].rsp, (u64)prev);
}

/* Caller must hold ONLY rq_locks[cpu] (IRQs off). On return rq_locks[cpu] is released. */
static void sched_switch_to(u32 next) {
    u32 cpu = sched_cpu_index();
    u32 prev;
    if (next == sched_cur()) {
        rq_lock_release(cpu);
        return;
    }
    prev = sched_cur();
    threads[next].on_cpu = cpu;
    threads[next].rq_cpu = cpu;
    sched_set_cur(next);
    schedule_tail(prev, next);
}

/* Dequeue `self` and transition its state while holding sched_run_lock,
 * acquiring rq_locks[cpu] in canonical order (sched_run_lock -> rq_locks[cpu]).
 * Caller then drops sched_run_lock BEFORE calling pick_next / sched_switch_to! */
static u32 sched_desched_begin_locked(u32 self, u32 new_state) {
    u32 cpu = sched_cpu_index();
    rq_lock_acquire(cpu);
    if (threads[self].cfs_on_rq) {
        cfs_dequeue_locked(self);
        cfs_update_min_cpu(cpu);
    }
    threads[self].state = new_state;
    return cpu;
}

static u32 wake_task_to_rq_locked(u32 i) {
    u32 cpu = threads[i].rq_cpu;
    u32 kick = 0;
    if (cpu >= SCHED_MAX_CPUS) {
        cpu = sched_cpu_index();
    }
    rq_lock_acquire(cpu);
    threads[i].rq_cpu = cpu;
    cfs_place_entity_cpu(cpu, i);
    threads[i].state = TCB_RUNNABLE;
    cfs_enqueue_locked(i);
    cfs_update_min_cpu(cpu);
    if (rq_cpu_is_idle(cpu)) {
        kick = 1u << cpu;
    }
    rq_lock_release(cpu);
    return kick;
}

/* Voluntary yield: operates purely under per-CPU rq_locks[cpu] (zero global lock!). */
void sched_yield(void) {
    u32 cpu;
    u32 self;
    u32 next;
    if (!active) {
        return;
    }
    arch.cpu_cli();
    cpu = sched_cpu_index();
    rq_lock_acquire(cpu);
    self = sched_cur();
    cfs_charge(self, VRUNTIME_SLICE);
    cfs_update_min_cpu(cpu);
    next = pick_next(self);
    if (next == self && idle_mark[self] != 0) {
        rq_lock_release(cpu);
        arch.cpu_sti_hlt();
        return;
    }
    sched_switch_to(next);
    arch.cpu_sti();
}

u64 sched_monotonic_ns(void) {
    if (tsc_hz() != 0) {
        return tsc_ns();
    }
    return clock_ticks * 10000000ULL;
}

void sched_sleep(u64 chan) {
    u32 self;
    if (!active || chan == 0) {
        return;
    }
    arch.cpu_cli();
    runlock_lock();
    self = sched_cur();
    threads[self].wait_chan = chan;
    (void)sched_desched_begin_locked(self, TCB_SLEEPING);
    runlock_unlock();
    sched_switch_to(pick_next(self));
    arch.cpu_sti();
}

void sched_sleep_cli(u64 chan) {
    u32 self;
    if (!active || chan == 0) {
        return;
    }
    runlock_lock();
    self = sched_cur();
    threads[self].wait_chan = chan;
    (void)sched_desched_begin_locked(self, TCB_SLEEPING);
    runlock_unlock();
    sched_switch_to(pick_next(self));
}

void sched_sleep_unlock_spin(u64 chan, volatile u32 *condition_lock) {
    u32 self;
    runlock_lock();
    self = sched_cur();
    threads[self].wait_chan = chan;
    (void)sched_desched_begin_locked(self, TCB_SLEEPING);
    runlock_unlock();
    spin_unlock(condition_lock);
    sched_switch_to(pick_next(self));
}

u32 sched_should_eintr(void);

u32 sched_park_commit_unlock_spin(u64 chan, volatile u32 *condition_lock,
                                  u32 nonrestartable) {
    u32 self;
    struct tcb *t;
    u32 result;
    runlock_lock();
    self = sched_cur();
    t = &threads[self];
    if (t->park_notify != 0) {
        t->park_state = PARK_NONE;
        t->park_notify = 0;
        t->wait_chan = 0;
        t->deadline_ns = 0;
        t->deadline_ticks = 0;
        t->timed_out = 0;
        result = PARK_WOKEN;
    } else if (t->is_user != 0
               && (t->task_cancelled != 0
                   || (nonrestartable != 0 ? sched_should_eintr_timer()
                                           : sched_should_eintr()) != 0)) {
        t->park_state = PARK_NONE;
        t->park_notify = 0;
        t->wait_chan = 0;
        t->deadline_ns = 0;
        t->deadline_ticks = 0;
        t->timed_out = 0;
        result = PARK_INTERRUPTED;
    } else if ((t->deadline_ns != 0 && sched_monotonic_ns() >= t->deadline_ns)
               || (t->deadline_ticks != 0 && clock_ticks >= t->deadline_ticks)) {
        t->park_state = PARK_NONE;
        t->park_notify = 0;
        t->wait_chan = 0;
        t->deadline_ns = 0;
        t->deadline_ticks = 0;
        t->timed_out = 0;
        result = PARK_TIMED_OUT;
    } else {
        t->wait_chan = chan;
        t->timed_out = 0;
        t->park_state = PARK_NONE;
        (void)sched_desched_begin_locked(self, TCB_SLEEPING);
        result = PARK_PARKED;
    }
    if (result != PARK_PARKED) {
        runlock_unlock();
        spin_unlock(condition_lock);
        return result;
    }
    runlock_unlock();
    spin_unlock(condition_lock);
    sched_switch_to(pick_next(self));
    if (threads[self].task_cancelled != 0) {
        threads[self].timed_out = 0;
        return PARK_INTERRUPTED;
    }
    if (threads[self].timed_out != 0) {
        threads[self].timed_out = 0;
        return PARK_TIMED_OUT;
    }
    return PARK_PARKED;
}

u32 sched_sleep_unlock_spin_intr(u64 chan, volatile u32 *condition_lock) {
    return sched_park_commit_unlock_spin(chan, condition_lock, 0)
           != PARK_INTERRUPTED;
}

void sched_wakeup(u64 chan) {
    u32 i;
    u64 flags;
    u32 kick_mask;
    if (chan == 0) {
        return;
    }
    kick_mask = 0;
    flags = cpu_irq_save();
    runlock_lock();
    for (i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == TCB_SLEEPING && threads[i].wait_chan == chan) {
            threads[i].wait_chan = 0;
            threads[i].deadline_ns = 0;
            threads[i].deadline_ticks = 0;
            threads[i].timed_out = 0;
            kick_mask = kick_mask | wake_task_to_rq_locked(i);
        } else if (threads[i].park_state == PARK_PREPARING
                   && threads[i].wait_chan == chan) {
            threads[i].park_notify = 1;
        }
    }
    runlock_unlock();
    if (kick_mask != 0) {
        for (i = 0; i < SCHED_MAX_CPUS; i++) {
            if ((kick_mask & (1u << i)) != 0) {
                sched_kick_cpu(i);
            }
        }
    }
    cpu_irq_restore(flags);
}

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
    runlock_lock();
    for (i = 0; i < MAX_THREADS && woke < n; i++) {
        if (threads[i].state == TCB_SLEEPING && threads[i].wait_chan == chan) {
            threads[i].wait_chan = 0;
            threads[i].deadline_ns = 0;
            threads[i].deadline_ticks = 0;
            threads[i].timed_out = 0;
            kick_mask = kick_mask | wake_task_to_rq_locked(i);
            woke++;
        } else if (threads[i].park_state == PARK_PREPARING
                   && threads[i].wait_chan == chan) {
            threads[i].park_notify = 1;
            woke++;
        }
    }
    runlock_unlock();
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

u64 sched_wait4_chan(u32 parent_pid) {
    return 0x5774000000000000ULL | (u64)parent_pid;
}

static u32 wake_chan_locked(u64 chan, u32 n, u32 *kick_mask) {
    u32 i;
    u32 woke = 0;
    if (chan == 0 || n == 0) {
        return 0;
    }
    for (i = 0; i < MAX_THREADS && woke < n; i++) {
        if (threads[i].state == TCB_SLEEPING && threads[i].wait_chan == chan) {
            threads[i].wait_chan = 0;
            threads[i].deadline_ns = 0;
            threads[i].deadline_ticks = 0;
            threads[i].timed_out = 0;
            *kick_mask = *kick_mask | wake_task_to_rq_locked(i);
            woke++;
        } else if (threads[i].park_state == PARK_PREPARING
                   && threads[i].wait_chan == chan) {
            threads[i].park_notify = 1;
            woke++;
        }
    }
    return woke;
}

static void kick_idle_cpus(u32 kick_mask) {
    u32 i;
    if (kick_mask == 0) {
        return;
    }
    for (i = 0; i < SCHED_MAX_CPUS; i++) {
        if ((kick_mask & (1u << i)) != 0) {
            sched_kick_cpu(i);
        }
    }
}

void kmutex_init(struct kmutex *m) {
    m->locked = 0;
    m->owner = 0xFFFFFFFFu;
}

void kmutex_lock(struct kmutex *m) {
    arch.cpu_cli();
    runlock_lock();
    while (m->locked) {
        u32 self = sched_cur();
        threads[self].wait_chan = (u64)m;
        (void)sched_desched_begin_locked(self, TCB_SLEEPING);
        runlock_unlock();
        sched_switch_to(pick_next(self));
        runlock_lock();
    }
    m->locked = 1;
    m->owner = threads[sched_cur()].id;
    runlock_unlock();
    arch.cpu_sti();
}

u32 kmutex_trylock(struct kmutex *m) {
    u32 got = 0;
    arch.cpu_cli();
    runlock_lock();
    if (!m->locked) {
        m->locked = 1;
        m->owner = threads[sched_cur()].id;
        got = 1;
    }
    runlock_unlock();
    arch.cpu_sti();
    return got;
}

void kmutex_unlock(struct kmutex *m) {
    u32 kick_mask = 0;
    arch.cpu_cli();
    runlock_lock();
    m->locked = 0;
    m->owner = 0xFFFFFFFFu;
    wake_chan_locked((u64)m, 1, &kick_mask);
    runlock_unlock();
    kick_idle_cpus(kick_mask);
    arch.cpu_sti();
}

void ksem_init(struct ksem *s, s32 count) {
    s->count = count;
}

void ksem_down(struct ksem *s) {
    arch.cpu_cli();
    runlock_lock();
    while (s->count <= 0) {
        u32 self = sched_cur();
        threads[self].wait_chan = (u64)s;
        (void)sched_desched_begin_locked(self, TCB_SLEEPING);
        runlock_unlock();
        sched_switch_to(pick_next(self));
        runlock_lock();
    }
    s->count = s->count - 1;
    runlock_unlock();
    arch.cpu_sti();
}

u32 ksem_trydown(struct ksem *s) {
    u32 got = 0;
    arch.cpu_cli();
    runlock_lock();
    if (s->count > 0) {
        s->count = s->count - 1;
        got = 1;
    }
    runlock_unlock();
    arch.cpu_sti();
    return got;
}

void ksem_up(struct ksem *s) {
    u32 kick_mask = 0;
    arch.cpu_cli();
    runlock_lock();
    s->count = s->count + 1;
    wake_chan_locked((u64)s, 1, &kick_mask);
    runlock_unlock();
    kick_idle_cpus(kick_mask);
    arch.cpu_sti();
}

void wait_queue_init(struct wait_queue_head *wq) {
    wq->pad = 0;
}

void wait_queue_sleep(struct wait_queue_head *wq) {
    sched_sleep((u64)wq);
}

void wait_queue_wake(struct wait_queue_head *wq) {
    sched_wakeup((u64)wq);
}

u32 wait_queue_wake_n(struct wait_queue_head *wq, u32 n) {
    return sched_wakeup_n((u64)wq, n);
}

void wait_queue_sleep_unlock_spin(struct wait_queue_head *wq, struct kmutex *m,
                                  volatile u32 *condition_lock) {
    u32 kick_mask = 0;
    u32 self;
    if (wq == 0 || m == 0) {
        return;
    }
    arch.cpu_cli();
    runlock_lock();
    self = sched_cur();
    m->locked = 0;
    m->owner = 0xFFFFFFFFu;
    wake_chan_locked((u64)m, 1, &kick_mask);
    threads[self].wait_chan = (u64)wq;
    (void)sched_desched_begin_locked(self, TCB_SLEEPING);
    if (condition_lock != 0) spin_unlock(condition_lock);
    runlock_unlock();
    kick_idle_cpus(kick_mask);
    sched_switch_to(pick_next(self));
    arch.cpu_sti();
    kmutex_lock(m);
}

void wait_queue_sleep_unlock(struct wait_queue_head *wq, struct kmutex *m) {
    wait_queue_sleep_unlock_spin(wq, m, 0);
}

/* Fast lockless check followed by locked expiration when any sleeper deadline
 * is due; runs BEFORE rq_tick_lock() so sched_run_lock -> rq_locks order holds. */
static void sched_expire_timers_tick(void) {
    u32 i;
    u32 any_due = 0;
    u64 now_ns = 0;
    u32 have_ns = 0;
    u32 kick_mask = 0;
    for (i = 0; i < MAX_THREADS; i++) {
        struct tcb *t = &threads[i];
        if (t->state != TCB_SLEEPING) continue;
        if (t->deadline_ns == 0 && t->deadline_ticks == 0) continue;
        if (t->deadline_ns != 0) {
            if (!have_ns) { now_ns = sched_monotonic_ns(); have_ns = 1; }
            if (now_ns >= t->deadline_ns) { any_due = 1; break; }
        } else if (clock_ticks >= t->deadline_ticks) {
            any_due = 1;
            break;
        }
    }
    if (!any_due) return;

    runlock_lock();
    have_ns = 0;
    for (i = 0; i < MAX_THREADS; i++) {
        struct tcb *t = &threads[i];
        u32 exp = 0;
        if (t->state != TCB_SLEEPING) continue;
        if (t->deadline_ns == 0 && t->deadline_ticks == 0) continue;
        if (t->deadline_ns != 0) {
            if (!have_ns) { now_ns = sched_monotonic_ns(); have_ns = 1; }
            if (now_ns >= t->deadline_ns) exp = 1;
        } else if (clock_ticks >= t->deadline_ticks) {
            exp = 1;
        }
        if (exp) {
            t->wait_chan = 0;
            t->deadline_ns = 0;
            t->deadline_ticks = 0;
            t->timed_out = 1;
            kick_mask = kick_mask | wake_task_to_rq_locked(i);
        }
    }
    runlock_unlock();
    if (kick_mask != 0) {
        kick_idle_cpus(kick_mask);
    }
}

static void rq_tick_lock(void) {
    rq_lock_acquire(sched_cpu_index());
}

/* Called from the Local APIC timer ISR (vector 32) with interrupts off.
 * Per-CPU sharded: takes ONLY this CPU's rq_lock (no global sched_run_lock!). */
void sched_tick(void) {
    arch.lapic_irq();                    /* EOI + raw tick counter bump */
    if (!active) {
        return;
    }
    if (sched_cpu_index() == sched_bsp_cpu) {
        clock_ticks++;
        sched_expire_timers_tick();
    }
    rq_tick_lock();
    sched_ticks++;
    cfs_charge(sched_cur(), VRUNTIME_SLICE);
    cfs_update_min();
    sched_switch_to(pick_next(sched_cur()));
}

/* Terminate the sched_cur() thread. Does not return. */
extern void native_mutex_owner_died_key(u64 key);
void kthread_exit(void) {
    native_mutex_owner_died_key(sched_current_owner_key());
    arch.cpu_cli();
    runlock_lock();
    {
        u32 self = sched_cur();
        u32 cpu = sched_desched_begin_locked(self, TCB_RETIRED);
        u32 next;
        runlock_unlock();
        next = pick_next(self);
        if (next == self) {
            rq_lock_release(cpu);
            for (;;) {
                arch.cpu_sti();
                arch.cpu_halt();
                arch.cpu_cli();
            }
        }
        threads[next].on_cpu = cpu;
        threads[next].rq_cpu = cpu;
        sched_set_cur(next);
        schedule_tail(self, next);
    }
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

/* int nice(int inc): fakeos nr 503 (Linux x86-64 has no nice; 89 is readlink). */
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

/* ---- CFS task_group public API (nested hierarchical fair share + SE rb) */

u32 sched_tg_create_under(u32 parent, u32 shares) {
    u32 g;
    u32 c;
    if (parent != TG_ROOT && !tg_valid(parent)) {
        return TG_NONE;
    }
    if (shares == 0) {
        shares = NICE0_WEIGHT;
    }
    for (g = 1; g < MAX_TASK_GROUPS; g++) {
        if (!task_groups[g].used) {
            task_groups[g].used = 1;
            task_groups[g].parent = parent;
            task_groups[g].shares = shares;
            for (c = 0; c < SCHED_MAX_CPUS; c++) {
                task_groups[g].vruntime[c] = 0;
                task_groups[g].nr[c] = 0;
                task_groups[g].child_se_root[c] = TG_NONE;
                tg_se_clear_node(g, c);
            }
            return g;
        }
    }
    return TG_NONE;
}

u32 sched_tg_create(u32 shares) {
    return sched_tg_create_under(TG_ROOT, shares);
}

u32 sched_tg_parent(u32 tg) {
    if (!tg_valid(tg)) {
        return TG_NONE;
    }
    return task_groups[tg].parent;
}

u32 sched_tg_set_shares(u32 tg, u32 shares) {
    if (!tg_valid(tg)) {
        return 0;
    }
    if (shares == 0) {
        shares = NICE0_WEIGHT;
    }
    task_groups[tg].shares = shares;
    return 1;
}

u32 sched_tg_shares(u32 tg) {
    if (!tg_valid(tg)) {
        return 0;
    }
    return task_groups[tg].shares;
}

u32 sched_tg_of(u32 id) {
    if (id >= MAX_THREADS || threads[id].state == TCB_FREE) {
        return TG_NONE;
    }
    return threads[id].tg;
}

/* Move a live thread into `tg`. Updates group nr accounting if on a RQ. */
u32 sched_tg_attach(u32 id, u32 tg) {
    u64 flags;
    if (id >= MAX_THREADS || threads[id].state == TCB_FREE
        || threads[id].state == TCB_DEAD || tcb_is_idle(id)) {
        return 0;
    }
    if (!tg_valid(tg)) {
        return 0;
    }
    flags = cpu_irq_save();
    runlock_lock();
    for (;;) {
        u32 c = threads[id].rq_cpu;
        if (c >= SCHED_MAX_CPUS) c = 0;
        rq_lock_acquire(c);
        if (threads[id].cfs_on_rq && threads[id].rq_cpu != c) {
            rq_lock_release(c);
            continue;
        }
        if (threads[id].tg != tg) {
            if (threads[id].cfs_on_rq) {
                cfs_dequeue_locked(id);
                threads[id].tg = tg;
                cfs_place_entity_cpu(c, id);
                if (threads[id].state == TCB_RUNNABLE) {
                    cfs_enqueue_locked(id);
                }
                cfs_update_min_cpu(c);
            } else {
                threads[id].tg = tg;
            }
        }
        rq_lock_release(c);
        break;
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

u64 sched_tick_count(void) {
    return sched_ticks;
}

/* Tick-resolution boot-relative elapsed time, not aggregate CPU runtime.
 * Aligned64-bit loads are atomic on x86-64. Timer IRQ exclusion can delay this
 * clock, as on UP; no RTC/TSC catch-up or suspend/hotplug claim is made. */
u64 sched_clock_ticks(void) {
    return clock_ticks;
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
    u64 flags=cpu_irq_save();u32 pid=0;
    runlock_lock();
    if(next_pid && next_pid!=0xFFFFFFFFu) {pid=next_pid;next_pid=next_pid+1;}
    runlock_unlock();cpu_irq_restore(flags);return pid;
}

u32 sched_current_pid(void) {
    return threads[sched_cur()].pid;
}

u32 sched_current_tid(void) {
    return threads[sched_cur()].tid;
}

/* Stable owner identity for native objects (architecture section 6: the
 * owner is NOT a PID or a recycled TCB index). User tasks pack
 * (pid << 32 | tid); tid comes from the never-recycling pid allocator, so a
 * dead owner can never be impersonated by slot reuse. Kernel threads (pid 0)
 * use their TCB slot — trusted test callers; their exit walk covers reuse. */
u64 sched_current_owner_key(void) {
    u32 cur = sched_cur();
    u32 pid = threads[cur].pid;
    if (pid == 0) return (u64)cur;
    return ((u64)pid << 32) | (u64)threads[cur].tid;
}

/* Live user threads that share `pid` (CLONE_THREAD group). */
u32 sched_count_live_pid(u32 pid) {
    u64 flags=cpu_irq_save();u32 n=0;struct process_owner *p;
    runlock_lock();p=process_find(pid);if(p)n=p->live;
    runlock_unlock();cpu_irq_restore(flags);return n;
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
    u64 flags = cpu_irq_save();
    runlock_lock();
    struct process_owner *p=process_find(threads[sched_cur()].pid);
    if(p)p->as_h=new_as_h;
    threads[sched_cur()].as_h = new_as_h;
    mem.as_activate_h(new_as_h);
    runlock_unlock();
    cpu_irq_restore(flags);
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
 * Process-shared file descriptors, working directory, environment & credentials
 * ------------------------------------------------------------------------- */

static u32 fh_pinned_by_peer_locked(u32 pid, u32 skip_tcb, u32 fh) {
    u32 i;
    if (pid == 0 || fh == FD_EMPTY || fh == FD_RESERVED) return 0;
    for (i = 0; i < MAX_THREADS; i++) {
        if (i == skip_tcb) continue;
        if (threads[i].state != TCB_FREE && threads[i].state != TCB_DEAD
            && threads[i].pid == pid && threads[i].pinned_fh == fh) {
            return 1;
        }
    }
    return 0;
}

u32 sched_fd_get(u32 idx) {
    u64 flags;
    u32 self;
    u32 fh;
    struct process_owner *p;
    if (idx >= FD_MAX) {
        return FD_EMPTY;
    }
    flags = cpu_irq_save();
    runlock_lock();
    self = sched_cur();
    p = process_or_kernel_locked(threads[self].pid);
    fh = p->fd[idx];
    if (fh == FD_RESERVED) {
        fh = FD_EMPTY;
    } else if (fh != FD_EMPTY && threads[self].is_user) {
        threads[self].pinned_fh = fh;
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    return fh;
}

void sched_fd_set(u32 idx, u32 fh) {
    u64 flags;
    struct process_owner *p;
    if (idx >= FD_MAX) return;
    flags = cpu_irq_save();
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    p->fd[idx] = fh;
    runlock_unlock();
    cpu_irq_restore(flags);
}

/* Lowest empty descriptor; atomically marks FD_RESERVED so concurrent open/
 * socket/pipe calls in sibling threads never pick the same fd number. */
u32 sched_fd_alloc(void) {
    u64 flags = cpu_irq_save();
    u32 i;
    u32 got = FD_EMPTY;
    struct process_owner *p;
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    for (i = 0; i < FD_MAX; i++) {
        if (p->fd[i] == FD_EMPTY) {
            p->fd[i] = FD_RESERVED;
            got = i;
            break;
        }
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    return got;
}

/* Drop caller's active syscall FD pin and flush any deferred closes that
 * were waiting for this thread's in-flight operation to finish. */
void sched_fd_unpin_self(void) {
    u64 flags;
    u32 self = sched_cur();
    u32 old;
    u32 pid;
    u32 to_close[FD_MAX];
    u32 n_close = 0;
    u32 i;
    struct process_owner *p;
    if (threads[self].pinned_fh == FD_EMPTY) return;
    flags = cpu_irq_save();
    runlock_lock();
    old = threads[self].pinned_fh;
    threads[self].pinned_fh = FD_EMPTY;
    pid = threads[self].pid;
    p = process_find(pid);
    if (p && p->ndeferred > 0 && !fh_pinned_by_peer_locked(pid, self, old)) {
        u32 kept = 0;
        for (i = 0; i < p->ndeferred; i++) {
            u32 dfh = p->deferred_close[i];
            if (dfh == old) {
                to_close[n_close] = dfh;
                n_close = n_close + 1;
            } else {
                p->deferred_close[kept] = dfh;
                kept = kept + 1;
            }
        }
        p->ndeferred = kept;
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    for (i = 0; i < n_close; i++) {
        sched_fh_close_backend(to_close[i]);
    }
}

/* Thread-safe close(fd): detaches slot immediately; defers backend close only
 * if a sibling thread in the same process is mid-syscall on that handle. */
u32 sched_fd_close_slot(u32 idx) {
    u64 flags;
    u32 self;
    u32 pid;
    u32 fh;
    u32 defer = 0;
    struct process_owner *p;
    if (idx >= FD_MAX) return 0;
    flags = cpu_irq_save();
    runlock_lock();
    self = sched_cur();
    threads[self].pinned_fh = FD_EMPTY;
    pid = threads[self].pid;
    p = process_or_kernel_locked(pid);
    fh = p->fd[idx];
    if (fh == FD_EMPTY || fh == FD_RESERVED) {
        runlock_unlock();
        cpu_irq_restore(flags);
        return 0;
    }
    p->fd[idx] = FD_EMPTY;
    if (fh_pinned_by_peer_locked(pid, self, fh) && p->ndeferred < FD_MAX) {
        p->deferred_close[p->ndeferred] = fh;
        p->ndeferred = p->ndeferred + 1;
        defer = 1;
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    if (!defer) {
        sched_fh_close_backend(fh);
    }
    return 1;
}

/* Thread-safe dup2 install onto `newfd` after caller retained `ofh`. */
void sched_fd_dup2_install(u32 newfd, u32 ofh) {
    u64 flags;
    u32 self;
    u32 pid;
    u32 nfh;
    u32 defer = 0;
    struct process_owner *p;
    if (newfd >= FD_MAX) return;
    flags = cpu_irq_save();
    runlock_lock();
    self = sched_cur();
    threads[self].pinned_fh = FD_EMPTY;
    pid = threads[self].pid;
    p = process_or_kernel_locked(pid);
    nfh = p->fd[newfd];
    p->fd[newfd] = ofh;
    if (nfh != FD_EMPTY && nfh != FD_RESERVED) {
        if (fh_pinned_by_peer_locked(pid, self, nfh) && p->ndeferred < FD_MAX) {
            p->deferred_close[p->ndeferred] = nfh;
            p->ndeferred = p->ndeferred + 1;
            defer = 1;
        }
    } else {
        nfh = FD_EMPTY;
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    if (!defer && nfh != FD_EMPTY) {
        sched_fh_close_backend(nfh);
    }
}

/* Final process exit: close all remaining descriptors and deferred closes. */
void sched_process_close_all_fds(u32 pid) {
    u64 flags;
    u32 to_close[FD_MAX * 2];
    u32 n_close = 0;
    u32 i;
    struct process_owner *p;
    if (pid == 0) return;
    flags = cpu_irq_save();
    runlock_lock();
    p = process_find(pid);
    if (p) {
        for (i = 0; i < FD_MAX; i++) {
            u32 fh = p->fd[i];
            p->fd[i] = FD_EMPTY;
            if (fh != FD_EMPTY && fh != FD_RESERVED) {
                to_close[n_close] = fh;
                n_close = n_close + 1;
            }
        }
        for (i = 0; i < p->ndeferred; i++) {
            u32 dfh = p->deferred_close[i];
            p->deferred_close[i] = FD_EMPTY;
            if (dfh != FD_EMPTY && dfh != FD_RESERVED) {
                to_close[n_close] = dfh;
                n_close = n_close + 1;
            }
        }
        p->ndeferred = 0;
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    for (i = 0; i < n_close; i++) {
        sched_fh_close_backend(to_close[i]);
    }
}

void sched_fd_close_all(void) {
    sched_process_close_all_fds(threads[sched_cur()].pid);
}

const char *sched_cwd(void) {
    u64 flags = cpu_irq_save();
    const char *res;
    struct process_owner *p;
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    res = p->cwd;
    runlock_unlock();
    cpu_irq_restore(flags);
    return res;
}

void sched_cwd_set(const char *kpath) {
    u64 flags = cpu_irq_save();
    struct process_owner *p;
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    proc_cwd_copy(p->cwd, kpath);
    runlock_unlock();
    cpu_irq_restore(flags);
}

u32 sched_umask_get(void) {
    u64 flags;
    u32 m;
    struct process_owner *p;
    if (!active) return kernel_owner_ensure()->umask & 0x1FF;
    flags = cpu_irq_save();
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    m = p->umask & 0x1FF;
    runlock_unlock();
    cpu_irq_restore(flags);
    return m;
}

u32 sched_umask_set(u32 mask) {
    u64 flags = cpu_irq_save();
    u32 old;
    struct process_owner *p;
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    old = p->umask & 0x1FF;
    p->umask = mask & 0x1FF;
    runlock_unlock();
    cpu_irq_restore(flags);
    return old;
}

u32 sched_current_uid(void) {
    u64 flags; u32 v; struct process_owner *p;
    if (!active) return kernel_owner_ensure()->uid;
    flags = cpu_irq_save(); runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid); v = p->uid;
    runlock_unlock(); cpu_irq_restore(flags); return v;
}

u32 sched_current_euid(void) {
    u64 flags; u32 v; struct process_owner *p;
    if (!active) return kernel_owner_ensure()->euid;
    flags = cpu_irq_save(); runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid); v = p->euid;
    runlock_unlock(); cpu_irq_restore(flags); return v;
}

u32 sched_current_gid(void) {
    u64 flags; u32 v; struct process_owner *p;
    if (!active) return kernel_owner_ensure()->gid;
    flags = cpu_irq_save(); runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid); v = p->gid;
    runlock_unlock(); cpu_irq_restore(flags); return v;
}

u32 sched_current_egid(void) {
    u64 flags; u32 v; struct process_owner *p;
    if (!active) return kernel_owner_ensure()->egid;
    flags = cpu_irq_save(); runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid); v = p->egid;
    runlock_unlock(); cpu_irq_restore(flags); return v;
}

void sched_set_uid(u32 uid) {
    u64 flags = cpu_irq_save(); struct process_owner *p;
    runlock_lock(); p = process_or_kernel_locked(threads[sched_cur()].pid); p->uid = uid;
    runlock_unlock(); cpu_irq_restore(flags);
}

void sched_set_euid(u32 euid) {
    u64 flags = cpu_irq_save(); struct process_owner *p;
    runlock_lock(); p = process_or_kernel_locked(threads[sched_cur()].pid); p->euid = euid;
    runlock_unlock(); cpu_irq_restore(flags);
}

void sched_set_gid(u32 gid) {
    u64 flags = cpu_irq_save(); struct process_owner *p;
    runlock_lock(); p = process_or_kernel_locked(threads[sched_cur()].pid); p->gid = gid;
    runlock_unlock(); cpu_irq_restore(flags);
}

void sched_set_egid(u32 egid) {
    u64 flags = cpu_irq_save(); struct process_owner *p;
    runlock_lock(); p = process_or_kernel_locked(threads[sched_cur()].pid); p->egid = egid;
    runlock_unlock(); cpu_irq_restore(flags);
}

u32 sched_current_ngroups(void) {
    u64 flags = cpu_irq_save(); u32 v; struct process_owner *p;
    runlock_lock(); p = process_or_kernel_locked(threads[sched_cur()].pid); v = p->ngroups;
    runlock_unlock(); cpu_irq_restore(flags); return v;
}

u32 sched_groups_get(u32 *dst, u32 n) {
    u64 flags = cpu_irq_save();
    u32 i;
    u32 ng;
    struct process_owner *p;
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    ng = p->ngroups;
    if (ng > 8) ng = 8;
    if (dst == 0) {
        runlock_unlock(); cpu_irq_restore(flags);
        return ng;
    }
    if (n < ng) {
        runlock_unlock(); cpu_irq_restore(flags);
        return 0xFFFFFFFF;
    }
    for (i = 0; i < ng; i++) {
        dst[i] = p->groups[i];
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    return ng;
}

u32 sched_groups_set(const u32 *src, u32 n) {
    u64 flags;
    u32 i;
    struct process_owner *p;
    if (n > 8) return 0;
    flags = cpu_irq_save();
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    for (i = 0; i < n; i++) {
        p->groups[i] = src[i];
    }
    p->ngroups = n;
    runlock_unlock();
    cpu_irq_restore(flags);
    return 1;
}

u32 sched_current_ppid(void) {
    return threads[sched_cur()].ppid;
}

static void cmdline_store_pid(u32 pid, const char *data, u64 n) {
    u64 flags;
    u64 i;
    struct process_owner *p;
    if (n > 127) n = 127;
    flags = cpu_irq_save();
    runlock_lock();
    p = process_or_kernel_locked(pid);
    for (i = 0; i < n; i++) p->cmdline[i] = data[i];
    p->cmdline[n] = 0;
    p->cmdline_len = (u32)n;
    runlock_unlock();
    cpu_irq_restore(flags);
}

void sched_set_cmdline(const char *data, u64 n) {
    cmdline_store_pid(threads[sched_cur()].pid, data, n);
}

void sched_set_cmdline_tcb(u32 tid, const char *data, u64 n) {
    if (tid >= MAX_THREADS) return;
    cmdline_store_pid(threads[tid].pid, data, n);
}

void sched_set_cmdline_argv(u32 argc, const char *argbuf) {
    char tmp[128];
    u64 at = 0;
    u32 i;
    for (i = 0; i < argc && at < 127; i++) {
        const char *s = argbuf + (u64)i * 128;
        u64 k = 0;
        while (s[k] != 0 && at < 127) {
            tmp[at++] = s[k++];
        }
        if (i + 1 < argc && at < 127) {
            tmp[at++] = 0;
        }
    }
    sched_set_cmdline(tmp, at);
}

u64 sched_cmdline_copy(char *buf, u64 cap) {
    u64 flags;
    u64 n;
    u64 i;
    struct process_owner *p;
    if (buf == 0 || cap == 0) return 0;
    flags = cpu_irq_save();
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    n = p->cmdline_len;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) buf[i] = p->cmdline[i];
    runlock_unlock();
    cpu_irq_restore(flags);
    return n;
}

static void environ_store_pid(u32 pid, const char *data, u64 n) {
    u64 flags;
    u64 i;
    struct process_owner *p;
    if (n > 255) n = 255;
    flags = cpu_irq_save();
    runlock_lock();
    p = process_or_kernel_locked(pid);
    for (i = 0; i < n; i++) p->environ[i] = data[i];
    p->environ[n] = 0;
    p->environ_len = (u32)n;
    runlock_unlock();
    cpu_irq_restore(flags);
}

void sched_set_environ(const char *data, u64 n) {
    environ_store_pid(threads[sched_cur()].pid, data, n);
}

void sched_set_environ_tcb(u32 tid, const char *data, u64 n) {
    if (tid >= MAX_THREADS) return;
    environ_store_pid(threads[tid].pid, data, n);
}

void sched_set_environ_default(void) {
    static const char defenv[] = {
        'F','A','K','E','C','C','_','P','K','G','=','/','s','r','c', 0,
        'F','A','K','E','C','C','_','R','T','C','A','C','H','E','=','0'
    };
    sched_set_environ(defenv, 32);
}

void sched_set_environ_tcb_default(u32 tid) {
    static const char defenv[] = {
        'F','A','K','E','C','C','_','P','K','G','=','/','s','r','c', 0,
        'F','A','K','E','C','C','_','R','T','C','A','C','H','E','=','0'
    };
    sched_set_environ_tcb(tid, defenv, 32);
}

void sched_set_environ_envp(u32 envc, const char *envbuf) {
    char tmp[256];
    u64 at = 0;
    u32 i;
    for (i = 0; i < envc && at < 255; i++) {
        const char *s = envbuf + (u64)i * 128;
        u64 k = 0;
        while (s[k] != 0 && at < 255) {
            tmp[at++] = s[k++];
        }
        if (i + 1 < envc && at < 255) {
            tmp[at++] = 0;
        }
    }
    sched_set_environ(tmp, at);
}

u64 sched_environ_copy(char *buf, u64 cap) {
    u64 flags;
    u64 n;
    u64 i;
    struct process_owner *p;
    if (buf == 0 || cap == 0) return 0;
    flags = cpu_irq_save();
    runlock_lock();
    p = process_or_kernel_locked(threads[sched_cur()].pid);
    n = p->environ_len;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) buf[i] = p->environ[i];
    runlock_unlock();
    cpu_irq_restore(flags);
    return n;
}

u32 sched_current_threads(void) { return sched_count_live_pid(threads[sched_cur()].pid); }

/* Visible in /proc and ps: a user process leader (tid==pid), including zombies
 * that have not yet been reaped. Kernel threads (pid 0) stay hidden. */
static u32 tcb_of_user_pid(u32 pid) {
    u32 i;
    u32 any = 0xFFFFFFFF;
    if (pid == 0) {
        return 0xFFFFFFFF;
    }
    for (i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == TCB_FREE || !threads[i].is_user
            || threads[i].as_h == 0 || threads[i].pid != pid) {
            continue;
        }
        if (threads[i].tid == pid) {
            return i;
        }
        if (any == 0xFFFFFFFF) {
            any = i;
        }
    }
    return any;
}

u32 sched_pid_visible(u32 pid) {
    u64 flags=cpu_irq_save();u32 yes;
    runlock_lock();yes=process_find(pid)!=0;
    runlock_unlock();cpu_irq_restore(flags);return yes;
}

u32 sched_pid_ppid(u32 pid) { return sched_process_parent(pid); }

u32 sched_pid_threads(u32 pid) { return sched_count_live_pid(pid); }

/* Do not export a borrowed AS pointer: exec/exit on another CPU can free it.
 * These bounded, non-allocating formatters finish under the same lock used by
 * sched_replace_as, so detach/swap precedes any destruction by the owner. */
u64 sched_pid_vma_kb(u32 pid) {
    u64 flags=cpu_irq_save();u64 n=0;struct process_owner *p;
    runlock_lock();p=process_find(pid);
    if(p && p->as_h)n=mem.as_vma_kb_h(p->as_h);
    runlock_unlock();cpu_irq_restore(flags);return n;
}
u64 sched_pid_maps(u32 pid,char *buf,u64 cap) {
    u64 flags=cpu_irq_save();u64 n=0;struct process_owner *p;
    runlock_lock();p=process_find(pid);
    if(p && p->as_h)n=mem.as_format_maps_h(p->as_h,buf,cap);
    runlock_unlock();cpu_irq_restore(flags);return n;
}

char sched_pid_state_char(u32 pid) {
    u64 flags=cpu_irq_save();u32 id;char state='?';struct process_owner *p;
    runlock_lock();p=process_find(pid);id=tcb_of_user_pid(pid);
    if(p && !p->live)state='Z';
    else if(id!=0xFFFFFFFF) {
        if(threads[id].state==TCB_RUNNABLE)state='R';
        else if(threads[id].state==TCB_SLEEPING)state='S';
        else if(threads[id].state==TCB_STOPPED)state='T';
    }
    runlock_unlock();cpu_irq_restore(flags);return state;
}

static u64 sched_cmdline_copy_pid_locked(u32 pid, char *buf, u64 cap) {
    struct process_owner *p;
    u64 n;
    u64 i;
    if (buf == 0 || cap == 0) return 0;
    p = (pid == 0) ? process_or_kernel_locked(threads[sched_cur()].pid)
                   : process_find(pid);
    if (p == 0) return 0;
    n = p->cmdline_len;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) buf[i] = p->cmdline[i];
    return n;
}
u64 sched_cmdline_copy_pid(u32 pid,char *buf,u64 cap) {
    u64 flags=cpu_irq_save();u64 n;
    runlock_lock();n=sched_cmdline_copy_pid_locked(pid,buf,cap);
    runlock_unlock();cpu_irq_restore(flags);return n;
}

static u64 sched_environ_copy_pid_locked(u32 pid, char *buf, u64 cap) {
    struct process_owner *p;
    u64 n;
    u64 i;
    if (buf == 0 || cap == 0) return 0;
    p = (pid == 0) ? process_or_kernel_locked(threads[sched_cur()].pid)
                   : process_find(pid);
    if (p == 0) return 0;
    n = p->environ_len;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) buf[i] = p->environ[i];
    return n;
}
u64 sched_environ_copy_pid(u32 pid,char *buf,u64 cap) {
    u64 flags=cpu_irq_save();u64 n;
    runlock_lock();n=sched_environ_copy_pid_locked(pid,buf,cap);
    runlock_unlock();cpu_irq_restore(flags);return n;
}

/* Sorted unique visible user pids. Returns the count written to out. */
u32 sched_pid_list(u32 *out,u32 cap) {
    u64 flags;u32 n=0,i,j;
    if(!out || !cap)return 0;
    flags=cpu_irq_save();runlock_lock();
    for(i=0;i<MAX_THREADS && n<cap;i++)if(processes[i].pid)out[n++]=processes[i].pid;
    runlock_unlock();cpu_irq_restore(flags);
    for(i=1;i<n;i++){u32 pid=out[i];j=i;while(j && out[j-1]>pid){out[j]=out[j-1];j--;}out[j]=pid;}
    return n;
}



/* True when a live (neither exited nor freed) child TCB of ppid exists. The
 * exit record alone is enough for wait once the child is DEAD. */
u32 sched_has_live_child(u32 ppid) {
    u64 flags=cpu_irq_save();u32 i,found=0;
    runlock_lock();
    for(i=0;i<MAX_THREADS;i++)if(processes[i].pid && processes[i].ppid==ppid){found=1;break;}
    runlock_unlock();cpu_irq_restore(flags);return found;
}
u32 sched_is_live_child(u32 ppid,u32 pid) {
    u64 flags=cpu_irq_save();u32 found=0;struct process_owner *p;
    runlock_lock();p=process_find(pid);if(p && p->ppid==ppid)found=1;
    runlock_unlock();cpu_irq_restore(flags);return found;
}
u32 sched_reap_dead(u32 pid) {
    u64 flags=cpu_irq_save();u32 ok=0;struct process_owner *p;
    runlock_lock();p=process_find(pid);
    if(p && p->drained && !p->live && !p->resident && !p->as_h){p->pid=0;ok=1;}
    runlock_unlock();cpu_irq_restore(flags);return ok;
}
void sched_reparent_children(u32 old_ppid,u32 new_ppid) {
    u64 flags=cpu_irq_save();u32 i;
    runlock_lock();
    for(i=0;i<MAX_THREADS;i++) {
        if(processes[i].pid && processes[i].ppid==old_ppid)processes[i].ppid=new_ppid;
        if(threads[i].pid && threads[i].ppid==old_ppid)threads[i].ppid=new_ppid;
    }
    runlock_unlock();cpu_irq_restore(flags);
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
        sched_bitset_or(&threads[sched_cur()].sig_ignore, (1u << sig));
    } else {
        sched_bitset_and(&threads[sched_cur()].sig_ignore, ~(1u << sig));
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

/* nanosleep/select/poll are never restarted by SA_RESTART. Keep the
 * restartable read/wait contract above unchanged. Delivery needs the actual
 * user return frame; this helper only observes pending signals. */
u32 sched_should_eintr_timer(void) {
    u32 pend;
    u32 sig;
    if (!threads[sched_cur()].is_user) return 0;
    if (sched_has_pending_fatal()) return 1;
    pend = threads[sched_cur()].pending_sigs & ~threads[sched_cur()].sig_ignore;
    for (sig = 1; sig <= 31; sig++) {
        if ((pend & (1u << sig)) && threads[sched_cur()].sig_handler[sig] > 1) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Interruptible park admission (Go gopark/sema discipline).
 *
 * The documented admission gap: sched_signal_send holds run_lock, sets a
 * pending fatal signal, and wakes tasks that are already SLEEPING — but a
 * task that is still RUNNABLE when the signal lands publishes SLEEPING
 * afterwards and strands forever. The fix serializes admission with the
 * signal/wake producers: the waiter registers its park intent (prepare)
 * while still on-CPU, and commit decides park-vs-refuse in ONE run_lock
 * transaction. Every producer (futex WAKE, select/poll readiness wakeups,
 * sched_signal_send) runs under the same run_lock, so it either precedes
 * the commit check (refusal or latch is visible) or observes the published
 * sleeper (normal wake). Producers latching PARK_NOTIFY on a preparing
 * waiter close the equivalent lost-wakeup window for predicates checked
 * outside a condition lock (futex user word, readiness scans) without any
 * VM dereference under run_lock.
 *
 * Kernel-internal sleeps (AS/file cleanup, kernel mutex/semaphore loops,
 * driver rendezvous) stay uninterruptible and never register. All early
 * returns from callers between prepare and commit must abort. */
void sched_park_prepare(u64 chan) {
    u64 flags;
    struct tcb *t;
    if (chan == 0) {
        return;
    }
    flags = cpu_irq_save();
    runlock_lock();
    t = &threads[sched_cur()];
    t->park_state = PARK_PREPARING;
    t->park_notify = 0;
    t->wait_chan = chan;
    t->deadline_ns = 0;
    t->deadline_ticks = 0;
    t->timed_out = 0;
    runlock_unlock();
    cpu_irq_restore(flags);
}

/* Prepare a park AND arm a monotonic ns deadline while caller already holds
 * an IRQ-off condition spinlock (lock order: condition -> run_lock).
 * abs_ns == 0 or ~0ULL means no deadline (infinite wait). */
void sched_park_prepare_deadline(u64 chan, u64 abs_ns) {
    struct tcb *t;
    if (chan == 0) {
        return;
    }
    runlock_lock();
    t = &threads[sched_cur()];
    t->park_state = PARK_PREPARING;
    t->park_notify = 0;
    t->wait_chan = chan;
    t->deadline_ns = (abs_ns == ~0ULL) ? 0 : abs_ns;
    t->deadline_ticks = 0;
    t->timed_out = 0;
    runlock_unlock();
}

void sched_timer_arm_ns(u64 abs_ns) {
    u64 flags = cpu_irq_save();
    struct tcb *t;
    runlock_lock();
    t = &threads[sched_cur()];
    t->deadline_ns = (abs_ns == ~0ULL) ? 0 : abs_ns;
    t->deadline_ticks = 0;
    t->timed_out = 0;
    runlock_unlock();
    cpu_irq_restore(flags);
}

void sched_timer_arm_ticks(u64 abs_ticks) {
    u64 flags = cpu_irq_save();
    struct tcb *t;
    runlock_lock();
    t = &threads[sched_cur()];
    t->deadline_ticks = abs_ticks;
    t->deadline_ns = 0;
    t->timed_out = 0;
    runlock_unlock();
    cpu_irq_restore(flags);
}

/* Park the current task on its own private timer channel until abs_ns (when
 * nonzero) or abs_ticks, using nonrestartable EINTR policy. */
u32 sched_sleep_deadline(u64 abs_ticks, u64 abs_ns) {
    u64 flags = cpu_irq_save();
    u64 chan;
    struct tcb *t;
    runlock_lock();
    t = &threads[sched_cur()];
    chan = (u64)&t->timer_chan;
    t->park_state = PARK_PREPARING;
    t->park_notify = 0;
    t->wait_chan = chan;
    t->deadline_ns = abs_ns;
    t->deadline_ticks = abs_ns != 0 ? 0 : abs_ticks;
    t->timed_out = 0;
    runlock_unlock();
    cpu_irq_restore(flags);
    return sched_park_commit(chan, 1);
}

/* Drop a prepared registration: any early return between prepare and
 * commit, and the trusted probe rendezvous gate before its own park. */
void sched_park_abort(void) {
    u64 flags;
    struct tcb *t;
    flags = cpu_irq_save();
    runlock_lock();
    t = &threads[sched_cur()];
    t->park_state = PARK_NONE;
    t->park_notify = 0;
    t->wait_chan = 0;
    t->deadline_ns = 0;
    t->deadline_ticks = 0;
    t->timed_out = 0;
    runlock_unlock();
    cpu_irq_restore(flags);
}

/* Commit a prepared park. Returns PARK_PARKED (slept and was woken),
 * PARK_WOKEN (a producer latched the notification while this task was
 * preparing/on-CPU — it never slept), PARK_INTERRUPTED (fatal or caught
 * nonrestartable signal pending — it never slept), or PARK_TIMED_OUT
 * (per-task deadline expired before wakeup). */
u32 sched_park_commit(u64 chan, u32 nonrestartable) {
    u32 self;
    u32 to;
    struct tcb *t;
    if (chan == 0) {
        return PARK_PARKED;
    }
    arch.cpu_cli();
    runlock_lock();
    self = sched_cur();
    t = &threads[self];
    if (t->park_notify != 0) {
        t->park_state = PARK_NONE;
        t->park_notify = 0;
        t->wait_chan = 0;
        t->deadline_ns = 0;
        t->deadline_ticks = 0;
        t->timed_out = 0;
        runlock_unlock();
        arch.cpu_sti();
        return PARK_WOKEN;
    }
    if (t->is_user != 0
        && (t->task_cancelled != 0
            || (nonrestartable != 0 ? sched_should_eintr_timer()
                                    : sched_should_eintr()) != 0)) {
        t->park_state = PARK_NONE;
        t->park_notify = 0;
        t->wait_chan = 0;
        t->deadline_ns = 0;
        t->deadline_ticks = 0;
        t->timed_out = 0;
        runlock_unlock();
        arch.cpu_sti();
        return PARK_INTERRUPTED;
    }
    if ((t->deadline_ns != 0 && sched_monotonic_ns() >= t->deadline_ns)
        || (t->deadline_ticks != 0 && clock_ticks >= t->deadline_ticks)) {
        t->park_state = PARK_NONE;
        t->park_notify = 0;
        t->wait_chan = 0;
        t->deadline_ns = 0;
        t->deadline_ticks = 0;
        t->timed_out = 0;
        runlock_unlock();
        arch.cpu_sti();
        return PARK_TIMED_OUT;
    }
    /* Publish the actual sleep in the same run_lock transaction, then drop
     * sched_run_lock BEFORE switching stacks under per-CPU rq_locks[cpu]. */
    t->wait_chan = chan;
    t->timed_out = 0;
    t->park_state = PARK_NONE;
    (void)sched_desched_begin_locked(self, TCB_SLEEPING);
    runlock_unlock();
    sched_switch_to(pick_next(self));
    /* Woken: state is RUNNABLE and wait_chan cleared by wakeup/timer. */
    if (threads[self].task_cancelled != 0) {
        threads[self].timed_out = 0;
        arch.cpu_sti();
        return PARK_INTERRUPTED;
    }
    to = threads[self].timed_out;
    threads[self].timed_out = 0;
    arch.cpu_sti();
    return to != 0 ? (u32)PARK_TIMED_OUT : (u32)PARK_PARKED;
}

u32 sched_current_task_cancelled(void) {
    u32 self = sched_cur();
    return threads[self].is_user && threads[self].task_cancelled != 0;
}

/* Cancel a single user task by tid (architecture section 3/5: task_cap CANCEL
 * stops ONLY the targeted task without killing sibling tasks in the process). */
u32 sched_cancel_task_tid(u32 tid) {
    u64 flags;
    u32 i;
    u32 found = 0;
    u32 kick_mask = 0;
    if (tid == 0) return 0;
    flags = cpu_irq_save();
    runlock_lock();
    for (i = 0; i < MAX_THREADS; i++) {
        struct tcb *t = &threads[i];
        if (t->is_user && t->tid == tid && t->state != TCB_FREE
            && t->state != TCB_DEAD && t->state != TCB_RETIRED) {
            t->task_cancelled = 1;
            found = 1;
            if (t->state == TCB_SLEEPING || t->state == TCB_STOPPED) {
                t->wait_chan = 0;
                t->deadline_ns = 0;
                t->deadline_ticks = 0;
                t->timed_out = 0;
                kick_mask = kick_mask | wake_task_to_rq_locked(i);
            } else if (t->on_cpu != SCHED_CPU_NONE
                       && t->on_cpu != sched_cpu_index()) {
                u32 apic = smp_cpu_apic_id(t->on_cpu);
                if (apic != 0xFFFFFFFFu) {
                    lapic_send_ipi(apic, (u32)SCHED_IPI_VECTOR);
                    sched_ipi_resched = sched_ipi_resched + 1;
                }
            }
            break;
        }
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    if (kick_mask != 0) {
        kick_idle_cpus(kick_mask);
    }
    return found;
}

u32 sched_signal_send(u32 pid, u32 sig) {
    u32 i;
    u32 found = 0;
    u64 flags;
    u32 wake_parents[MAX_THREADS];
    u32 n_wake = 0;
    u32 kick_mask = 0;
    if (sig > 31) {
        return 0;
    }
    flags = cpu_irq_save();
    runlock_lock();
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
                    threads[i].wait_chan = 0;
                    threads[i].cont_notify = 1;
                    kick_mask = kick_mask | wake_task_to_rq_locked(i);
                    if (threads[i].ppid != 0 && n_wake < MAX_THREADS) {
                        wake_parents[n_wake] = threads[i].ppid;
                        n_wake = n_wake + 1;
                    }
                }
                sched_bitset_and(&threads[i].pending_sigs, ~((1u << SIGSTOP_N) | (1u << SIGTSTP_N)
                                             | (1u << SIGTTIN_N) | (1u << SIGTTOU_N)));
            }
            sched_bitset_or(&threads[i].pending_sigs, (1u << sig));
            /* Wake sleepers so they can observe stop/kill/cont. */
            if (threads[i].state == TCB_SLEEPING) {
                threads[i].wait_chan = 0;
                threads[i].deadline_ns = 0;
                threads[i].deadline_ticks = 0;
                threads[i].timed_out = 0;
                kick_mask = kick_mask | wake_task_to_rq_locked(i);
            }
            /* KILL must not leave a process stuck STOPPED. */
            if (sig == SIGKILL_N && threads[i].state == TCB_STOPPED) {
                kick_mask = kick_mask | wake_task_to_rq_locked(i);
            }
            /* Running on a remote CPU: IPI so stop/handler/kill is prompt. */
            if (sig != 0 && threads[i].on_cpu != SCHED_CPU_NONE
                && threads[i].on_cpu != sched_cpu_index()) {
                u32 apic = smp_cpu_apic_id(threads[i].on_cpu);
                if (apic != 0xFFFFFFFFu) {
                    lapic_send_ipi(apic, (u32)SCHED_IPI_VECTOR);
                    sched_ipi_resched = sched_ipi_resched + 1;
                }
            } else if (sig == SIGCONT_N && threads[i].state == TCB_RUNNABLE) {
                u32 cpu = threads[i].rq_cpu;
                if (cpu < SCHED_MAX_CPUS && rq_cpu_is_idle(cpu)) {
                    kick_mask = kick_mask | (1u << cpu);
                }
            }
        }
    }
    runlock_unlock();
    cpu_irq_restore(flags);
    if (kick_mask != 0) {
        kick_idle_cpus(kick_mask);
    }
    for (i = 0; i < n_wake; i++) {
        sched_wakeup(sched_wait4_chan(wake_parents[i]));
    }
    return found;
}

/* Stop the sched_cur() process and switch away until SIGCONT. */
void sched_stop_self(u32 sig) {
    u32 self;
    u32 ppid;
    u32 kick_mask = 0;
    arch.cpu_cli();
    runlock_lock();
    self = sched_cur();
    ppid = threads[self].ppid;
    threads[self].wait_chan = 0;
    threads[self].stop_sig = sig;
    threads[self].stop_notify = 1;
    sched_bitset_and(&threads[self].pending_sigs, ~((1u << SIGSTOP_N) | (1u << SIGTSTP_N)
                                       | (1u << SIGTTIN_N) | (1u << SIGTTOU_N)));
    if (ppid != 0) {
        wake_chan_locked(sched_wait4_chan(ppid), MAX_THREADS, &kick_mask);
    }
    (void)sched_desched_begin_locked(self, TCB_STOPPED);
    runlock_unlock();
    kick_idle_cpus(kick_mask);
    sched_switch_to(pick_next(self));
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
    sched_bitset_and(&threads[sched_cur()].pending_sigs, ~(1u << sig));
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
    sched_bitset_and(&threads[sched_cur()].pending_sigs, ~ign);
    pend = threads[sched_cur()].pending_sigs;

    /* Prefer custom handlers; default INT/TERM terminate; STOP/TSTP stop. */
    for (sig = 1; sig <= 31; sig++) {
        u64 h;
        if ((pend & (1u << sig)) == 0) continue;
        if (sig == SIGKILL_N) continue;
        h = threads[sched_cur()].sig_handler[sig];
        /* A kernel wait has no user frame to redirect. Preserve a caught
         * signal for syscall exit, never substitute its default action or
         * silently drop it. In particular caught TERM must not kill sleepers. */
        if (h > 1 && f_rip == 0) continue;

        /* SIGSTOP cannot be caught or ignored. */
        if (sig == SIGSTOP_N) {
            sched_bitset_and(&threads[sched_cur()].pending_sigs, ~(1u << sig));
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
                sched_bitset_and(&threads[sched_cur()].pending_sigs, ~(1u << sig));
                pend = threads[sched_cur()].pending_sigs;
                continue;
            }
            sched_bitset_and(&threads[sched_cur()].pending_sigs, ~(1u << sig));
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
            sched_bitset_and(&threads[sched_cur()].pending_sigs, ~(1u << sig));
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
        sched_bitset_and(&threads[sched_cur()].pending_sigs, ~(1u << sig));
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
/* ---------------------------------------------------------------------------
 * TLB shootdown sender (hook installed by kmain as mem.as_shootdown_hook).
 *
 * One outstanding request, serialised by sd_lock: the sender writes the
 * target AS handle + range into the mem-owned request block, sends a
 * vector-35 IPI to every CPU whose current address space matches, and
 * spins until each target's ack counter catches up with the generation.
 * Targets that were not running the space (or that switched away before
 * the IPI landed) acknowledge without flushing — their CR3 reload already
 * flushed everything. The sender's own CPU flushes locally before sending.
 *
 * Lock-order note: sd_lock is leaf-level (handler never takes it; the
 * handler only reads the request block, flushes and acks).
 * ------------------------------------------------------------------------- */
static volatile u32 sd_lock;

u64 sched_tlb_shootdown_hook(u64 handle, u64 va_start, u64 va_end) {
    u32 cpu = smp_cpuid();
    u32 ncpu = smp_cpu_count();
    u32 i;
    u32 seqs[8];
    u64 va;
    u64 irq;

    /* Local flush first: the mutator's address space is current here. */
    for (va = va_start; va < va_end; va += 4096) {
        mem.invlpg_page(va);
    }
    if (ncpu <= 1) {
        return 1;
    }
    for (i = 0; i < 8; i++) seqs[i] = 0;

    /* Push into each target's mailbox and send its IPI. Waiting happens
     * AFTER all pushes, with IRQs enabled and NO lock held — a target that
     * is itself mid-shootdown drains and acknowledges independently, so
     * concurrent forks cannot deadlock. */
    irq = cpu_irq_save();
    for (i = 0; i < ncpu && i < 8; i++) {
        u32 apic;
        u32 slot;
        if (i == cpu) continue;
        if (mem.as_current_h[i] != handle) continue;
        apic = smp_cpu_apic_id(i);
        if (apic == 0xFFFFFFFFu) continue;
        spin_lock(&mem.as_sd_qlock[i]);
        while (mem.as_sd_qhead[i] - mem.as_sd_qtail[i] >= 16) {
            /* Mailbox full: target drains at its next IPI window. Release
             * briefly so its handler can run. */
            spin_unlock(&mem.as_sd_qlock[i]);
            cpu_irq_restore(irq);
            io_wait();
            irq = cpu_irq_save();
            spin_lock(&mem.as_sd_qlock[i]);
        }
        slot = i * 16u + (mem.as_sd_qhead[i] % 16u);
        mem.as_sd_handle[slot] = handle;
        mem.as_sd_start[slot] = va_start;
        mem.as_sd_end[slot] = va_end;
        mem.as_sd_seq[i] = mem.as_sd_seq[i] + 1;
        mem.as_sd_eseq[slot] = mem.as_sd_seq[i];
        seqs[i] = mem.as_sd_seq[i];
        mem.as_sd_qhead[i] = mem.as_sd_qhead[i] + 1;
        spin_unlock(&mem.as_sd_qlock[i]);
        lapic_send_ipi(apic, (u32)TLB_IPI_VECTOR);
    }
    cpu_irq_restore(irq);
    for (i = 0; i < ncpu && i < 8; i++) {
        u32 spins = 0;
        if (seqs[i] == 0) continue;
        while (mem.as_sd_done[i] < seqs[i]) {
            io_wait();
            spins = spins + 1;
            if ((spins & 0x7FFFFFu) == 0) {
                /* Bounded visibility: a target that never services its IPI
                 * would otherwise freeze the whole machine silently. */
                kprintf("[SDT] timeout cpu=%u target=%u done=%u seq=%u cur=%u mine=%u\n",
                        (u64)cpu, (u64)i, (u64)mem.as_sd_done[i],
                        (u64)seqs[i], (u64)mem.as_current_h[i], (u64)handle);
            }
        }
    }
    return 1;
}

void sched_resched_from_ipi(void) {
    u32 cpu;
    if (!active) {
        return;
    }
    cpu = sched_cpu_index();
    rq_lock_acquire(cpu);
    if (idle_mark[sched_cur()] == 0) {
        rq_lock_release(cpu);
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
    /* SYSCALL/SYSRET MSRs are per-CPU. uproc_selftest() programs them only
     * on the BSP; without a matching AP setup, a user thread that is stolen
     * or finishes a pre-emption-migrated syscall executes sysretq with
     * STAR=0 while RSP is already the user stack, which double-faults. */
    syscall_init();
    /* EFER.NXE is also per-CPU. Existing user stack/data PTEs carry NX;
     * without this, a perfectly present NX leaf faults with RSVD on the AP. */
    mem.vmm_init();
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
        rq_lock_acquire(cpu);
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
        threads[slot].timer_chan = (u64)&threads[slot].timer_chan;
        threads[slot].deadline_ns = 0;
        threads[slot].deadline_ticks = 0;
        threads[slot].timed_out = 0;
        threads[slot].fs_base = 0;
        threads[slot].clear_child_tid = 0;
        threads[slot].pinned_fh = FD_EMPTY;
        threads[slot].task_cancelled = 0;
        threads[slot].pid = 0;
        threads[slot].tid = 0;
        threads[slot].ppid = 0;
        threads[slot].is_user = 0;
        threads[slot].pending_sigs = 0;
        threads[slot].sig_ignore = 0;
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

/* Trusted boot-only rendezvous. No syscall or user-supplied arm operation.
 * A targeted child executes its REAL futex path; synchronous injection is at
 * the exact post-predicate/pre-publication boundary. The driver records and
 * rescues a stranded sleeper as FAILURE, never as accepted progress. */
static u32 wp_pid,wp_mode,wp_fired,wp_cpu_mask,wp_driver_mask,wp_release;
u64 sched_wait_probe_scan(u32 what);
static u64 wp_main_chan,wp_peer_chan;
static volatile u32 wp_gate_lock;
void sched_wait_probe_arm(u32 pid,u32 mode) {
    u64 flags=cpu_irq_save();runlock_lock();
    wp_pid=pid;wp_mode=mode;wp_fired=0;wp_cpu_mask=0;wp_driver_mask=0;wp_release=0;wp_main_chan=0;wp_peer_chan=0;
    runlock_unlock();cpu_irq_restore(flags);
}
void sched_wait_probe_point(u64 chan) {
    u64 flags=cpu_irq_save();u32 sig=0,pid=0,self,hold=0,wake=0;
    runlock_lock();self=sched_cur();
    if(wp_pid && threads[self].pid==wp_pid) {
        wp_cpu_mask=wp_cpu_mask | (1u<<sched_cpu_index());
        if(threads[self].tid==wp_pid)wp_main_chan=chan;else wp_peer_chan=chan;
        if(!wp_fired && ((wp_mode==6 && threads[self].tid!=wp_pid)
           || (wp_mode!=6 && threads[self].tid==wp_pid))) {
            wp_fired=1;pid=wp_pid;
            if(wp_mode==0)sig=9;
            if(wp_mode==6)hold=1;
            if(wp_mode==3 || wp_mode==4)sig=15;
            if(wp_mode==7)wake=1;
        }
    }
    runlock_unlock();cpu_irq_restore(flags);
    if(sig)sched_signal_send(pid,sig);
    if(wake){*(volatile u32 *)chan=1;sched_wakeup(chan);}
    if(hold) {
        /* The futex park intent never committed: drop the registration so
         * the uninterruptible gate park below is the only claimed chan. */
        sched_park_abort();
        flags=cpu_irq_save();spin_lock(&wp_gate_lock);
        while(!wp_release) {
            sched_sleep_unlock_spin((u64)&wp_release,&wp_gate_lock);
            spin_lock(&wp_gate_lock);
        }
        spin_unlock(&wp_gate_lock);cpu_irq_restore(flags);
    }
}
u64 sched_wait_probe_scan(u32 what) {
    u64 flags=cpu_irq_save(),n=0;u32 i;
    runlock_lock();
    if(what==9)n=wp_release;
    else if(what==0)n=wp_fired;
    else if(what==3)n=wp_main_chan;
    else if(what==4)n=wp_peer_chan;
    else if(what==7)n=wp_cpu_mask | wp_driver_mask;
    else if(what==10)n=wp_cpu_mask;
    else if(what==11)n=wp_driver_mask;
    else for(i=0;i<MAX_THREADS;i++)if(threads[i].pid==wp_pid && threads[i].state==TCB_SLEEPING) {
        if(what==1)n=n+1;
        if(what==2 && (threads[i].wait_chan==wp_main_chan || threads[i].wait_chan==wp_peer_chan) && (threads[i].pending_sigs & ((1u<<9)|(1u<<15))))n=n+1;
        if(what==5 && threads[i].tid==wp_pid)n=1;
    }
    runlock_unlock();cpu_irq_restore(flags);return n;
}
void sched_wait_probe_rescue(void) {
    u64 flags=cpu_irq_save();u32 i,kicks=0;
    runlock_lock();
    for(i=0;i<MAX_THREADS;i++)if(threads[i].pid==wp_pid && threads[i].state==TCB_SLEEPING)
        wake_chan_locked(threads[i].wait_chan,MAX_THREADS,&kicks);
    runlock_unlock();cpu_irq_restore(flags);kick_idle_cpus(kicks);
}

void sched_wait_probe_release(void) {
    u64 flags=cpu_irq_save();spin_lock(&wp_gate_lock);wp_release=1;
    sched_wakeup((u64)&wp_release);spin_unlock(&wp_gate_lock);cpu_irq_restore(flags);
}

void sched_wait_probe_note_driver(void) {
    u64 flags=cpu_irq_save();runlock_lock();
    wp_driver_mask=wp_driver_mask | (1u<<sched_cpu_index());
    runlock_unlock();cpu_irq_restore(flags);
}
