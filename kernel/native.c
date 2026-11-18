package kernel;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t s32;

/* Kernel-private native object foundation. NOT user capabilities: callers are
 * trusted kernel code. User exposure requires process-owned capability tables
 * and cancellation registration; see docs/native-kernel-architecture.md. */
enum {
    NC_SEND = 1, NC_RECV = 2, NC_CHANNEL = 1,
    NC_BAD_HANDLE = -1, NC_RIGHTS = -2, NC_FULL = -3,
    NC_AGAIN = -4, NC_CLOSED = -5, NC_EOF = 1, NC_INVAL = -6, NC_TYPE = -7,
    NC_OBJECTS = 8, NC_HANDLES = 32, NC_CAPACITY = 8
};
struct native_object {
    u32 kind;
    u32 readers;
    u32 writers;
    u32 pins;
    u32 head;
    u32 count;
    u32 recv_wait;
    u32 send_wait;
    u64 messages[NC_CAPACITY];
};
struct native_handle {
    u64 generation;
    u32 live;
    u32 object;
    u32 rights;
};
static struct native_object nc_objects[NC_OBJECTS];
static struct native_handle nc_handles[NC_HANDLES];
static volatile u32 nc_lock;
extern void spin_lock(volatile u32 *lock);
extern void spin_unlock(volatile u32 *lock);
extern u64 cpu_irq_save(void);
extern void cpu_irq_restore(u64 flags);
extern void sched_sleep_unlock_spin(u64 chan, volatile u32 *lock);
extern void sched_wakeup(u64 chan);

/* All helpers require nc_lock, IRQs disabled. Never wrap a generation: an
 * exhausted handle slot is retired, not allowed to resurrect an old ID. */
static s32 nc_lookup(u64 h) {
    u32 i = (u32)(h & 255ULL) - 1;
    if (i >= NC_HANDLES || !nc_handles[i].live
        || nc_handles[i].generation != (h >> 8)) return NC_BAD_HANDLE;
    return (s32)i;
}
static s32 nc_alloc_handle(u32 oi, u32 rights, u64 *out) {
    u32 i;
    struct native_object *o = &nc_objects[oi];
    for (i = 0; i < NC_HANDLES; i++) {
        if (!nc_handles[i].live
            && nc_handles[i].generation < 0x00FFFFFFFFFFFFFFULL) {
            struct native_handle *h = &nc_handles[i];
            h->generation++;
            h->live = 1;
            h->object = oi;
            h->rights = rights;
            /* Explicit pointer-member loads/stores: the current fakecc seed
             * miscompiles indexed-struct-member postincrement into storing the
             * array index. Actual guest refcount/exhaustion tests guard this. */
            if (rights & NC_SEND) o->writers = o->writers + 1;
            if (rights & NC_RECV) o->readers = o->readers + 1;
            *out = (h->generation << 8) | (u64)(i + 1);
            return 0;
        }
    }
    return NC_FULL;
}
static void nc_reclaim(struct native_object *o) {
    if (!o->readers && !o->writers && !o->pins) o->kind = 0;
}
static void nc_close_locked(u32 hi) {
    struct native_handle *h = &nc_handles[hi];
    struct native_object *o = &nc_objects[h->object];
    if (h->rights & NC_SEND) o->writers--;
    if (h->rights & NC_RECV) o->readers--;
    h->live = 0;
    /* Also wakes operations on THIS now-revoked handle, even when other
     * endpoints still exist. Pins keep their condition addresses alive. */
    sched_wakeup((u64)&o->recv_wait);
    sched_wakeup((u64)&o->send_wait);
    nc_reclaim(o);
}

s32 native_channel_create(u64 *send, u64 *recv) {
    u64 flags;
    u32 oi;
    s32 rc = NC_FULL;
    if (send == 0 || recv == 0 || send == recv) return NC_INVAL;
    *send = 0;
    *recv = 0;
    flags = cpu_irq_save();
    spin_lock(&nc_lock);
    for (oi = 0; oi < NC_OBJECTS; oi++) {
        struct native_object *o = &nc_objects[oi];
        if (o->kind) continue;
        o->kind = NC_CHANNEL;
        o->readers = 0; o->writers = 0; o->pins = 0;
        o->head = 0; o->count = 0;
        rc = nc_alloc_handle(oi, NC_SEND, send);
        if (rc == 0) {
            rc = nc_alloc_handle(oi, NC_RECV, recv);
            if (rc != 0) nc_close_locked((u32)nc_lookup(*send));
        }
        if (rc != 0) { *send = 0; *recv = 0; nc_reclaim(o); }
        break;
    }
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return rc;
}

s32 native_handle_dup(u64 handle, u32 rights, u64 *out) {
    u64 flags;
    s32 hi;
    s32 rc;
    if (out == 0 || !rights || (rights & ~(u32)(NC_SEND | NC_RECV))) return NC_INVAL;
    *out = 0;
    flags = cpu_irq_save();
    spin_lock(&nc_lock);
    hi = nc_lookup(handle);
    rc = hi;
    if (hi >= 0) {
        if ((nc_handles[hi].rights & rights) != rights) rc = NC_RIGHTS;
        else rc = nc_alloc_handle(nc_handles[hi].object, rights, out);
    }
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return rc;
}
s32 native_handle_close(u64 handle) {
    u64 flags = cpu_irq_save();
    s32 hi;
    spin_lock(&nc_lock);
    hi = nc_lookup(handle);
    if (hi >= 0) nc_close_locked((u32)hi);
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return hi < 0 ? hi : 0;
}

/* Message values are opaque copied kernel scalars, never followed as pointers.
 * This API performs no user access and must NOT be wired directly to syscalls.
 * Linearization is under nc_lock. Sleep publication takes run_lock BEFORE
 * releasing nc_lock. Wake may be spurious; every predicate and ID is rechecked.
 * Closing a blocked operation's handle revokes it with BAD_HANDLE. */
static s32 nc_transfer(u64 handle, u64 *value, u32 send, u32 block) {
    u64 flags;
    s32 hi;
    s32 rc;
    u32 oi;
    struct native_object *o;
    if (value == 0 || block > 1) return NC_INVAL;
    flags = cpu_irq_save();
    spin_lock(&nc_lock);
    hi = nc_lookup(handle);
    if (hi < 0) { rc = hi; goto done; }
    if (!(nc_handles[hi].rights & (send ? NC_SEND : NC_RECV))) {
        rc = NC_RIGHTS; goto done;
    }
    oi = nc_handles[hi].object;
    o = &nc_objects[oi];
    if (o->kind != NC_CHANNEL) { rc = NC_TYPE; goto done; }
    o->pins++;
    for (;;) {
        hi = nc_lookup(handle);
        if (hi < 0) { rc = NC_BAD_HANDLE; break; }
        if (send) {
            if (!o->readers) { rc = NC_CLOSED; break; }
            if (o->count < NC_CAPACITY) {
                o->messages[(o->head + o->count) % NC_CAPACITY] = *value;
                o->count++;
                sched_wakeup((u64)&o->recv_wait);
                rc = 0; break;
            }
        } else {
            if (o->count) {
                *value = o->messages[o->head];
                o->head = (o->head + 1) % NC_CAPACITY;
                o->count--;
                sched_wakeup((u64)&o->send_wait);
                rc = 0; break;
            }
            if (!o->writers) { rc = NC_EOF; break; }
        }
        if (!block) { rc = NC_AGAIN; break; }
        sched_sleep_unlock_spin(send ? (u64)&o->send_wait : (u64)&o->recv_wait, &nc_lock);
        spin_lock(&nc_lock);
    }
    o->pins--;
    nc_reclaim(o);
 done:
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return rc;
}
s32 native_channel_send(u64 handle, u64 value, u32 block) {
    return nc_transfer(handle, &value, 1, block);
}
s32 native_channel_recv(u64 handle, u64 *value, u32 block) {
    return nc_transfer(handle, value, 0, block);
}

/* Boot regression: private objects, no sockets, pipes, VFS or NIC required. */
extern void kprintf(const char *fmt, ...);
extern u32 kthread_create(u64 entry, u64 arg);
extern void kthread_exit(void);
extern void sched_yield(void);
extern u64 sched_clock_ticks(void);
extern u32 sched_thread_state(u32 id);
extern u32 smp_cpuid(void);
extern u32 smp_cpu_count(void);
extern u32 sched_cfs_rb_check(void);
static volatile u32 nc_test_lock;
static u64 nc_test_handles[4];
static u32 nc_test_seen[512];
static u32 nc_test_done;
static u32 nc_test_errors;
static u32 nc_test_mask;
static u32 nc_test_count;
static s32 nc_test_revoked;
static u32 nc_test_ready;
static u32 nc_test_go;
static u32 nc_test_send_hits[8];
static u32 nc_test_recv_hits[8];
/* All four workers must exist and remain runnable before payload release.
 * This is a scheduling barrier, not a timing delay or CPU-affinity override.
 * The same bounded yielding protocol works on one CPU. */
static u32 nc_start_barrier(void) {
    u64 until = sched_clock_ticks() + 800;
    u64 flags = cpu_irq_save();
    spin_lock(&nc_test_lock);
    nc_test_ready++;
    if (nc_test_ready == 4) nc_test_go = 1;
    spin_unlock(&nc_test_lock);
    cpu_irq_restore(flags);
    for (;;) {
        u32 go;
        flags = cpu_irq_save();
        spin_lock(&nc_test_lock);
        go = nc_test_go;
        spin_unlock(&nc_test_lock);
        cpu_irq_restore(flags);
        if (go) return go == 1;
        if (sched_clock_ticks() >= until) return 0;
        sched_yield();
    }
}
static void nc_sent(void) {
    u64 flags = cpu_irq_save();
    u32 cpu = smp_cpuid();
    spin_lock(&nc_test_lock);
    nc_test_mask |= 1u << cpu;
    if (cpu < 8) nc_test_send_hits[cpu]++;
    spin_unlock(&nc_test_lock);
    cpu_irq_restore(flags);
}
static void nc_record(u64 value, u32 done, u32 error) {
    u64 flags = cpu_irq_save();
    u32 cpu = smp_cpuid();
    spin_lock(&nc_test_lock);
    if (value) {
        nc_test_mask |= 1u << cpu;
        if (cpu < 8) nc_test_recv_hits[cpu]++;
        if (value > 512 || nc_test_seen[value - 1]) nc_test_errors++;
        else { nc_test_seen[value - 1] = 1; nc_test_count++; }
    }
    nc_test_done += done;
    nc_test_errors += error;
    spin_unlock(&nc_test_lock);
    cpu_irq_restore(flags);
}
static void nc_worker(u64 arg) {
    u32 id = (u32)arg;
    u32 i;
    u32 error = 0;
    if (!nc_start_barrier()) { error = 1; goto done; }
    if (id < 2) {
        for (i = 0; i < 256; i++) {
            if (native_channel_send(nc_test_handles[id], (u64)(id * 256 + i + 1), 1) != 0) {
                error = 1; break;
            }
            nc_sent();
            if ((i & 7) == 0) sched_yield();
        }
    } else {
        u64 value;
        s32 rc;
        for (;;) {
            rc = native_channel_recv(nc_test_handles[id], &value, 1);
            if (rc == NC_EOF) break;
            if (rc != 0) { error = 1; break; }
            nc_record(value, 0, 0);
        }
    }
 done:
    if (native_handle_close(nc_test_handles[id]) != 0) error = 1;
    nc_record(0, 1, error);
    kthread_exit();
}
static void nc_revoked_worker(u64 arg) {
    u64 value;
    nc_test_revoked = native_channel_recv(arg, &value, 1);
    nc_record(0, 1, 0);
    kthread_exit();
}
static u32 nc_wait_done(u32 n) {
    u64 until = sched_clock_ticks() + 800;
    for (;;) {
        u64 flags = cpu_irq_save();
        u32 done;
        spin_lock(&nc_test_lock);
        done = nc_test_done;
        spin_unlock(&nc_test_lock);
        cpu_irq_restore(flags);
        if (done >= n) return 1;
        if (sched_clock_ticks() >= until) return 0;
        sched_yield();
    }
}
static u32 nc_empty(void) {
    u64 flags = cpu_irq_save();
    u32 i;
    u32 empty = 1;
    spin_lock(&nc_lock);
    for (i = 0; i < NC_OBJECTS; i++) if (nc_objects[i].kind) empty = 0;
    for (i = 0; i < NC_HANDLES; i++) if (nc_handles[i].live) empty = 0;
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return empty;
}
static u32 nc_expect(u32 condition, u32 tag) {
    if (!condition) kprintf("[NATIVE] expectation %u failed\n", (u64)tag);
    return condition;
}
void native_object_selftest(void) {
    u64 tx, rx, dup, value, old;
    u64 handles[NC_HANDLES];
    u32 n, i, wid, created;
    u32 ready;
    u64 flags;
    u32 ok = 1;
    u64 until;
    kprintf("\n[NATIVE] Kernel-only typed channel/object self-test...\n");
    if (native_channel_create(&tx, &rx) != 0) goto fail;
    old = rx;
    ok &= nc_expect((native_channel_send(rx, 1, 0) == NC_RIGHTS), 1);
    ok &= nc_expect((native_channel_recv(tx, &value, 0) == NC_RIGHTS), 2);
    ok &= nc_expect((native_handle_dup(tx, NC_RECV, &dup) == NC_RIGHTS), 3);
    ok &= nc_expect((native_channel_recv(rx, &value, 0) == NC_AGAIN), 4);
    for (i = 0; i < NC_CAPACITY; i++) ok &= nc_expect((native_channel_send(tx, i + 10, 0) == 0), 5);
    ok &= nc_expect((native_channel_send(tx, 99, 0) == NC_AGAIN), 6);
    ok &= nc_expect((native_handle_close(tx) == 0), 7);
    for (i = 0; i < NC_CAPACITY; i++) {
        ok &= nc_expect((native_channel_recv(rx, &value, 0) == 0), 8);
        ok &= nc_expect((value == i + 10), 9);
    }
    ok &= nc_expect((native_channel_recv(rx, &value, 0) == NC_EOF), 10);
    ok &= nc_expect((native_handle_close(rx) == 0), 11);
    ok &= nc_expect((native_handle_close(rx) == NC_BAD_HANDLE), 12);
    if (native_channel_create(&tx, &rx) != 0) goto fail;
    ok &= nc_expect((native_channel_recv(old, &value, 0) == NC_BAD_HANDLE), 13);
    ok &= nc_expect((native_handle_close(rx) == 0), 14);
    ok &= nc_expect((native_channel_send(tx, 1, 0) == NC_CLOSED), 15);
    ok &= nc_expect((native_handle_close(tx) == 0), 16);
    /* Exhaust objects independently of handles, then reuse every slot. */
    for (i = 0; i < NC_OBJECTS; i++)
        ok &= nc_expect((native_channel_create(&handles[2 * i], &handles[2 * i + 1]) == 0), 17);
    ok &= nc_expect((native_channel_create(&dup, &value) == NC_FULL), 18);
    for (i = 0; i < 2 * NC_OBJECTS; i++) ok &= nc_expect((native_handle_close(handles[i]) == 0), 19);
    ok &= nc_expect((native_handle_close(0) == NC_BAD_HANDLE), 20);
    ok &= nc_expect((native_channel_create(&tx, &tx) == NC_INVAL), 21);
    ok &= nc_expect((native_channel_recv(old, 0, 0) == NC_INVAL), 22);
    /* Exhaust handles with duplicates; failed pair allocation must roll back
     * its first handle, and the object must not leak. Leave exactly one free. */
    if (native_channel_create(&tx, &rx) != 0) goto fail;
    n = 0;
    while (n < NC_HANDLES - 3 && native_handle_dup(tx, NC_SEND, &handles[n]) == 0) n++;
    ok &= nc_expect((n == NC_HANDLES - 3), 23);
    ok &= nc_expect((native_channel_create(&dup, &value) == NC_FULL), 24);
    for (i = 0; i < n; i++) ok &= nc_expect((native_handle_close(handles[i]) == 0), 25);
    ok &= nc_expect((native_handle_close(tx) == 0), 26);
    ok &= nc_expect((native_handle_close(rx) == 0), 27);
    ok &= nc_expect((nc_empty()), 28);
    if (!ok) goto fail;
    kprintf("      [PASS] native handles: rights/stale/FIFO/full/EOF/peer-close/rollback/no-leak\n");

    /* Deterministically revoke an already sleeping receive; the last-close
     * path must wake it and keep the object pinned until it has returned. */
    if (native_channel_create(&tx, &rx) != 0) goto fail;
    nc_test_done = 0; nc_test_revoked = 0;
    wid = kthread_create((u64)&nc_revoked_worker, rx);
    if (wid == 0xFFFFFFFFu) goto fail;
    until = sched_clock_ticks() + 800;
    while (sched_thread_state(wid) != 3 && sched_clock_ticks() < until) sched_yield();
    ok &= nc_expect((sched_thread_state(wid) == 3), 29); /* TCB_SLEEPING */
    ok &= nc_expect((native_handle_close(rx) == 0), 30);
    ok &= nc_expect((native_handle_close(tx) == 0), 31);
    ok &= nc_expect((nc_wait_done(1)), 32);
    ok &= nc_expect((nc_test_revoked == NC_BAD_HANDLE), 33);
    ok &= nc_expect((nc_empty()), 34);
    if (!ok) goto fail;
    kprintf("      [PASS] native close: blocked operation revoked, pin drained, object reclaimed\n");

    ok &= nc_expect((sched_cfs_rb_check()), 40);
    if (!ok) goto fail;
    if (native_channel_create(&tx, &rx) != 0) goto fail;
    nc_test_handles[0] = tx; nc_test_handles[2] = rx;
    if (native_handle_dup(tx, NC_SEND, &nc_test_handles[1]) != 0) goto fail;
    if (native_handle_dup(rx, NC_RECV, &nc_test_handles[3]) != 0) goto fail;
    nc_test_done = 0; nc_test_errors = 0; nc_test_count = 0; nc_test_mask = 0;
    for (i = 0; i < 512; i++) nc_test_seen[i] = 0;
    nc_test_ready = 0; nc_test_go = 0;
    for (i = 0; i < 8; i++) { nc_test_send_hits[i] = 0; nc_test_recv_hits[i] = 0; }
    /* kmain currently runs as the BSP idle TCB. It cannot act as a barrier
     * controller while runnable workers exist: pick_next correctly prefers
     * real work over idle. Publish the complete batch before local preemption;
     * the LAST WORKER (not idle kmain) releases the barrier. */
    flags = cpu_irq_save();
    created = 0;
    for (i = 0; i < 4; i++) {
        if (kthread_create((u64)&nc_worker, i) == 0xFFFFFFFFu) break;
        created++;
    }
    if (created != 4) {
        spin_lock(&nc_test_lock);
        nc_test_go = 2;
        spin_unlock(&nc_test_lock);
    }
    cpu_irq_restore(flags);
    for (i = created; i < 4; i++) native_handle_close(nc_test_handles[i]);
    ok &= nc_expect((created == 4), 41);
    if (!ok) { nc_wait_done(created); goto fail; }
    ok &= nc_expect((nc_wait_done(4)), 35);
    flags = cpu_irq_save();
    spin_lock(&nc_test_lock);
    ready = nc_test_ready;
    spin_unlock(&nc_test_lock);
    cpu_irq_restore(flags);
    ok &= nc_expect((ready == 4), 43);
    kprintf("      native barrier: %u/4 workers runnable before payload release\n", (u64)ready);
    ok &= nc_expect((nc_test_errors == 0 && nc_test_count == 512), 36);
    for (i = 0; i < 512; i++) ok &= nc_expect((nc_test_seen[i] == 1), 37);
    ok &= nc_expect((nc_empty()), 38);
    ok &= nc_expect((sched_cfs_rb_check()), 42);
    if (smp_cpu_count() > 1) ok &= nc_expect(((nc_test_mask & (nc_test_mask - 1)) != 0), 39);
    for (i = 0; i < smp_cpu_count() && i < 8; i++)
        kprintf("      native CPU %u: send=%u recv=%u\n", (u64)i,
                (u64)nc_test_send_hits[i], (u64)nc_test_recv_hits[i]);
    if (!ok) goto fail;
    kprintf("      [PASS] native channel MPMC: 512 exact-once messages, CPUs=%u mask=0x%x, no leaks\n",
            (u64)smp_cpu_count(), (u64)nc_test_mask);
    kprintf("[NATIVE] PASS kernel-only object/channel suite\n");
    return;
 fail:
    kprintf("[NATIVE] FAIL kernel-only object/channel suite (done=%u errors=%u count=%u mask=0x%x)\n",
            (u64)nc_test_done, (u64)nc_test_errors, (u64)nc_test_count, (u64)nc_test_mask);
}
