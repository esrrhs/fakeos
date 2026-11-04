package kernel;
import arch;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t s32;

extern void kprintf(const char *fmt, ...);

extern void spin_lock(volatile u32 *lock);
extern void spin_unlock(volatile u32 *lock);
extern u64 cpu_irq_save(void);
extern void cpu_irq_restore(u64 flags);

extern u32 kthread_create(u64 entry, u64 arg);
extern void sched_yield(void);
extern void kthread_exit(void);
extern void sched_init(void);
extern u64 sched_tick_count(void);
extern u32 sched_thread_state(u32 id);
extern u32 sched_set_nice(u32 id, s32 nice);
extern u32 sched_current_id(void);
extern u32 smp_cpuid(void);
extern u32 smp_cpu_count(void);
extern u64 sched_steal_total(void);
extern u64 sched_steal_on_cpu(u32 cpu);
extern u64 sched_steal_kernel_total(void);

enum {
    LOCK_WORKERS = 4,
    WORK_EACH    = 200,
    SPINNERS     = 3
};

static volatile u32 g_lock;
static volatile u64 g_shared;
static volatile u64 g_completed;
static volatile u64 g_spin_counts[SPINNERS];
static volatile u64 g_spin_done;
static volatile u64 g_spin_deadline;

/* Worker threads: under a spinlock, each adds WORK_EACH to the shared counter
 * and explicitly yields after every critical section. */
static void lock_worker(u64 id) {
    u64 i;
    for (i = 0; i < WORK_EACH; i++) {
        spin_lock(&g_lock);
        g_shared = g_shared + 1;
        spin_unlock(&g_lock);
        sched_yield();
    }
    spin_lock(&g_lock);
    g_completed = g_completed + 1;
    spin_unlock(&g_lock);
    kthread_exit();
}

/* Preemption spinners: never yield. They run until a tick deadline, proving
 * the APIC timer preemptively migrates the CPU across all three threads. */
static void spinner(u64 id) {
    u64 n = 0;
    while (sched_tick_count() < g_spin_deadline) {
        n++;
    }
    g_spin_counts[id] = n;
    g_spin_done = g_spin_done + 1;
    kthread_exit();
}

static int test_lock_workers(void) {
    u32 ids[LOCK_WORKERS];
    u64 i;
    g_lock = 0;
    g_shared = 0;
    g_completed = 0;

    for (i = 0; i < LOCK_WORKERS; i++) {
        ids[i] = kthread_create((u64)(&lock_worker), i);
        if (ids[i] == 0xFFFFFFFF) {
            kprintf("      [FAIL] could not create lock worker %u\n", i);
            return 0;
        }
    }

    /* Cooperative workers finish almost instantly: slot 0 is idle-only and
     * each yield rotates straight to the next worker. Spin here (still
     * receiving preemptive ticks) until all complete or a tick deadline. */
    u64 deadline = sched_tick_count() + 500;
    while (g_completed != LOCK_WORKERS && sched_tick_count() < deadline) {
        sched_yield();
    }

    if (g_completed != LOCK_WORKERS) {
        kprintf("      [FAIL] only %u of %u lock workers completed\n",
                g_completed, (u64)LOCK_WORKERS);
        return 0;
    }
    if (g_shared != (u64)LOCK_WORKERS * WORK_EACH) {
        kprintf("      [FAIL] locked counter %u, expected %u (lost update)\n",
                g_shared, (u64)LOCK_WORKERS * WORK_EACH);
        return 0;
    }
    for (i = 0; i < LOCK_WORKERS; i++) {
        if (sched_thread_state(ids[i]) != 2) {
            kprintf("      [FAIL] worker %u not marked dead\n", i);
            return 0;
        }
    }
    return 1;
}

static int test_preemptive_spinners(void) {
    u64 i;
    g_spin_done = 0;
    for (i = 0; i < SPINNERS; i++) {
        g_spin_counts[i] = 0;
    }
    /* Deadline ~300 ms of ticks beyond the current tick. */
    g_spin_deadline = sched_tick_count() + 30;

    for (i = 0; i < SPINNERS; i++) {
        if (kthread_create((u64)(&spinner), i) == 0xFFFFFFFF) {
            kprintf("      [FAIL] could not create spinner %u\n", i);
            return 0;
        }
    }

    /* Spinners terminate when the tick deadline passes; idle-yield while the
     * preemptive timer round-robins across them. */
    u64 spin_deadline = sched_tick_count() + 300;
    while (g_spin_done != SPINNERS && sched_tick_count() < spin_deadline) {
        sched_yield();
    }

    if (g_spin_done != SPINNERS) {
        kprintf("      [FAIL] only %u of %u never-yielding spinners finished\n",
                g_spin_done, (u64)SPINNERS);
        return 0;
    }
    for (i = 0; i < SPINNERS; i++) {
        if (g_spin_counts[i] == 0) {
            kprintf("      [FAIL] spinner %u never got scheduled (no preemption)\n", i);
            return 0;
        }
    }
    return 1;
}


/* Weighted fair: one nice-0 spinner and one nice+10 spinner. Over a shared
 * tick window the nicer thread must accumulate fewer busy-loop iterations
 * (CFS charges more vruntime per tick at lower weight). */
static volatile u64 g_fair_counts[2];
static volatile u64 g_fair_done;
static volatile u64 g_fair_deadline;

static void fair_spinner(u64 id) {
    u64 n = 0;
    while (sched_tick_count() < g_fair_deadline) {
        n++;
    }
    g_fair_counts[id] = n;
    g_fair_done = g_fair_done + 1;
    kthread_exit();
}

static int test_weighted_fair(void) {
    u32 id0;
    u32 id1;
    u64 hi;
    u64 lo;
    g_fair_done = 0;
    g_fair_counts[0] = 0;
    g_fair_counts[1] = 0;
    g_fair_deadline = sched_tick_count() + 40;

    id0 = kthread_create((u64)(&fair_spinner), 0);
    id1 = kthread_create((u64)(&fair_spinner), 1);
    if (id0 == 0xFFFFFFFF || id1 == 0xFFFFFFFF) {
        kprintf("      [FAIL] could not create fair spinners\n");
        return 0;
    }
    if (!sched_set_nice(id1, 10)) {
        kprintf("      [FAIL] sched_set_nice(+10) failed\n");
        return 0;
    }

    {
        u64 deadline = sched_tick_count() + 400;
        while (g_fair_done != 2 && sched_tick_count() < deadline) {
            sched_yield();
        }
    }
    if (g_fair_done != 2) {
        kprintf("      [FAIL] fair spinners did not finish (%u)\n", g_fair_done);
        return 0;
    }
    if (g_fair_counts[0] == 0 || g_fair_counts[1] == 0) {
        kprintf("      [FAIL] fair spinner got zero work (0=%u 1=%u)\n",
                g_fair_counts[0], g_fair_counts[1]);
        return 0;
    }
    /* nice 0 weight 1024 vs nice+10 weight ~110 => expect ~9:1 share.
     * Accept a loose bound: nice0 must beat nice10 by at least 2x. */
    hi = g_fair_counts[0];
    lo = g_fair_counts[1];
    if (hi < lo * 2) {
        kprintf("      [FAIL] weighted fair ratio too weak: nice0=%u nice+10=%u\n",
                hi, lo);
        return 0;
    }
    return 1;
}

void sched_selftest(void) {
    kprintf("\n[SCHED] Kernel Threads & CFS-lite Weighted Fair Scheduler Online:\n");
    kprintf("      Threads      : %u TCB slots, 16 KiB buddy-backed stacks\n", (u64)32);
    kprintf("      Switch       : callee-saved RSP context, min-vruntime + nice weights\n");
    kprintf("      Spinlock     : xchg TAS, IRQ save/restore primitives\n");

    sched_init();

    kprintf("\n[SCHED] Running Scheduler Self-Tests...\n");
    /* Interrupts were left disabled after the APIC calibration block; the
     * trampoline and idle paths manage sti/cli, and ticks drive switches. */
    arch.cpu_sti();
    if (test_lock_workers()) {
        kprintf("      [PASS] Cooperative threads: %u x %u locked increments, exact total, clean exit\n",
                (u64)LOCK_WORKERS, (u64)WORK_EACH);
    }
    if (test_preemptive_spinners()) {
        kprintf("      [PASS] Timer preemption: %u never-yielding threads all scheduled\n",
                (u64)SPINNERS);
    }
    if (test_weighted_fair()) {
        kprintf("      [PASS] CFS-lite: nice0 spinner got >=2x work of nice+10\n");
    }
    arch.cpu_cli();
}


/* Cross-CPU steal smoke: after APs are in the scheduler, flood the BSP
 * runqueue with kernel workers and require the AP to steal at least one. */
enum { STEAL_WORKERS = 6 };

static volatile u32 g_steal_done;
static volatile u32 g_steal_saw_ap;
static volatile u64 g_steal_cpu_hits[8];

static void steal_worker(u64 id) {
    u32 c = smp_cpuid();
    (void)id;
    if (c < 8) {
        g_steal_cpu_hits[c] = g_steal_cpu_hits[c] + 1;
    }
    if (c != 0) {
        g_steal_saw_ap = 1;
    }
    /* Brief burn so an AP tick can observe runnable peers still queued. */
    {
        u64 n = 0;
        u64 i;
        for (i = 0; i < 200000; i++) {
            n = n + i;
        }
        (void)n;
    }
    g_steal_done = g_steal_done + 1;
    kthread_exit();
}

void sched_steal_selftest(void) {
    u32 i;
    u64 steals0;
    u64 steals1;
    u32 ncpu = smp_cpu_count();

    kprintf("\n[SCHED] Cross-CPU runqueue steal self-test...\n");
    if (ncpu < 2) {
        kprintf("      [PASS] Cross-CPU steal: skipped (-smp 1)\n");
        return;
    }

    g_steal_done = 0;
    g_steal_saw_ap = 0;
    for (i = 0; i < 8; i++) {
        g_steal_cpu_hits[i] = 0;
    }
    steals0 = sched_steal_total();

    arch.cpu_sti();
    for (i = 0; i < STEAL_WORKERS; i++) {
        if (kthread_create((u64)(&steal_worker), i) == 0xFFFFFFFF) {
            kprintf("      [FAIL] steal worker create %u\n", (u64)i);
            arch.cpu_cli();
            return;
        }
    }

    {
        u64 deadline = sched_tick_count() + 800;
        while (g_steal_done != STEAL_WORKERS && sched_tick_count() < deadline) {
            sched_yield();
        }
    }
    /* Extra settle for AP tick accounting. */
    arch.pit_busy_wait_ms(50);
    steals1 = sched_steal_total();
    arch.cpu_cli();

    if (g_steal_done != STEAL_WORKERS) {
        kprintf("      [FAIL] steal workers done %u/%u\n",
                (u64)g_steal_done, (u64)STEAL_WORKERS);
        return;
    }
    if (g_steal_saw_ap == 0 && sched_steal_on_cpu(1) == 0
        && (steals1 - steals0) == 0) {
        kprintf("      [FAIL] no AP steal observed (hits0=%u hits1=%u steals=%u)\n",
                g_steal_cpu_hits[0], g_steal_cpu_hits[1], steals1 - steals0);
        return;
    }
    kprintf("      [PASS] Cross-CPU steal: kernel workers on AP (steals=%u kernel=%u hits0=%u hits1=%u)\n",
            steals1 - steals0, sched_steal_kernel_total(),
            g_steal_cpu_hits[0], g_steal_cpu_hits[1]);
}
