package kernel;
import arch;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

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

void sched_selftest(void) {
    kprintf("\n[SCHED] Kernel Threads & Round-Robin Scheduler Online:\n");
    kprintf("      Threads      : %u TCB slots, 16 KiB buddy-backed stacks\n", (u64)32);
    kprintf("      Switch       : callee-saved RSP context, timer + yield driven\n");
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
    arch.cpu_cli();
}
