package kernel;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t s32;

/* Kernel-private native object foundation. NOT user capabilities: callers are
 * trusted kernel code. User exposure requires process-owned capability tables
 * and cancellation registration; see docs/native-kernel-architecture.md. */
enum {
    NC_SEND = 1, NC_RECV = 2, NC_CHANNEL = 1, NC_MUTEX = 2,
    NC_MLOCK = 4, NC_MUNLOCK = 8, NC_MWAIT = 16,
    NC_BAD_HANDLE = -1, NC_RIGHTS = -2, NC_FULL = -3,
    NC_AGAIN = -4, NC_CLOSED = -5, NC_EOF = 1, NC_INVAL = -6, NC_TYPE = -7,
    NC_INTR = -8,
    NC_DEADLK = -9, NC_OWNER_DIED = -10, NC_NOT_RECOVERABLE = -11,
    NC_OBJECTS = 8, NC_HANDLES = 32, NC_CAPACITY = 8,
    /* Mutex states. M_DIED: owner died, next acquisition recovers with
     * OWNER_DIED. M_NOTREC: recovery was abandoned; terminal poison. */
    M_FREE = 0, M_LOCKED = 1, M_DIED = 2, M_NOTREC = 3,
    MQ_CAP = 8
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
    /* NC_MUTEX fields (parallel with messages; m_wait is the wake channel) */
    u32 m_state;
    u32 m_recover;
    u64 m_owner;
    u32 m_wait;
    u32 m_waiters;
    u32 m_qhead;
    /* Monotonic count of enqueue events. m_waiters counts queue SLOTS and
     * stays occupied by the holder until it unlocks, so it cannot tell a
     * test "how many contenders are actually blocked on this mutex"; this
     * counter can, and it only ever moves forward. */
    u32 m_enqueues;
    /* Per-waiter wait channel. The contenders must NOT share one channel:
     * sched_wakeup() wakes EVERY sleeper on the channel, so a shared
     * channel would wake the losers of a handoff too. They would re-enqueue
     * at a slot computed from an already-advanced m_qhead/m_waiters and so
     * lose their FIFO place. One channel per waiter lets the unlock wake
     * exactly the contender it handed the mutex to.
     *
     * The queue stores SLOT INDICES, not owner keys: a waiter parks on
     * m_chan[slot], and the slot is released when the waiter leaves. A
     * slot's channel address therefore never moves, which is what makes
     * the wakeup target exact. */
    u64 m_chan[MQ_CAP];
    u64 m_queue[MQ_CAP];
    /* Owner key currently parked on m_chan[slot]; 0 means the slot is free.
     * A slot is allocated on enqueue and released on dequeue, so the channel
     * a waiter parks on never moves and a wakeup always targets exactly the
     * intended contender. */
    u64 m_tkey[MQ_CAP];
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
extern void sched_park_abort(void);
extern void sched_park_commit(u64 chan, u32 nonrestartable);
extern void sched_wakeup(u64 chan);
extern void sched_park_prepare(u64 chan);
extern u32 sched_park_commit_unlock_spin(u64 chan, volatile u32 *lock, u32 nonrestartable);
extern u64 sched_current_owner_key(void);
enum { PARK_INTR_RESULT = 2 };              /* matches sched.c PARK_INTERRUPTED */

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
            /* A mutex capability is one object reference, whatever its rights. */
            if (rights & (NC_MLOCK | NC_MUNLOCK | NC_MWAIT))
                o->readers = o->readers + 1;
            *out = (h->generation << 8) | (u64)(i + 1);
            return 0;
        }
    }
    return NC_FULL;
}
static void nc_reclaim(struct native_object *o) {
    if (o->readers || o->writers || o->pins) return;
    /* Last-cap close while LOCKED does not destroy the mutex: the owner
     * reference survives until unlock/death (architecture section 6). */
    if (o->kind == NC_MUTEX && o->m_state == M_LOCKED) return;
    o->kind = 0;
}
static void nc_close_locked(u32 hi) {
    struct native_handle *h = &nc_handles[hi];
    struct native_object *o = &nc_objects[h->object];
    if (h->rights & NC_SEND) o->writers--;
    if (h->rights & NC_RECV) o->readers--;
    if (h->rights & (NC_MLOCK | NC_MUNLOCK | NC_MWAIT)) o->readers--;
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
    if (out == 0 || !rights
        || (rights & ~(u32)(NC_SEND | NC_RECV | NC_MLOCK | NC_MUNLOCK | NC_MWAIT)))
        return NC_INVAL;
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
 * Linearization is under nc_lock. Sleep publication takes run_lock BEFORE
 * releasing nc_lock. Wake may be spurious; every predicate and ID is rechecked.
 * Closing a blocked operation's handle revokes it with BAD_HANDLE.
 *
 * DELIBERATE 4-ARGUMENT LIMIT: the current fakecc miscompiles 5-argument
 * calls — the 5th integer argument arrives with garbage register contents
 * (a boolean flag read as 0xfffffff442d31e80 in R8), which silently routed
 * kernel callers into the user park path; see
 * docs/evidence/native-caps-20261121/RESULT.md. The interruptible twin below
 * duplicates the transfer loop ON PURPOSE; only the park step differs. */
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

/* Interruptible twin for USER capability syscalls (kernel/natcap.c): the
 * park intent is registered while nc_lock is still held, and commit
 * publishes sleep in one run_lock transaction that also refuses park on
 * pending fatal/caught cancellation (PARK_INTERRUPTED -> NC_INTR). Peer
 * enqueue/close hold nc_lock, signals hold run_lock, so every producer
 * either precedes the commit check or observes the published sleeper. */
static s32 nc_transfer_intr(u64 handle, u64 *value, u32 send, u32 block) {
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
        {
            u64 wchan = send ? (u64)&o->send_wait : (u64)&o->recv_wait;
            sched_park_prepare(wchan);
            if (sched_park_commit_unlock_spin(wchan, &nc_lock, 0)
                == PARK_INTR_RESULT) {
                rc = NC_INTR;
                break;
            }
        }
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
/* User capability syscalls go through these: identical semantics plus the
 * interruptible park-admission contract (see kernel/natcap.c). */
s32 native_channel_send_intr(u64 handle, u64 value, u32 block) {
    return nc_transfer_intr(handle, &value, 1, block);
}
s32 native_channel_recv_intr(u64 handle, u64 *value, u32 block) {
    return nc_transfer_intr(handle, value, 0, block);
}

/* ---------------------------------------------------------------------------
 * Native robust mutex (architecture section 6, gate 3).
 *
 * Typed kernel object independent of any shared-memory location. The owner is
 * a stable task identity from sched_current_owner_key(): user tasks are
 * (pid << 32 | tid) where tid comes from the never-recycling pid allocator,
 * so a dead owner can never be impersonated by slot reuse; kernel threads use
 * their TCB slot and are trusted test callers. Nonrecursive: self-lock ->
 * NC_DEADLK, nonowner unlock -> NC_RIGHTS (EPERM).
 *
 * Handoff prevents barging: unlock with queued waiters transfers ownership to
 * the FIFO head AT UNLOCK TIME (state stays M_LOCKED under the new owner), so
 * a newcomer arriving before the woken waiter resumes sees the lock held and
 * queues behind it. The wait queue is bounded (MQ_CAP); overflow -> NC_AGAIN.
 *
 * Robustness: a dying owner's exit walk marks M_DIED and wakes waiters; the
 * next acquirer gets NC_OWNER_DIED and must call consistent() before a normal
 * unlock, otherwise that unlock poisons the mutex as M_NOTREC (terminal).
 * Kill-while-handed (cancellation lands after a handoff woke us): the task
 * never returned to user code, so the protected data is untouched and the
 * lock releases CLEANLY (M_FREE), not through the recovery path.
 *
 * Locking: everything under nc_lock, IRQs off; parks follow the
 * prepare/commit contract exactly like nc_transfer_intr (nc_lock -> run_lock,
 * never inverted). Waiters pin the object; reclaim is blocked while M_LOCKED
 * with no handles left (owner reference survives close-until-death).
 * ------------------------------------------------------------------------- */
s32 native_mutex_create(u64 *out) {
    u64 flags;
    u32 oi;
    u32 n;
    s32 rc = NC_FULL;
    if (out == 0) return NC_INVAL;
    *out = 0;
    flags = cpu_irq_save();
    spin_lock(&nc_lock);
    for (oi = 0; oi < NC_OBJECTS; oi++) {
        struct native_object *o = &nc_objects[oi];
        if (o->kind) continue;
        o->kind = NC_MUTEX;
        o->readers = 0; o->writers = 0; o->pins = 0;
        o->head = 0; o->count = 0; o->recv_wait = 0; o->send_wait = 0;
        o->m_state = M_FREE; o->m_recover = 0; o->m_owner = 0;
        o->m_wait = 0; o->m_waiters = 0; o->m_qhead = 0; o->m_enqueues = 0;
        /* Each wait slot owns one channel. The address of the slot is the
         * channel: it is unique per waiter and never moves, so a handoff
         * wakes exactly the contender it picked. */
        for (n = 0; n < MQ_CAP; n++) {
            o->m_chan[n] = (u64)&o->m_chan[n];
            o->m_tkey[n] = 0;
        }
        rc = nc_alloc_handle(oi, NC_MLOCK | NC_MUNLOCK | NC_MWAIT, out);
        if (rc != 0) nc_reclaim(o);
        break;
    }
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return rc;
}

/* Remove one waiter's queue entry, preserving FIFO order of the rest.
 * The queue holds SLOT INDICES; the slot owns the wait channel, so removing
 * an entry only has to release that slot. The remaining entries keep their
 * slots (and therefore their channels) untouched, which is what makes the
 * handoff wakeup exact. */
static void nm_queue_remove(struct native_object *o, u64 key) {
    u32 i;
    u32 kept = 0;
    u32 n = o->m_waiters;
    for (i = 0; i < n; i++) {
        u32 from = (o->m_qhead + i) % MQ_CAP;
        u32 slot = (u32)o->m_queue[from];
        u64 v = o->m_tkey[slot];
        if (v == key) {
            o->m_tkey[slot] = 0;          /* release the slot */
            continue;
        }
        o->m_queue[(o->m_qhead + kept) % MQ_CAP] = (u64)slot;
        kept = kept + 1;
    }
    o->m_waiters = kept;
}

/* Wake every waiter of the mutex. Used where the mutex stops being held
 * without a handoff (poisoned, owner died, the holder was killed): all
 * contenders must reconsider, and each one owns its own channel. */
static void nm_wake_all(struct native_object *o) {
    u32 i;
    u32 n = o->m_waiters;
    for (i = 0; i < n; i++) {
        u32 slot = (u32)o->m_queue[(o->m_qhead + i) % MQ_CAP];
        sched_wakeup(o->m_chan[slot]);
    }
}

/* Allocate a free wait slot for `key`, or MQ_CAP when the table is full. */
static u32 nm_slot_alloc(struct native_object *o, u64 key) {
    u32 i;
    for (i = 0; i < MQ_CAP; i++) {
        if (o->m_tkey[i] == 0) {
            o->m_tkey[i] = key;
            return i;
        }
    }
    return MQ_CAP;
}

s32 native_mutex_lock_intr(u64 handle, u32 block) {
    u64 flags;
    s32 hi;
    s32 rc;
    u32 oi;
    struct native_object *o;
    u64 me;
    u32 first = 1;
    if (block > 1) return NC_INVAL;
    flags = cpu_irq_save();
    spin_lock(&nc_lock);
    hi = nc_lookup(handle);
    if (hi < 0) { rc = hi; goto nm_done; }
    if (!(nc_handles[hi].rights & NC_MLOCK)) { rc = NC_RIGHTS; goto nm_done; }
    oi = nc_handles[hi].object;
    o = &nc_objects[oi];
    if (o->kind != NC_MUTEX) { rc = NC_TYPE; goto nm_done; }
    me = sched_current_owner_key();
    o->pins++;
    for (;;) {
        hi = nc_lookup(handle);
        if (hi < 0) { rc = NC_BAD_HANDLE; break; }
        if (o->m_state == M_NOTREC) { rc = NC_NOT_RECOVERABLE; break; }
        if (o->m_state == M_FREE) {
            o->m_owner = me;
            o->m_state = M_LOCKED;
            o->m_recover = 0;
            rc = 0;
            break;
        }
        if (o->m_state == M_DIED) {
            /* Recovery acquisition: caller MUST consistent() or poison. */
            o->m_owner = me;
            o->m_state = M_LOCKED;
            o->m_recover = 1;
            rc = NC_OWNER_DIED;
            break;
        }
        if (o->m_owner == me) {
            if (first) { rc = NC_DEADLK; break; }
            rc = 0;                        /* woken by a handoff to us */
            break;
        }
        if (!block) { rc = NC_AGAIN; break; }
        if (o->m_waiters >= MQ_CAP) { rc = NC_AGAIN; break; }
        {
            /* Take a wait slot: it owns the channel this waiter parks on,
             * and it is released when the waiter leaves the queue. The slot
             * is what the handoff later wakes, so the wakeup targets exactly
             * the contender that was handed the mutex. */
            u32 slot = nm_slot_alloc(o, me);
            u64 wchan;
            if (slot >= MQ_CAP) { rc = NC_AGAIN; break; }
            wchan = o->m_chan[slot];
            sched_park_prepare(wchan);
            o->m_queue[(o->m_qhead + o->m_waiters) % MQ_CAP] = (u64)slot;
            o->m_waiters = o->m_waiters + 1;
            o->m_enqueues = o->m_enqueues + 1;
            if (sched_park_commit_unlock_spin(wchan, &nc_lock, 0)
                == PARK_INTR_RESULT) {
                spin_lock(&nc_lock);
                if (o->m_owner == me && o->m_state == M_LOCKED) {
                    /* Killed while a handoff landed on us: we never ran user
                     * code owning it, so release cleanly (no recovery). */
                    o->m_owner = 0;
                    o->m_state = M_FREE;
                    o->m_recover = 0;
                    nm_wake_all(o);
                    rc = NC_INTR;
                    break;
                }
                nm_queue_remove(o, me);
                rc = NC_INTR;
                break;
            }
            spin_lock(&nc_lock);
            first = 0;
            if (o->m_owner == me && o->m_state == M_LOCKED) {
                rc = 0;                    /* handed while we slept */
                break;
            }
            nm_queue_remove(o, me);        /* spurious/signal wake: requeue */
            continue;
        }
    }
    o->pins--;
    nc_reclaim(o);
 nm_done:
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return rc;
}

s32 native_mutex_unlock(u64 handle) {
    u64 flags;
    s32 hi;
    s32 rc;
    u32 oi;
    struct native_object *o;
    u64 me;
    flags = cpu_irq_save();
    spin_lock(&nc_lock);
    hi = nc_lookup(handle);
    if (hi < 0) { rc = hi; goto nm_done; }
    if (!(nc_handles[hi].rights & NC_MUNLOCK)) { rc = NC_RIGHTS; goto nm_done; }
    oi = nc_handles[hi].object;
    o = &nc_objects[oi];
    if (o->kind != NC_MUTEX) { rc = NC_TYPE; goto nm_done; }
    me = sched_current_owner_key();
    if (o->m_state != M_LOCKED || o->m_owner != me) { rc = NC_RIGHTS; goto nm_done; }
    if (o->m_recover) {
        /* Abandoned recovery poisons the mutex (terminal, wakes waiters). */
        o->m_state = M_NOTREC;
        o->m_owner = 0;
        o->m_recover = 0;
        nm_wake_all(o);
        rc = 0;
    } else if (o->m_waiters) {
        /* Direct FIFO handoff at unlock time: no newcomer barging. */
        u32 slot = (u32)o->m_queue[o->m_qhead];
        u64 next = o->m_tkey[slot];
        o->m_qhead = (o->m_qhead + 1) % MQ_CAP;
        o->m_waiters = o->m_waiters - 1;
        o->m_owner = next;                 /* state stays M_LOCKED */
        /* The handed-to contender does not need its slot any more: it owns
         * the mutex and will not park again until it unlocks. Release the
         * slot so it can be handed to the next waiter later, and wake it on
         * the channel it parked on. The losers of the handoff stay parked on
         * their own channels and keep their FIFO order. */
        o->m_tkey[slot] = 0;
        sched_wakeup(o->m_chan[slot]);
        rc = 0;
    } else {
        o->m_owner = 0;
        o->m_state = M_FREE;
        o->m_recover = 0;
        rc = 0;
    }
    nc_reclaim(o);
 nm_done:
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return rc;
}

s32 native_mutex_consistent(u64 handle) {
    u64 flags;
    s32 hi;
    s32 rc;
    u32 oi;
    struct native_object *o;
    u64 me;
    flags = cpu_irq_save();
    spin_lock(&nc_lock);
    hi = nc_lookup(handle);
    if (hi < 0) { rc = hi; goto nm_done; }
    if (!(nc_handles[hi].rights & NC_MUNLOCK)) { rc = NC_RIGHTS; goto nm_done; }
    oi = nc_handles[hi].object;
    o = &nc_objects[oi];
    if (o->kind != NC_MUTEX) { rc = NC_TYPE; goto nm_done; }
    me = sched_current_owner_key();
    if (o->m_state == M_NOTREC) { rc = NC_NOT_RECOVERABLE; goto nm_done; }
    if (o->m_state == M_LOCKED && o->m_owner == me && o->m_recover) {
        o->m_recover = 0;
        rc = 0;
    } else {
        rc = NC_INVAL;                     /* not the recovering owner */
    }
 nm_done:
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return rc;
}

/* Observability for deterministic tests: out[0]=state out[1]=waiters
 * out[2]=owner pid part. Requires the WAIT right. */
s32 native_mutex_stat(u64 handle, u32 *out) {
    u64 flags;
    s32 hi;
    s32 rc;
    u32 oi;
    struct native_object *o;
    if (out == 0) return NC_INVAL;
    flags = cpu_irq_save();
    spin_lock(&nc_lock);
    hi = nc_lookup(handle);
    if (hi < 0) { rc = hi; goto nm_done; }
    if (!(nc_handles[hi].rights & NC_MWAIT)) { rc = NC_RIGHTS; goto nm_done; }
    oi = nc_handles[hi].object;
    o = &nc_objects[oi];
    if (o->kind != NC_MUTEX) { rc = NC_TYPE; goto nm_done; }
    out[0] = o->m_state;
    out[1] = o->m_waiters;
    out[2] = (u32)(o->m_owner >> 32);
    out[3] = o->m_enqueues;     /* total enqueue events; monotonic */
    rc = 0;
 nm_done:
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
    return rc;
}

/* Robustness walk: the task identified by key died owning mutexes. Called
 * from task-exit paths OUTSIDE scheduler spinlocks (kthread_exit,
 * proc_exit_task). Marks each owned mutex M_DIED and wakes its waiters; the
 * next acquisition recovers with NC_OWNER_DIED. */
void native_mutex_owner_died_key(u64 key) {
    u64 flags;
    u32 i;
    flags = cpu_irq_save();
    spin_lock(&nc_lock);
    for (i = 0; i < NC_OBJECTS; i++) {
        struct native_object *o = &nc_objects[i];
        if (o->kind == NC_MUTEX && o->m_state == M_LOCKED && o->m_owner == key) {
            o->m_state = M_DIED;
            o->m_owner = 0;
            nm_wake_all(o);
            nc_reclaim(o);
        }
    }
    spin_unlock(&nc_lock);
    cpu_irq_restore(flags);
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
/* --- Native mutex selftest workers (gate 3, deterministic stage machine) ---
 * Stage waits use sched_sleep on per-worker channels, NEVER spin-yield:
 * kmain runs as the BSP idle TCB and pick_next prefers real work over idle,
 * so a spin-yield waiter would starve the controller (the MPMC barrier hit
 * the same constraint). Sleepers empty the runqueue, letting kmain advance
 * the stages. */
static u64 nm_handle;
static volatile u32 nm_stage;
static volatile u32 nm_rc[3];
static volatile u32 nm_order[3];
static volatile u32 nm_order_n;
static u32 nm_stat[3];
static u64 nm_wchan[3];

/* Total enqueue events observed on the mutex under test. Read through
 * native_mutex_stat() (out[3]), which returns the object's own monotonic
 * counter. The queue LENGTH cannot serve this purpose: m_waiters counts
 * queue SLOTS, and a slot stays occupied by the holder until it unlocks
 * (the direct handoff pops the head at unlock time), so m_waiters is not
 * "number of blocked contenders" — after W0 wins, the queue still holds
 * its slot, and using m_waiters as the "contender N is queued" signal
 * fires one step early and lets a later contender enqueue first. */

/* Read the mutex's monotonic enqueue counter. */
static u32 nm_enqueue_count(void) {
    u32 st[4];
    if (native_mutex_stat(nm_handle, st) != 0) {
        return 0;
    }
    return st[3];
}

/* Wait until the mutex has seen `n` enqueue events in total. This is the
 * exact "n contenders are actually blocked on the mutex" fact: the counter
 * moves only inside the enqueue path, so it cannot fire early the way a
 * pre-block announcement can (an announced contender may still be between
 * the announcement and the enqueue, letting a later contender overtake
 * it). */
static void nm_wait_enqueues(u32 n, u32 timeout_ticks) {
    u64 until = sched_clock_ticks() + timeout_ticks;
    while (nm_enqueue_count() < n && sched_clock_ticks() < until) {
        sched_yield();
    }
}
/* Sleep until nm_stage == want under the park-admission contract: the stage
 * read and the park registration are ONE nc_test_lock transaction, and the
 * controller publishes the stage BEFORE waking the channel, so every
 * interleaving resolves — the check sees the stage, the wakeup latches
 * park_notify on the registered PREPARING waiter (commit -> PARK_WOKEN), or
 * the wakeup finds the published sleeper. sched_sleep alone has no such
 * admission and loses wakeups across CPUs (observed on SMP2). */
static void nm_wait_stage(u32 id, u32 want) {
    for (;;) {
        u64 f;
        u32 prepared = 0;
        if (nm_stage == want) return;
        f = cpu_irq_save();
        spin_lock(&nc_test_lock);
        if (nm_stage != want) {
            sched_park_prepare((u64)&nm_wchan[id]);
            prepared = 1;
        }
        spin_unlock(&nc_test_lock);
        cpu_irq_restore(f);
        if (!prepared) return;
        if (nm_stage == want) {
            sched_park_abort();            /* never leave a dangling park */
            return;
        }
        sched_park_commit((u64)&nm_wchan[id], 0);
    }
}

/* FIFO contender. Main gates each lock with a stage so queue order is
 * deterministic: W0 (stage 1), W1 (3), W2 (4). W0 holds the handed-off lock
 * until main releases stage 6, so the handoff chain W0->W1->W2 completes
 * strictly after main's no-barge trylocks. */
static void nm_contender(u64 arg) {
    u32 id = (u32)arg;
    u32 lock_stage = id == 0 ? 1u : (id == 1 ? 3u : 4u);
    s32 rc;
    nm_wait_stage(id, lock_stage);
    rc = native_mutex_lock_intr(nm_handle, 1);
    nm_rc[id] = (u32)rc;
    {
        u64 f = cpu_irq_save();
        spin_lock(&nc_test_lock);
        if (nm_order_n < 3) {
            nm_order[nm_order_n] = id;
            nm_order_n = nm_order_n + 1;
        }
        spin_unlock(&nc_test_lock);
        cpu_irq_restore(f);
    }
    if (id == 0) {
        nm_wait_stage(0, 6u);
    }
    native_mutex_unlock(nm_handle);
    if (id == 2) nm_stage = 7;
    kthread_exit();
}

/* mode 1: lock, publish, exit WITHOUT unlocking (owner death).
 * mode 2: block-lock, expect OWNER_DIED, consistent, unlock (recovery).
 * mode 3: block-lock, expect OWNER_DIED, unlock WITHOUT consistent
 *         (abandoned recovery -> poison), then relock to confirm NOTREC. */
static void nm_lifecycle(u64 arg) {
    u32 mode = (u32)arg;
    s32 rc;
    if (mode == 1) {
        rc = native_mutex_lock_intr(nm_handle, 1);
        nm_rc[0] = (u32)rc;
        nm_stage = 1;
        kthread_exit();
    }
    rc = native_mutex_lock_intr(nm_handle, 1);
    nm_rc[1] = (u32)rc;
    if (rc == NC_OWNER_DIED && mode == 2) {
        native_mutex_consistent(nm_handle);
        native_mutex_unlock(nm_handle);
    } else if (rc == NC_OWNER_DIED && mode == 3) {
        native_mutex_unlock(nm_handle);
        nm_rc[2] = (u32)native_mutex_lock_intr(nm_handle, 0);
    }
    nm_stage = 2;
    kthread_exit();
}

/* Locks, publishes, closes the ONLY capability while LOCKED, then dies: the
 * owner reference must survive the close and drain at the death walk. */
static void nm_close_owner(u64 arg) {
    (void)arg;
    nm_rc[0] = (u32)native_mutex_lock_intr(nm_handle, 1);
    nm_stage = 1;
    native_handle_close(nm_handle);
    kthread_exit();
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

    /* ---- Native robust mutex: API basics (gate 3 contract) ---- */
    ok &= nc_expect((native_mutex_create(&tx) == 0), 60);
    nm_handle = tx;
    ok &= nc_expect((native_mutex_lock_intr(tx, 0) == 0), 61);
    ok &= nc_expect((native_mutex_lock_intr(tx, 0) == NC_DEADLK), 62);
    ok &= nc_expect((native_mutex_stat(tx, nm_stat) == 0), 63);
    ok &= nc_expect((nm_stat[0] == M_LOCKED && nm_stat[1] == 0), 64);
    ok &= nc_expect((native_mutex_unlock(tx) == 0), 65);
    ok &= nc_expect((native_mutex_unlock(tx) == NC_RIGHTS), 66);
    ok &= nc_expect((native_mutex_consistent(tx) == NC_INVAL), 67);
    ok &= nc_expect((native_mutex_lock_intr(tx, 1) == 0), 68);
    ok &= nc_expect((native_mutex_lock_intr(tx, 2) == NC_INVAL), 69);
    ok &= nc_expect((native_mutex_unlock(tx) == 0), 70);
    ok &= nc_expect((native_handle_close(tx) == 0), 71);
    ok &= nc_expect((native_mutex_lock_intr(tx, 0) == NC_BAD_HANDLE), 72);
    ok &= nc_expect((native_mutex_create(&tx) == 0), 73);
    nm_handle = tx;
    ok &= nc_expect((native_handle_dup(tx, NC_MUNLOCK, &dup) == 0), 74);
    ok &= nc_expect((native_mutex_lock_intr(dup, 0) == NC_RIGHTS), 75);
    ok &= nc_expect((native_mutex_lock_intr(tx, 0) == 0), 76);
    ok &= nc_expect((native_mutex_unlock(dup) == 0), 77);
    ok &= nc_expect((native_mutex_unlock(tx) == NC_RIGHTS), 78);
    ok &= nc_expect((native_handle_close(dup) == 0), 79);
    ok &= nc_expect((native_handle_close(tx) == 0), 80);
    if (!ok) goto fail;
    kprintf("      [PASS] native mutex: self-lock DEADLK, nonowner EPERM, stat, attenuated dup\n");

    /* ---- FIFO handoff + no-barge storm (3 queued contenders) ---- */
    ok &= nc_expect((native_mutex_create(&tx) == 0), 81);
    nm_handle = tx;
    ok &= nc_expect((native_mutex_lock_intr(tx, 1) == 0), 82);
    nm_stage = 0; nm_order_n = 0;
    nm_rc[0] = 0xFFFFFFFFu; nm_rc[1] = 0xFFFFFFFFu; nm_rc[2] = 0xFFFFFFFFu;
    flags = cpu_irq_save();
    created = 0;
    for (i = 0; i < 3; i++) {
        if (kthread_create((u64)&nm_contender, (u64)i) == 0xFFFFFFFFu) break;
        created++;
    }
    cpu_irq_restore(flags);
    ok &= nc_expect((created == 3), 83);
    /* Stage arming ORDER: publish the stage, then wake the gated worker.
     * Workers re-check under Mesa semantics, so a worker that has not slept
     * yet loses nothing. */
    nm_stage = 1;
    sched_wakeup((u64)&nm_wchan[0]);
    nm_wait_enqueues(1, 800);
    ok &= nc_expect((nm_enqueue_count() >= 1), 84);
    nm_stage = 3;
    sched_wakeup((u64)&nm_wchan[1]);
    nm_wait_enqueues(2, 800);
    ok &= nc_expect((nm_enqueue_count() >= 2), 85);
    nm_stage = 4;
    sched_wakeup((u64)&nm_wchan[2]);
    nm_wait_enqueues(3, 800);
    ok &= nc_expect((nm_enqueue_count() >= 3), 86);
    ok &= nc_expect((native_mutex_unlock(tx) == 0), 87);       /* handoff -> W0 */
    ok &= nc_expect((native_mutex_lock_intr(tx, 0) == NC_AGAIN), 88);
    ok &= nc_expect((native_mutex_lock_intr(tx, 0) == NC_AGAIN), 89);
    nm_stage = 6;
    sched_wakeup((u64)&nm_wchan[0]);
    until = sched_clock_ticks() + 800;
    while (nm_stage != 7 && sched_clock_ticks() < until) sched_yield();
    ok &= nc_expect((nm_stage == 7), 90);
    ok &= nc_expect((nm_rc[0] == 0 && nm_rc[1] == 0 && nm_rc[2] == 0), 91);
    ok &= nc_expect((nm_order[0] == 0 && nm_order[1] == 1 && nm_order[2] == 2), 92);
    ok &= nc_expect((native_mutex_lock_intr(tx, 0) == 0), 93);
    ok &= nc_expect((native_mutex_unlock(tx) == 0), 94);
    ok &= nc_expect((native_handle_close(tx) == 0), 95);
    if (!ok) goto fail;
    kprintf("      [PASS] native mutex: FIFO handoff, no barging, bounded queue drains\n");

    /* ---- Owner death -> OWNER_DIED recovery ---- */
    ok &= nc_expect((native_mutex_create(&tx) == 0), 96);
    nm_handle = tx;
    nm_stage = 0;
    nm_rc[0] = 0xFFFFFFFFu; nm_rc[1] = 0xFFFFFFFFu; nm_rc[2] = 0xFFFFFFFFu;
    flags = cpu_irq_save();
    wid = kthread_create((u64)&nm_lifecycle, 1);
    cpu_irq_restore(flags);
    ok &= nc_expect((wid != 0xFFFFFFFFu), 97);
    until = sched_clock_ticks() + 800;
    while (nm_stage != 1 && sched_clock_ticks() < until) sched_yield();
    ok &= nc_expect((nm_stage == 1 && nm_rc[0] == 0), 98);
    flags = cpu_irq_save();
    wid = kthread_create((u64)&nm_lifecycle, 2);
    cpu_irq_restore(flags);
    ok &= nc_expect((wid != 0xFFFFFFFFu), 99);
    until = sched_clock_ticks() + 800;
    while (nm_stage != 2 && sched_clock_ticks() < until) sched_yield();
    ok &= nc_expect((nm_stage == 2 && nm_rc[1] == NC_OWNER_DIED), 100);
    ok &= nc_expect((native_mutex_lock_intr(tx, 0) == 0), 101);
    ok &= nc_expect((native_mutex_unlock(tx) == 0), 102);
    ok &= nc_expect((native_handle_close(tx) == 0), 103);
    if (!ok) goto fail;
    kprintf("      [PASS] native mutex: owner death -> OWNER_DIED + consistent recovery\n");

    /* ---- Abandoned recovery poisons as NOT_RECOVERABLE ---- */
    ok &= nc_expect((native_mutex_create(&tx) == 0), 104);
    nm_handle = tx;
    nm_stage = 0;
    nm_rc[0] = 0xFFFFFFFFu; nm_rc[1] = 0xFFFFFFFFu; nm_rc[2] = 0xFFFFFFFFu;
    flags = cpu_irq_save();
    wid = kthread_create((u64)&nm_lifecycle, 1);
    cpu_irq_restore(flags);
    ok &= nc_expect((wid != 0xFFFFFFFFu), 105);
    until = sched_clock_ticks() + 800;
    while (nm_stage != 1 && sched_clock_ticks() < until) sched_yield();
    ok &= nc_expect((nm_stage == 1 && nm_rc[0] == 0), 106);
    flags = cpu_irq_save();
    wid = kthread_create((u64)&nm_lifecycle, 3);
    cpu_irq_restore(flags);
    ok &= nc_expect((wid != 0xFFFFFFFFu), 107);
    until = sched_clock_ticks() + 800;
    while (nm_stage != 2 && sched_clock_ticks() < until) sched_yield();
    ok &= nc_expect((nm_stage == 2 && nm_rc[1] == NC_OWNER_DIED
                     && nm_rc[2] == NC_NOT_RECOVERABLE), 108);
    ok &= nc_expect((native_mutex_lock_intr(tx, 0) == NC_NOT_RECOVERABLE), 109);
    ok &= nc_expect((native_mutex_consistent(tx) == NC_NOT_RECOVERABLE), 110);
    ok &= nc_expect((native_handle_close(tx) == 0), 111);
    if (!ok) goto fail;
    kprintf("      [PASS] native mutex: abandoned recovery poisons NOT_RECOVERABLE\n");

    /* ---- Last-cap close while LOCKED: owner reference until death ---- */
    ok &= nc_expect((native_mutex_create(&tx) == 0), 112);
    nm_handle = tx;
    nm_stage = 0;
    nm_rc[0] = 0xFFFFFFFFu;
    flags = cpu_irq_save();
    wid = kthread_create((u64)&nm_close_owner, 0);
    cpu_irq_restore(flags);
    ok &= nc_expect((wid != 0xFFFFFFFFu), 113);
    until = sched_clock_ticks() + 800;
    while (nm_stage != 1 && sched_clock_ticks() < until) sched_yield();
    ok &= nc_expect((nm_stage == 1 && nm_rc[0] == 0), 114);
    until = sched_clock_ticks() + 800;
    while (!nc_empty() && sched_clock_ticks() < until) sched_yield();
    ok &= nc_expect((nc_empty()), 115);
    if (!ok) goto fail;
    kprintf("      [PASS] native mutex: close-while-locked survives, drains at death\n");

    ok &= nc_expect((nc_empty()), 116);
    if (!ok) goto fail;
    kprintf("[NATIVE] PASS kernel-only object/channel suite\n");
    return;
 fail:
    kprintf("[NATIVE] FAIL kernel-only object/channel suite (done=%u errors=%u count=%u mask=0x%x)\n",
            (u64)nc_test_done, (u64)nc_test_errors, (u64)nc_test_count, (u64)nc_test_mask);
}
