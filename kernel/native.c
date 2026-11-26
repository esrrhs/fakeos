package kernel;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t  s32;

/* ---------------------------------------------------------------------------
 * Sharded native typed-object core (architecture sections 3, 4, 5, 6, 7).
 *
 * Object kinds:
 *   NC_CHANNEL (1) — bounded FIFO message queue: copied byte payloads (0..256B)
 *                    plus up to 4 transferred capabilities (DUP or two-phase
 *                    all-or-nothing MOVE). Scalar u64 send/recv share the same
 *                    8-byte envelope.
 *   NC_MUTEX   (2) — robust nonrecursive capability mutex: stable (pid,tid)
 *                    owner identity, bounded FIFO queue with direct handoff at
 *                    unlock (zero newcomer barging), OWNER_DIED + consistent()
 *                    recovery or terminal NOT_RECOVERABLE poison.
 *   NC_TASK    (3) — typed kernel task completion/control object (JOIN/CANCEL).
 *   NC_PROC    (4) — typed native process completion/control object.
 *
 * Locking hierarchy (strict DAG, never inverted):
 *   nc_obj_locks[oi_0] -> nc_obj_locks[oi_1] (oi_0 < oi_1)
 *       -> nc_reg_lock (pure leaf: handle/object slot table only, never wakes)
 *       -> sched_run_lock (via sched_wakeup / sched_park_* / sched_nat_acquire)
 *
 * Independent channels and mutexes operate under disjoint nc_obj_locks[oi]
 * spinlocks with zero cross-object serialization.
 * ------------------------------------------------------------------------- */

enum {
    NC_SEND     = 1,
    NC_RECV     = 2,
    NC_MLOCK    = 4,
    NC_MUNLOCK  = 8,
    NC_MWAIT    = 16,
    NC_TRANSFER = 32,
    NC_JOIN     = 64,
    NC_CANCEL   = 128,
    NC_INSPECT  = 256,
    NC_ALL_RIGHTS = 511,

    NC_CHANNEL  = 1,
    NC_MUTEX    = 2,
    NC_TASK     = 3,
    NC_PROC     = 4,

    NC_EOF             = 1,
    NC_BAD_HANDLE      = -1,
    NC_RIGHTS          = -2,
    NC_FULL            = -3,
    NC_AGAIN           = -4,
    NC_CLOSED          = -5,
    NC_INVAL           = -6,
    NC_TYPE            = -7,
    NC_INTR            = -8,
    NC_DEADLK          = -9,
    NC_OWNER_DIED      = -10,
    NC_NOT_RECOVERABLE = -11,
    NC_TIMEDOUT        = -12,
    NC_MSGSIZE         = -13,

    NC_OBJECTS       = 64,
    NC_HANDLES       = 256,
    NC_CAPACITY      = 8,
    NC_TOTAL_MSGS    = 512,   /* NC_OBJECTS * NC_CAPACITY */
    NC_MSG_MAX_BYTES = 256,
    NC_MSG_MAX_CAPS  = 4,

    M_FREE   = 0,
    M_LOCKED = 1,
    M_DIED   = 2,
    M_NOTREC = 3,
    MQ_CAP   = 8,
    NW_SLOTS = 16,

    NATW_RECV   = 1,
    NATW_SEND   = 2,
    NATW_MUTEX  = 4,
    NATW_DONE   = 8,
    NATW_EOF    = 16,
    NATW_CLOSED = 32,

    PARK_INTR_RESULT    = 2,
    PARK_TIMEOUT_RESULT = 3
};

struct native_object {
    u32 kind;
    u32 readers;
    u32 writers;
    u32 pins;
    /* Channel state */
    u32 head;
    u32 count;
    u32 recv_wait;
    u32 send_wait;
    /* Mutex state */
    u32 m_state;
    u32 m_recover;
    u64 m_owner;
    u32 m_wait;
    u32 m_waiters;
    u32 m_qhead;
    u32 m_enqueues;
    u64 m_chan[MQ_CAP];
    u64 m_queue[MQ_CAP];
    u64 m_tkey[MQ_CAP];
    /* Task / Process completion state (NC_TASK / NC_PROC) */
    u32 exec_active;
    u32 exec_done;
    u64 exec_target;
    u64 exec_result;
    /* Multi-object wait_any registrations (stack/task wait tokens) */
    u64 wait_any_chans[NW_SLOTS];
};

struct native_handle {
    u64 generation;
    u32 live;
    u32 object;
    u32 rights;
};

/* Shared request envelope between natcap.c and native.c (avoids >4 args). */
struct nc_msg_req {
    u8 *bytes;
    u32 byte_len;
    u32 ncaps;
    u64 cap_handles[NC_MSG_MAX_CAPS];
    u32 cap_rights[NC_MSG_MAX_CAPS];
    u32 caller_pid;
    u32 move_count;
    u32 src_slots[NC_MSG_MAX_CAPS];
    u32 src_gens[NC_MSG_MAX_CAPS];
    u32 src_modes[NC_MSG_MAX_CAPS];
    u64 moved_old_nh[NC_MSG_MAX_CAPS];
    u64 recv_installed_caps[NC_MSG_MAX_CAPS];
};

struct nc_wait_entry {
    u64 nh;
    u32 events;
    u32 revents;
};

static struct native_object nc_objects[NC_OBJECTS];
static struct native_handle nc_handles[NC_HANDLES];
static volatile u32 nc_reg_lock;
static volatile u32 nc_obj_locks[NC_OBJECTS];

/* Flat message payload & capability storage (fakecc-safe: no 2D struct array). */
static u8  nc_msg_bytes[NC_TOTAL_MSGS * NC_MSG_MAX_BYTES];
static u32 nc_msg_len[NC_TOTAL_MSGS];
static u32 nc_msg_ncaps[NC_TOTAL_MSGS];
static u64 nc_msg_caps[NC_TOTAL_MSGS * NC_MSG_MAX_CAPS];
static u32 nc_msg_cap_rights[NC_TOTAL_MSGS * NC_MSG_MAX_CAPS];

static u32 nc_wait_rotor[8];

extern void spin_lock(volatile u32 *lock);
extern void spin_unlock(volatile u32 *lock);
extern u64 cpu_irq_save(void);
extern void cpu_irq_restore(u64 flags);
extern void sched_sleep_unlock_spin(u64 chan, volatile u32 *lock);
extern void sched_park_abort(void);
extern u32 sched_park_commit(u64 chan, u32 nonrestartable);
extern void sched_wakeup(u64 chan);
extern void sched_park_prepare(u64 chan);
extern void sched_park_prepare_deadline(u64 chan, u64 abs_ns);
extern u32 sched_park_commit_unlock_spin(u64 chan, volatile u32 *lock, u32 nonrestartable);
extern u64 sched_current_owner_key(void);
extern u64 sched_monotonic_ns(void);
extern u32 smp_cpuid(void);
extern s32 nat_commit_send_caps(struct nc_msg_req *req);
extern s32 nat_commit_recv_caps(struct nc_msg_req *req);

/* Validate deadline encoding (architecture section 5):
 *   0                  = nonblocking poll
 *   1..0x7FFFFFFFFFFFFFFF = monotonic absolute deadline in ns
 *   0xFFFFFFFFFFFFFFFF = infinite wait
 *   anything else      = overflow / invalid -> 0 */
static u32 nc_deadline_valid(u64 abs_ns) {
    if (abs_ns == ~0ULL) return 1;
    if (abs_ns <= 0x7FFFFFFFFFFFFFFFULL) return 1;
    return 0;
}

/* Requires nc_reg_lock. */
static s32 nc_lookup_reg_locked(u64 h) {
    u32 i = (u32)(h & 0xFFFFULL) - 1;
    if (i >= NC_HANDLES || !nc_handles[i].live
        || nc_handles[i].generation != (h >> 16)) {
        return NC_BAD_HANDLE;
    }
    return (s32)i;
}

static u32 nc_functional_rights(u32 rights) {
    return rights & ~(u32)NC_TRANSFER;
}

static u32 nc_rights_fit_kind(u32 kind, u32 rights) {
    u32 fn = nc_functional_rights(rights);
    if (fn == 0 || (rights & ~(u32)NC_ALL_RIGHTS) != 0) return 0;
    if (kind == NC_CHANNEL) {
        return (fn & ~(u32)(NC_SEND | NC_RECV)) == 0;
    }
    if (kind == NC_MUTEX) {
        return (fn & ~(u32)(NC_MLOCK | NC_MUNLOCK | NC_MWAIT)) == 0;
    }
    if (kind == NC_TASK || kind == NC_PROC) {
        return (fn & ~(u32)(NC_JOIN | NC_CANCEL | NC_INSPECT)) == 0;
    }
    return 0;
}

/* Requires nc_reg_lock (and nc_obj_locks[oi] if oi is already published). */
static s32 nc_alloc_handle_locked(u32 oi, u32 rights, u64 *out) {
    u32 i;
    struct native_object *o = &nc_objects[oi];
    if (!nc_rights_fit_kind(o->kind, rights)) return NC_RIGHTS;
    for (i = 0; i < NC_HANDLES; i++) {
        if (!nc_handles[i].live
            && nc_handles[i].generation < 0x0000FFFFFFFFFFFFULL) {
            struct native_handle *h = &nc_handles[i];
            h->generation = h->generation + 1;
            h->live = 1;
            h->object = oi;
            h->rights = rights;
            if (rights & NC_SEND) o->writers = o->writers + 1;
            if (rights & NC_RECV) o->readers = o->readers + 1;
            if (rights & (NC_MLOCK | NC_MUNLOCK | NC_MWAIT))
                o->readers = o->readers + 1;
            if (rights & (NC_JOIN | NC_CANCEL | NC_INSPECT))
                o->readers = o->readers + 1;
            *out = (h->generation << 16) | (u64)(i + 1);
            return 0;
        }
    }
    return NC_FULL;
}

/* Requires nc_reg_lock. */
static void nc_reclaim_reg_locked(u32 oi) {
    struct native_object *o = &nc_objects[oi];
    if (o->readers || o->writers || o->pins) return;
    if (o->kind == NC_MUTEX && o->m_state == M_LOCKED) return;
    if ((o->kind == NC_TASK || o->kind == NC_PROC) && o->exec_active) return;
    o->kind = 0;
}

/* Wake every wait_any sleeper currently registered on object `o`.
 * Caller holds nc_obj_locks[oi] with IRQs off. */
static void nc_wake_wait_any_locked(struct native_object *o) {
    u32 s;
    for (s = 0; s < NW_SLOTS; s++) {
        u64 ch = o->wait_any_chans[s];
        if (ch != 0) {
            sched_wakeup(ch);
        }
    }
}

/* Pin an object from a handle, verifying kind (0 = any) and required rights.
 * Returns >=0 object index on success (with o->pins incremented), or NC_*. */
static s32 nc_pin_handle(u64 handle, u32 want_rights, u32 want_kind) {
    u64 flags = cpu_irq_save();
    s32 hi;
    s32 rc;
    spin_lock(&nc_reg_lock);
    hi = nc_lookup_reg_locked(handle);
    if (hi < 0) {
        rc = NC_BAD_HANDLE;
    } else {
        struct native_handle *h = &nc_handles[hi];
        struct native_object *o = &nc_objects[h->object];
        if ((h->rights & want_rights) != want_rights) {
            rc = NC_RIGHTS;
        } else if (want_kind != 0 && o->kind != want_kind) {
            rc = NC_TYPE;
        } else {
            o->pins = o->pins + 1;
            rc = (s32)h->object;
        }
    }
    spin_unlock(&nc_reg_lock);
    cpu_irq_restore(flags);
    return rc;
}

static u32 nc_handle_still_live(u64 handle) {
    u32 ok;
    spin_lock(&nc_reg_lock);
    ok = nc_lookup_reg_locked(handle) >= 0 ? 1u : 0u;
    spin_unlock(&nc_reg_lock);
    return ok;
}

/* Drain any queued capability handles from channel `oi` into `out[]` when
 * the last reader is gone. Caller holds nc_obj_locks[oi]. */
static u32 nc_drain_queued_caps_locked(u32 oi, u64 *out, u32 cap) {
    struct native_object *o = &nc_objects[oi];
    u32 n = 0;
    while (o->count > 0) {
        u32 mslot = oi * NC_CAPACITY + o->head;
        u32 nc = nc_msg_ncaps[mslot];
        u32 k;
        for (k = 0; k < nc; k++) {
            u64 ch = nc_msg_caps[mslot * NC_MSG_MAX_CAPS + k];
            nc_msg_caps[mslot * NC_MSG_MAX_CAPS + k] = 0;
            if (ch != 0 && n < cap) {
                out[n] = ch;
                n = n + 1;
            }
        }
        nc_msg_ncaps[mslot] = 0;
        nc_msg_len[mslot] = 0;
        o->head = (o->head + 1) % NC_CAPACITY;
        o->count = o->count - 1;
    }
    o->head = 0;
    return n;
}

/* Drop one pin on `oi` (IRQs may be on or off). */
static void nc_unpin_object(u32 oi) {
    u64 flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    spin_lock(&nc_reg_lock);
    nc_objects[oi].pins = nc_objects[oi].pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
}

static void nc_zero_object_common(struct native_object *o, u32 kind) {
    u32 s;
    o->kind = kind;
    o->readers = 0;
    o->writers = 0;
    o->pins = 0;
    o->head = 0;
    o->count = 0;
    o->recv_wait = 0;
    o->send_wait = 0;
    o->m_state = M_FREE;
    o->m_recover = 0;
    o->m_owner = 0;
    o->m_wait = 0;
    o->m_waiters = 0;
    o->m_qhead = 0;
    o->m_enqueues = 0;
    o->exec_active = 0;
    o->exec_done = 0;
    o->exec_target = 0;
    o->exec_result = 0;
    for (s = 0; s < NW_SLOTS; s++) {
        o->wait_any_chans[s] = 0;
    }
}

s32 native_channel_create(u64 *send, u64 *recv) {
    u64 flags;
    u32 oi;
    s32 rc = NC_FULL;
    if (send == 0 || recv == 0 || send == recv) return NC_INVAL;
    *send = 0;
    *recv = 0;
    flags = cpu_irq_save();
    spin_lock(&nc_reg_lock);
    for (oi = 0; oi < NC_OBJECTS; oi++) {
        struct native_object *o = &nc_objects[oi];
        if (o->kind) continue;
        nc_zero_object_common(o, NC_CHANNEL);
        {
            u32 m;
            for (m = 0; m < NC_CAPACITY; m++) {
                u32 mslot = oi * NC_CAPACITY + m;
                nc_msg_len[mslot] = 0;
                nc_msg_ncaps[mslot] = 0;
            }
        }
        rc = nc_alloc_handle_locked(oi, NC_SEND | NC_TRANSFER, send);
        if (rc == 0) {
            rc = nc_alloc_handle_locked(oi, NC_RECV | NC_TRANSFER, recv);
            if (rc != 0) {
                s32 shi = nc_lookup_reg_locked(*send);
                if (shi >= 0) {
                    nc_handles[shi].live = 0;
                    o->writers = 0;
                }
            }
        }
        if (rc != 0) {
            *send = 0;
            *recv = 0;
            o->kind = 0;
        }
        break;
    }
    spin_unlock(&nc_reg_lock);
    cpu_irq_restore(flags);
    return rc;
}

s32 native_handle_dup(u64 handle, u32 rights, u64 *out) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 hi;
    s32 rc;
    if (out == 0 || nc_functional_rights(rights) == 0
        || (rights & ~(u32)NC_ALL_RIGHTS) != 0) {
        return NC_INVAL;
    }
    *out = 0;
    pinned = nc_pin_handle(handle, 0, 0);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    spin_lock(&nc_reg_lock);
    hi = nc_lookup_reg_locked(handle);
    if (hi < 0) {
        rc = NC_BAD_HANDLE;
    } else if ((nc_handles[hi].rights & rights) != rights) {
        rc = NC_RIGHTS;
    } else {
        rc = nc_alloc_handle_locked(oi, rights, out);
    }
    nc_objects[oi].pins = nc_objects[oi].pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

/* Iterative handle close: closing the last reader of a channel may discard
 * queued capabilities; processing them via an explicit worklist avoids
 * unbounded recursion on the kernel stack. */
s32 native_handle_close(u64 handle) {
    u64 work[64];
    u32 head = 0;
    u32 tail = 0;
    s32 first_rc = 0;
    u32 first = 1;

    work[tail] = handle;
    tail = tail + 1;

    while (head != tail) {
        u64 cur = work[head];
        s32 pinned;
        u32 oi;
        u64 flags;
        s32 hi;
        head = (head + 1) % 64;

        pinned = nc_pin_handle(cur, 0, 0);
        if (pinned < 0) {
            if (first) first_rc = pinned;
            first = 0;
            continue;
        }
        oi = (u32)pinned;
        flags = cpu_irq_save();
        spin_lock(&nc_obj_locks[oi]);
        spin_lock(&nc_reg_lock);
        hi = nc_lookup_reg_locked(cur);
        if (hi < 0) {
            if (first) first_rc = NC_BAD_HANDLE;
            nc_objects[oi].pins = nc_objects[oi].pins - 1;
            nc_reclaim_reg_locked(oi);
            spin_unlock(&nc_reg_lock);
            spin_unlock(&nc_obj_locks[oi]);
            cpu_irq_restore(flags);
            first = 0;
            continue;
        }
        {
            struct native_handle *h = &nc_handles[hi];
            struct native_object *o = &nc_objects[oi];
            u32 r = h->rights;
            if (r & NC_SEND) o->writers = o->writers - 1;
            if (r & NC_RECV) o->readers = o->readers - 1;
            if (r & (NC_MLOCK | NC_MUNLOCK | NC_MWAIT)) o->readers = o->readers - 1;
            if (r & (NC_JOIN | NC_CANCEL | NC_INSPECT)) o->readers = o->readers - 1;
            h->live = 0;
            spin_unlock(&nc_reg_lock);

            if (o->kind == NC_CHANNEL && o->readers == 0 && o->count > 0) {
                u64 dropped[32];
                u32 nd = nc_drain_queued_caps_locked(oi, dropped, 32);
                u32 k;
                for (k = 0; k < nd; k++) {
                    u32 next_t = (tail + 1) % 64;
                    if (next_t != head) {
                        work[tail] = dropped[k];
                        tail = next_t;
                    }
                }
            }
            sched_wakeup((u64)&o->recv_wait);
            sched_wakeup((u64)&o->send_wait);
            nc_wake_wait_any_locked(o);

            spin_lock(&nc_reg_lock);
            o->pins = o->pins - 1;
            nc_reclaim_reg_locked(oi);
            spin_unlock(&nc_reg_lock);
        }
        spin_unlock(&nc_obj_locks[oi]);
        cpu_irq_restore(flags);
        first = 0;
    }
    return first_rc;
}

static void nc_pack_u64(u8 *dst, u64 v) {
    u32 b;
    for (b = 0; b < 8; b++) {
        dst[b] = (u8)((v >> (b * 8)) & 0xFFu);
    }
}

static u64 nc_unpack_u64(const u8 *src) {
    u64 v = 0;
    u32 b;
    for (b = 0; b < 8; b++) {
        v = v | ((u64)src[b] << (b * 8));
    }
    return v;
}

/* Kernel-only uninterruptible scalar transfer (preserved for kernel callers). */
static s32 nc_transfer(u64 handle, u64 *value, u32 send, u32 block) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    struct native_object *o;
    if (value == 0 || block > 1) return NC_INVAL;
    pinned = nc_pin_handle(handle, send ? NC_SEND : NC_RECV, NC_CHANNEL);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];

    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    for (;;) {
        if (!nc_handle_still_live(handle)) { rc = NC_BAD_HANDLE; break; }
        if (send) {
            if (!o->readers) { rc = NC_CLOSED; break; }
            if (o->count < NC_CAPACITY) {
                u32 mslot = oi * NC_CAPACITY + ((o->head + o->count) % NC_CAPACITY);
                nc_pack_u64(&nc_msg_bytes[mslot * NC_MSG_MAX_BYTES], *value);
                nc_msg_len[mslot] = 8;
                nc_msg_ncaps[mslot] = 0;
                o->count = o->count + 1;
                sched_wakeup((u64)&o->recv_wait);
                nc_wake_wait_any_locked(o);
                rc = 0;
                break;
            }
        } else {
            if (o->count) {
                u32 mslot = oi * NC_CAPACITY + o->head;
                if (nc_msg_ncaps[mslot] != 0 || nc_msg_len[mslot] != 8) {
                    rc = NC_MSGSIZE;
                    break;
                }
                *value = nc_unpack_u64(&nc_msg_bytes[mslot * NC_MSG_MAX_BYTES]);
                nc_msg_len[mslot] = 0;
                o->head = (o->head + 1) % NC_CAPACITY;
                o->count = o->count - 1;
                sched_wakeup((u64)&o->send_wait);
                nc_wake_wait_any_locked(o);
                rc = 0;
                break;
            }
            if (!o->writers) { rc = NC_EOF; break; }
        }
        if (!block) { rc = NC_AGAIN; break; }
        sched_sleep_unlock_spin(send ? (u64)&o->send_wait : (u64)&o->recv_wait,
                                &nc_obj_locks[oi]);
        spin_lock(&nc_obj_locks[oi]);
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

/* Interruptible + deadline-aware scalar transfer (<=4 arguments). */
static s32 nc_transfer_deadline(u64 handle, u64 *value, u32 send, u64 abs_deadline_ns) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    u32 timed_flag = 0;
    struct native_object *o;
    if (value == 0 || !nc_deadline_valid(abs_deadline_ns)) return NC_INVAL;
    pinned = nc_pin_handle(handle, send ? NC_SEND : NC_RECV, NC_CHANNEL);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];

    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    for (;;) {
        if (!nc_handle_still_live(handle)) { rc = NC_BAD_HANDLE; break; }
        if (send) {
            if (!o->readers) { rc = NC_CLOSED; break; }
            if (o->count < NC_CAPACITY) {
                u32 mslot = oi * NC_CAPACITY + ((o->head + o->count) % NC_CAPACITY);
                nc_pack_u64(&nc_msg_bytes[mslot * NC_MSG_MAX_BYTES], *value);
                nc_msg_len[mslot] = 8;
                nc_msg_ncaps[mslot] = 0;
                o->count = o->count + 1;
                sched_wakeup((u64)&o->recv_wait);
                nc_wake_wait_any_locked(o);
                rc = 0;
                break;
            }
        } else {
            if (o->count) {
                u32 mslot = oi * NC_CAPACITY + o->head;
                if (nc_msg_ncaps[mslot] != 0 || nc_msg_len[mslot] != 8) {
                    rc = NC_MSGSIZE;
                    break;
                }
                *value = nc_unpack_u64(&nc_msg_bytes[mslot * NC_MSG_MAX_BYTES]);
                nc_msg_len[mslot] = 0;
                o->head = (o->head + 1) % NC_CAPACITY;
                o->count = o->count - 1;
                sched_wakeup((u64)&o->send_wait);
                nc_wake_wait_any_locked(o);
                rc = 0;
                break;
            }
            if (!o->writers) { rc = NC_EOF; break; }
        }
        if (abs_deadline_ns == 0) { rc = NC_AGAIN; break; }
        if (timed_flag) { rc = NC_TIMEDOUT; break; }
        {
            u64 wchan = send ? (u64)&o->send_wait : (u64)&o->recv_wait;
            u32 pr;
            sched_park_prepare_deadline(wchan, abs_deadline_ns);
            pr = sched_park_commit_unlock_spin(wchan, &nc_obj_locks[oi], 0);
            spin_lock(&nc_obj_locks[oi]);
            if (pr == PARK_INTR_RESULT) {
                rc = NC_INTR;
                break;
            }
            if (pr == PARK_TIMEOUT_RESULT) {
                /* Re-check queue once before failing: ready work wins. */
                timed_flag = 1;
            }
        }
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

s32 native_channel_send(u64 handle, u64 value, u32 block) {
    return nc_transfer(handle, &value, 1, block);
}
s32 native_channel_recv(u64 handle, u64 *value, u32 block) {
    return nc_transfer(handle, value, 0, block);
}
s32 native_channel_send_intr(u64 handle, u64 value, u32 block) {
    if (block > 1) return NC_INVAL;
    return nc_transfer_deadline(handle, &value, 1, block ? ~0ULL : 0ULL);
}
s32 native_channel_recv_intr(u64 handle, u64 *value, u32 block) {
    if (block > 1) return NC_INVAL;
    return nc_transfer_deadline(handle, value, 0, block ? ~0ULL : 0ULL);
}
s32 native_channel_send_timed(u64 handle, u64 value, u64 abs_deadline_ns) {
    return nc_transfer_deadline(handle, &value, 1, abs_deadline_ns);
}
s32 native_channel_recv_timed(u64 handle, u64 *value, u64 abs_deadline_ns) {
    return nc_transfer_deadline(handle, value, 0, abs_deadline_ns);
}

/* ---------------------------------------------------------------------------
 * Bounded byte payload + two-phase capability transfer (DUP and MOVE).
 * User memory has ALREADY been validated/copied into `req` by natcap.c with
 * IRQs ON and zero spinlocks held.
 * ------------------------------------------------------------------------- */
s32 native_chan_send_msg(u64 handle, struct nc_msg_req *req, u64 abs_deadline_ns) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    u32 timed_flag = 0;
    struct native_object *o;
    if (req == 0 || req->byte_len > NC_MSG_MAX_BYTES
        || req->ncaps > NC_MSG_MAX_CAPS || !nc_deadline_valid(abs_deadline_ns)) {
        return NC_INVAL;
    }
    pinned = nc_pin_handle(handle, NC_SEND, NC_CHANNEL);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];

    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    for (;;) {
        if (!nc_handle_still_live(handle)) { rc = NC_BAD_HANDLE; break; }
        if (!o->readers) { rc = NC_CLOSED; break; }
        if (o->count < NC_CAPACITY) {
            u32 mslot;
            u32 k;
            if (req->ncaps > 0 && req->caller_pid != 0) {
                /* Two-phase commit: verify source slots are still live at
                 * their expected generations and atomically retire MOVE slots
                 * under o->obj_lock -> sched_run_lock. */
                s32 crc = nat_commit_send_caps(req);
                if (crc != 0) {
                    rc = crc;
                    break;
                }
            }
            mslot = oi * NC_CAPACITY + ((o->head + o->count) % NC_CAPACITY);
            for (k = 0; k < req->byte_len; k++) {
                nc_msg_bytes[mslot * NC_MSG_MAX_BYTES + k] = req->bytes[k];
            }
            nc_msg_len[mslot] = req->byte_len;
            nc_msg_ncaps[mslot] = req->ncaps;
            for (k = 0; k < req->ncaps; k++) {
                nc_msg_caps[mslot * NC_MSG_MAX_CAPS + k] = req->cap_handles[k];
                nc_msg_cap_rights[mslot * NC_MSG_MAX_CAPS + k] = req->cap_rights[k];
                req->cap_handles[k] = 0; /* ownership transferred to queue */
            }
            o->count = o->count + 1;
            sched_wakeup((u64)&o->recv_wait);
            nc_wake_wait_any_locked(o);
            rc = 0;
            break;
        }
        if (abs_deadline_ns == 0) { rc = NC_AGAIN; break; }
        if (timed_flag) { rc = NC_TIMEDOUT; break; }
        {
            u64 wchan = (u64)&o->send_wait;
            u32 pr;
            sched_park_prepare_deadline(wchan, abs_deadline_ns);
            pr = sched_park_commit_unlock_spin(wchan, &nc_obj_locks[oi], 0);
            spin_lock(&nc_obj_locks[oi]);
            if (pr == PARK_INTR_RESULT) { rc = NC_INTR; break; }
            if (pr == PARK_TIMEOUT_RESULT) { timed_flag = 1; }
        }
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

s32 native_chan_recv_msg(u64 handle, struct nc_msg_req *req, u64 abs_deadline_ns) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    u32 timed_flag = 0;
    struct native_object *o;
    if (req == 0 || !nc_deadline_valid(abs_deadline_ns)) return NC_INVAL;
    pinned = nc_pin_handle(handle, NC_RECV, NC_CHANNEL);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];

    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    for (;;) {
        if (!nc_handle_still_live(handle)) { rc = NC_BAD_HANDLE; break; }
        if (o->count > 0) {
            u32 mslot = oi * NC_CAPACITY + o->head;
            u32 mlen = nc_msg_len[mslot];
            u32 mcaps = nc_msg_ncaps[mslot];
            u32 k;
            /* Peek check: short buffers leave the message & caps intact! */
            if (mlen > req->byte_len || mcaps > req->ncaps) {
                rc = NC_MSGSIZE;
                break;
            }
            req->ncaps = mcaps;
            for (k = 0; k < mcaps; k++) {
                req->cap_handles[k] = nc_msg_caps[mslot * NC_MSG_MAX_CAPS + k];
                req->cap_rights[k] = nc_msg_cap_rights[mslot * NC_MSG_MAX_CAPS + k];
            }
            if (mcaps > 0 && req->caller_pid != 0) {
                s32 irc = nat_commit_recv_caps(req);
                if (irc != 0) {
                    /* Receiver cap table full: leave message intact at head! */
                    rc = irc;
                    break;
                }
            }
            req->byte_len = mlen;
            for (k = 0; k < mlen; k++) {
                req->bytes[k] = nc_msg_bytes[mslot * NC_MSG_MAX_BYTES + k];
            }
            for (k = 0; k < mcaps; k++) {
                nc_msg_caps[mslot * NC_MSG_MAX_CAPS + k] = 0;
                nc_msg_cap_rights[mslot * NC_MSG_MAX_CAPS + k] = 0;
            }
            nc_msg_len[mslot] = 0;
            nc_msg_ncaps[mslot] = 0;
            o->head = (o->head + 1) % NC_CAPACITY;
            o->count = o->count - 1;
            sched_wakeup((u64)&o->send_wait);
            nc_wake_wait_any_locked(o);
            rc = 0;
            break;
        }
        if (!o->writers) {
            req->byte_len = 0;
            req->ncaps = 0;
            rc = NC_EOF;
            break;
        }
        if (abs_deadline_ns == 0) { rc = NC_AGAIN; break; }
        if (timed_flag) { rc = NC_TIMEDOUT; break; }
        {
            u64 wchan = (u64)&o->recv_wait;
            u32 pr;
            sched_park_prepare_deadline(wchan, abs_deadline_ns);
            pr = sched_park_commit_unlock_spin(wchan, &nc_obj_locks[oi], 0);
            spin_lock(&nc_obj_locks[oi]);
            if (pr == PARK_INTR_RESULT) { rc = NC_INTR; break; }
            if (pr == PARK_TIMEOUT_RESULT) { timed_flag = 1; }
        }
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

/* ---------------------------------------------------------------------------
 * Native robust mutex (architecture section 6, per-object spinlock + deadline).
 * ------------------------------------------------------------------------- */
s32 native_mutex_create(u64 *out) {
    u64 flags;
    u32 oi;
    u32 n;
    s32 rc = NC_FULL;
    if (out == 0) return NC_INVAL;
    *out = 0;
    flags = cpu_irq_save();
    spin_lock(&nc_reg_lock);
    for (oi = 0; oi < NC_OBJECTS; oi++) {
        struct native_object *o = &nc_objects[oi];
        if (o->kind) continue;
        nc_zero_object_common(o, NC_MUTEX);
        for (n = 0; n < MQ_CAP; n++) {
            o->m_chan[n] = (u64)&o->m_chan[n];
            o->m_tkey[n] = 0;
        }
        rc = nc_alloc_handle_locked(
            oi, NC_MLOCK | NC_MUNLOCK | NC_MWAIT | NC_TRANSFER, out);
        if (rc != 0) o->kind = 0;
        break;
    }
    spin_unlock(&nc_reg_lock);
    cpu_irq_restore(flags);
    return rc;
}

static void nm_queue_remove(struct native_object *o, u64 key) {
    u32 i;
    u32 kept = 0;
    u32 n = o->m_waiters;
    for (i = 0; i < n; i++) {
        u32 from = (o->m_qhead + i) % MQ_CAP;
        u32 slot = (u32)o->m_queue[from];
        u64 v = o->m_tkey[slot];
        if (v == key) {
            o->m_tkey[slot] = 0;
            continue;
        }
        o->m_queue[(o->m_qhead + kept) % MQ_CAP] = (u64)slot;
        kept = kept + 1;
    }
    o->m_waiters = kept;
}

static void nm_wake_all(struct native_object *o) {
    u32 i;
    u32 n = o->m_waiters;
    for (i = 0; i < n; i++) {
        u32 slot = (u32)o->m_queue[(o->m_qhead + i) % MQ_CAP];
        sched_wakeup(o->m_chan[slot]);
    }
}

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

s32 native_mutex_lock_timed(u64 handle, u64 abs_deadline_ns) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    struct native_object *o;
    u64 me;
    u32 first = 1;
    u32 need_rights;
    if (!nc_deadline_valid(abs_deadline_ns)) return NC_INVAL;
    need_rights = abs_deadline_ns != 0 ? (u32)(NC_MLOCK | NC_MWAIT) : (u32)NC_MLOCK;
    pinned = nc_pin_handle(handle, need_rights, NC_MUTEX);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];
    me = sched_current_owner_key();

    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    for (;;) {
        if (!nc_handle_still_live(handle)) { rc = NC_BAD_HANDLE; break; }
        if (o->m_state == M_NOTREC) { rc = NC_NOT_RECOVERABLE; break; }
        if (o->m_state == M_FREE) {
            o->m_owner = me;
            o->m_state = M_LOCKED;
            o->m_recover = 0;
            rc = 0;
            break;
        }
        if (o->m_state == M_DIED) {
            o->m_owner = me;
            o->m_state = M_LOCKED;
            o->m_recover = 1;
            rc = NC_OWNER_DIED;
            break;
        }
        if (o->m_owner == me) {
            if (first) { rc = NC_DEADLK; break; }
            rc = 0;
            break;
        }
        if (abs_deadline_ns == 0) { rc = NC_AGAIN; break; }
        if (abs_deadline_ns != ~0ULL && sched_monotonic_ns() >= abs_deadline_ns) {
            rc = NC_TIMEDOUT;
            break;
        }
        if (o->m_waiters >= MQ_CAP) { rc = NC_AGAIN; break; }
        {
            u32 slot = nm_slot_alloc(o, me);
            u64 wchan;
            u32 pr;
            if (slot >= MQ_CAP) { rc = NC_AGAIN; break; }
            wchan = o->m_chan[slot];
            sched_park_prepare_deadline(wchan, abs_deadline_ns);
            o->m_queue[(o->m_qhead + o->m_waiters) % MQ_CAP] = (u64)slot;
            o->m_waiters = o->m_waiters + 1;
            o->m_enqueues = o->m_enqueues + 1;
            pr = sched_park_commit_unlock_spin(wchan, &nc_obj_locks[oi], 0);
            spin_lock(&nc_obj_locks[oi]);
            if (pr == PARK_INTR_RESULT) {
                if (o->m_owner == me && o->m_state == M_LOCKED) {
                    o->m_owner = 0;
                    o->m_state = M_FREE;
                    o->m_recover = 0;
                    nm_wake_all(o);
                    nc_wake_wait_any_locked(o);
                    rc = NC_INTR;
                    break;
                }
                nm_queue_remove(o, me);
                rc = NC_INTR;
                break;
            }
            first = 0;
            /* Direct handoff wins over timeout if unlock already transferred! */
            if (o->m_owner == me && o->m_state == M_LOCKED) {
                rc = 0;
                break;
            }
            nm_queue_remove(o, me);
            if (pr == PARK_TIMEOUT_RESULT) {
                rc = NC_TIMEDOUT;
                break;
            }
        }
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

s32 native_mutex_lock_intr(u64 handle, u32 block) {
    if (block > 1) return NC_INVAL;
    /* Note: kernel-side callers pass handles carrying MLOCK|MWAIT; user
     * trylock (block==0) via natcap checks MLOCK before calling here. */
    return native_mutex_lock_timed(handle, block ? ~0ULL : 0ULL);
}

s32 native_mutex_unlock(u64 handle) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    struct native_object *o;
    u64 me;
    pinned = nc_pin_handle(handle, NC_MUNLOCK, NC_MUTEX);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];
    me = sched_current_owner_key();

    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    if (!nc_handle_still_live(handle)) {
        rc = NC_BAD_HANDLE;
    } else if (o->m_state != M_LOCKED || o->m_owner != me) {
        rc = NC_RIGHTS;
    } else if (o->m_recover) {
        o->m_state = M_NOTREC;
        o->m_owner = 0;
        o->m_recover = 0;
        nm_wake_all(o);
        nc_wake_wait_any_locked(o);
        rc = 0;
    } else if (o->m_waiters) {
        u32 slot = (u32)o->m_queue[o->m_qhead];
        u64 next = o->m_tkey[slot];
        o->m_qhead = (o->m_qhead + 1) % MQ_CAP;
        o->m_waiters = o->m_waiters - 1;
        o->m_owner = next;
        o->m_tkey[slot] = 0;
        sched_wakeup(o->m_chan[slot]);
        rc = 0;
    } else {
        o->m_owner = 0;
        o->m_state = M_FREE;
        o->m_recover = 0;
        nc_wake_wait_any_locked(o);
        rc = 0;
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

s32 native_mutex_consistent(u64 handle) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    struct native_object *o;
    u64 me;
    pinned = nc_pin_handle(handle, NC_MUNLOCK, NC_MUTEX);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];
    me = sched_current_owner_key();

    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    if (!nc_handle_still_live(handle)) {
        rc = NC_BAD_HANDLE;
    } else if (o->m_state == M_NOTREC) {
        rc = NC_NOT_RECOVERABLE;
    } else if (o->m_state == M_LOCKED && o->m_owner == me && o->m_recover) {
        o->m_recover = 0;
        rc = 0;
    } else {
        rc = NC_INVAL;
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

s32 native_mutex_stat(u64 handle, u32 *out) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    struct native_object *o;
    if (out == 0) return NC_INVAL;
    pinned = nc_pin_handle(handle, NC_MWAIT, NC_MUTEX);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];

    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    if (!nc_handle_still_live(handle)) {
        rc = NC_BAD_HANDLE;
    } else {
        out[0] = o->m_state;
        out[1] = o->m_waiters;
        out[2] = (u32)(o->m_owner >> 32);
        out[3] = o->m_enqueues;
        rc = 0;
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

void native_mutex_owner_died_key(u64 key) {
    u64 flags;
    u32 oi;
    flags = cpu_irq_save();
    for (oi = 0; oi < NC_OBJECTS; oi++) {
        struct native_object *o = &nc_objects[oi];
        if (o->kind != NC_MUTEX) continue;
        spin_lock(&nc_obj_locks[oi]);
        if (o->kind == NC_MUTEX && o->m_state == M_LOCKED && o->m_owner == key) {
            o->m_state = M_DIED;
            o->m_owner = 0;
            nm_wake_all(o);
            nc_wake_wait_any_locked(o);
            spin_lock(&nc_reg_lock);
            nc_reclaim_reg_locked(oi);
            spin_unlock(&nc_reg_lock);
        }
        spin_unlock(&nc_obj_locks[oi]);
    }
    cpu_irq_restore(flags);
}

/* ---------------------------------------------------------------------------
 * Typed Task / Process completion objects (NC_TASK / NC_PROC, arch section 3).
 * Completion metadata lives here independently of TCB / kernel-stack reclaim.
 * ------------------------------------------------------------------------- */
s32 native_exec_obj_create(u32 kind, u64 target_id, u64 *out_nh) {
    u64 flags;
    u32 oi;
    s32 rc = NC_FULL;
    if (out_nh == 0 || (kind != NC_TASK && kind != NC_PROC)) return NC_INVAL;
    *out_nh = 0;
    flags = cpu_irq_save();
    spin_lock(&nc_reg_lock);
    for (oi = 0; oi < NC_OBJECTS; oi++) {
        struct native_object *o = &nc_objects[oi];
        if (o->kind) continue;
        nc_zero_object_common(o, kind);
        o->exec_active = 1;
        o->exec_done = 0;
        o->exec_target = target_id;
        o->exec_result = 0;
        rc = nc_alloc_handle_locked(
            oi, NC_JOIN | NC_CANCEL | NC_INSPECT | NC_TRANSFER, out_nh);
        if (rc != 0) {
            o->exec_active = 0;
            o->kind = 0;
        }
        break;
    }
    spin_unlock(&nc_reg_lock);
    cpu_irq_restore(flags);
    return rc;
}

void native_exec_obj_complete(u32 kind, u64 target_id, u64 exit_val) {
    u64 flags;
    u32 oi;
    if (target_id == 0) return;
    flags = cpu_irq_save();
    for (oi = 0; oi < NC_OBJECTS; oi++) {
        struct native_object *o = &nc_objects[oi];
        if (o->kind != kind) continue;
        spin_lock(&nc_obj_locks[oi]);
        if (o->kind == kind && o->exec_active && o->exec_target == target_id) {
            o->exec_result = exit_val;
            o->exec_done = 1;
            o->exec_active = 0;
            sched_wakeup((u64)&o->recv_wait);
            nc_wake_wait_any_locked(o);
            spin_lock(&nc_reg_lock);
            nc_reclaim_reg_locked(oi);
            spin_unlock(&nc_reg_lock);
        }
        spin_unlock(&nc_obj_locks[oi]);
    }
    cpu_irq_restore(flags);
}

s32 native_exec_obj_wait(u64 handle, u64 *result_out, u64 abs_deadline_ns) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    u32 timed_flag = 0;
    struct native_object *o;
    if (result_out == 0 || !nc_deadline_valid(abs_deadline_ns)) return NC_INVAL;
    pinned = nc_pin_handle(handle, NC_JOIN, 0);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];
    if (o->kind != NC_TASK && o->kind != NC_PROC) {
        nc_unpin_object(oi);
        return NC_TYPE;
    }
    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    for (;;) {
        if (!nc_handle_still_live(handle)) { rc = NC_BAD_HANDLE; break; }
        if (o->exec_done) {
            *result_out = o->exec_result;
            rc = 0;
            break;
        }
        if (abs_deadline_ns == 0) { rc = NC_AGAIN; break; }
        if (timed_flag) { rc = NC_TIMEDOUT; break; }
        {
            u64 wchan = (u64)&o->recv_wait;
            u32 pr;
            sched_park_prepare_deadline(wchan, abs_deadline_ns);
            pr = sched_park_commit_unlock_spin(wchan, &nc_obj_locks[oi], 0);
            spin_lock(&nc_obj_locks[oi]);
            if (pr == PARK_INTR_RESULT) { rc = NC_INTR; break; }
            if (pr == PARK_TIMEOUT_RESULT) { timed_flag = 1; }
        }
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

/* Returns 0 when still active (*target_out filled), NC_EOF (1) when already
 * completed, or negative NC_* error. */
s32 native_exec_obj_target(u64 handle, u32 want_kind, u32 want_rights, u64 *target_out) {
    u64 flags;
    s32 pinned;
    u32 oi;
    s32 rc;
    struct native_object *o;
    if (target_out == 0) return NC_INVAL;
    pinned = nc_pin_handle(handle, want_rights, want_kind);
    if (pinned < 0) return pinned;
    oi = (u32)pinned;
    o = &nc_objects[oi];
    flags = cpu_irq_save();
    spin_lock(&nc_obj_locks[oi]);
    if (!nc_handle_still_live(handle)) {
        rc = NC_BAD_HANDLE;
    } else if (o->exec_done) {
        *target_out = o->exec_target;
        rc = NC_EOF;
    } else {
        *target_out = o->exec_target;
        rc = 0;
    }
    spin_lock(&nc_reg_lock);
    o->pins = o->pins - 1;
    nc_reclaim_reg_locked(oi);
    spin_unlock(&nc_reg_lock);
    spin_unlock(&nc_obj_locks[oi]);
    cpu_irq_restore(flags);
    return rc;
}

/* ---------------------------------------------------------------------------
 * Native multi-object wait_any (architecture section 5, gate 3):
 *   - Locks unique objects in strictly ascending oi order (deadlock-free)
 *   - Rotating scan cursor prevents low-index starvation
 *   - One shared wait token registered across all watched objects
 *   - Monotonic absolute deadline + interruptible park-admission
 *   - Removes all registrations before returning
 * ------------------------------------------------------------------------- */
static u32 nc_eval_wait_item(struct native_object *o, u32 events) {
    u32 rev = 0;
    if (o->kind == NC_CHANNEL) {
        if (events & NATW_RECV) {
            if (o->count > 0) rev = rev | NATW_RECV;
            else if (o->writers == 0) rev = rev | NATW_RECV | NATW_EOF;
        }
        if (events & NATW_SEND) {
            if (o->readers == 0) rev = rev | NATW_SEND | NATW_CLOSED;
            else if (o->count < NC_CAPACITY) rev = rev | NATW_SEND;
        }
    } else if (o->kind == NC_MUTEX) {
        if (events & NATW_MUTEX) {
            if (o->m_state == M_FREE || o->m_state == M_DIED
                || o->m_state == M_NOTREC) {
                rev = rev | NATW_MUTEX;
            }
        }
    } else if (o->kind == NC_TASK || o->kind == NC_PROC) {
        if ((events & NATW_DONE) != 0 && o->exec_done) {
            rev = rev | NATW_DONE;
        }
    }
    return rev;
}

static u32 nc_wait_reg_add(struct native_object *o, u64 wchan) {
    u32 s;
    for (s = 0; s < NW_SLOTS; s++) {
        if (o->wait_any_chans[s] == wchan) return 1;
    }
    for (s = 0; s < NW_SLOTS; s++) {
        if (o->wait_any_chans[s] == 0) {
            o->wait_any_chans[s] = wchan;
            return 1;
        }
    }
    return 0;
}

static void nc_wait_reg_del(struct native_object *o, u64 wchan) {
    u32 s;
    for (s = 0; s < NW_SLOTS; s++) {
        if (o->wait_any_chans[s] == wchan) {
            o->wait_any_chans[s] = 0;
        }
    }
}

s32 native_wait_any(struct nc_wait_entry *entries, u32 count,
                    u64 abs_deadline_ns, u32 *win_out) {
    u32 item_oi[16];
    u32 sorted_oi[16];
    u32 n_unique = 0;
    u64 local_token = 0;
    u64 wchan = (u64)&local_token;
    u32 i;
    u32 u;
    u32 cpu;
    u32 start;
    u32 timed_flag = 0;
    s32 rc = 0;

    if (entries == 0 || win_out == 0 || count == 0 || count > 16
        || !nc_deadline_valid(abs_deadline_ns)) {
        return NC_INVAL;
    }

    /* Pin all referenced objects and validate event/right combinations. */
    for (i = 0; i < count; i++) {
        u32 ev = entries[i].events;
        u32 want_r = 0;
        s32 pinned;
        entries[i].revents = 0;
        if (ev == 0 || (ev & ~(u32)(NATW_RECV | NATW_SEND | NATW_MUTEX | NATW_DONE)) != 0) {
            while (i > 0) { i = i - 1; nc_unpin_object(item_oi[i]); }
            return NC_INVAL;
        }
        if (ev & NATW_RECV) want_r = want_r | NC_RECV;
        if (ev & NATW_SEND) want_r = want_r | NC_SEND;
        if (ev & NATW_MUTEX) want_r = want_r | NC_MWAIT;
        if (ev & NATW_DONE) want_r = want_r | NC_JOIN;
        pinned = nc_pin_handle(entries[i].nh, want_r, 0);
        if (pinned < 0) {
            while (i > 0) { i = i - 1; nc_unpin_object(item_oi[i]); }
            return pinned;
        }
        item_oi[i] = (u32)pinned;
        {
            u32 knd = nc_objects[item_oi[i]].kind;
            u32 bad_kind = 0;
            if ((ev & (NATW_RECV | NATW_SEND)) != 0 && knd != NC_CHANNEL) bad_kind = 1;
            if ((ev & NATW_MUTEX) != 0 && knd != NC_MUTEX) bad_kind = 1;
            if ((ev & NATW_DONE) != 0 && knd != NC_TASK && knd != NC_PROC) bad_kind = 1;
            if (bad_kind) {
                nc_unpin_object(item_oi[i]);
                while (i > 0) { i = i - 1; nc_unpin_object(item_oi[i]); }
                return NC_TYPE;
            }
        }
        /* Insert into sorted unique object list. */
        {
            u32 dup = 0;
            u32 pos = 0;
            for (u = 0; u < n_unique; u++) {
                if (sorted_oi[u] == item_oi[i]) { dup = 1; break; }
                if (sorted_oi[u] < item_oi[i]) pos = u + 1;
            }
            if (!dup) {
                u32 m = n_unique;
                while (m > pos) {
                    sorted_oi[m] = sorted_oi[m - 1];
                    m = m - 1;
                }
                sorted_oi[pos] = item_oi[i];
                n_unique = n_unique + 1;
            }
        }
    }

    cpu = smp_cpuid();
    if (cpu >= 8) cpu = 0;
    start = nc_wait_rotor[cpu] % count;
    nc_wait_rotor[cpu] = nc_wait_rotor[cpu] + 1;

    for (;;) {
        u64 flags = cpu_irq_save();
        u32 found = 0;
        u32 reg_ok = 1;
        u32 pr;

        /* Lock all distinct objects in ascending index order. */
        for (u = 0; u < n_unique; u++) {
            spin_lock(&nc_obj_locks[sorted_oi[u]]);
        }
        for (i = 0; i < count; i++) {
            u32 idx = (start + i) % count;
            u32 rev;
            if (!nc_handle_still_live(entries[idx].nh)) {
                rc = NC_BAD_HANDLE;
                found = 1;
                break;
            }
            rev = nc_eval_wait_item(&nc_objects[item_oi[idx]], entries[idx].events);
            if (rev != 0) {
                entries[idx].revents = rev;
                *win_out = idx;
                rc = 0;
                found = 1;
                break;
            }
        }
        if (found) {
            for (u = n_unique; u > 0; u--) {
                spin_unlock(&nc_obj_locks[sorted_oi[u - 1]]);
            }
            cpu_irq_restore(flags);
            break;
        }
        if (abs_deadline_ns == 0) {
            for (u = n_unique; u > 0; u--) {
                spin_unlock(&nc_obj_locks[sorted_oi[u - 1]]);
            }
            cpu_irq_restore(flags);
            rc = NC_AGAIN;
            break;
        }
        if (timed_flag) {
            for (u = n_unique; u > 0; u--) {
                spin_unlock(&nc_obj_locks[sorted_oi[u - 1]]);
            }
            cpu_irq_restore(flags);
            rc = NC_TIMEDOUT;
            break;
        }

        /* Arm shared wait token across all objects while all obj_locks are held. */
        for (u = 0; u < n_unique; u++) {
            if (!nc_wait_reg_add(&nc_objects[sorted_oi[u]], wchan)) {
                reg_ok = 0;
                break;
            }
        }
        if (!reg_ok) {
            for (u = 0; u < n_unique; u++) {
                nc_wait_reg_del(&nc_objects[sorted_oi[u]], wchan);
            }
            for (u = n_unique; u > 0; u--) {
                spin_unlock(&nc_obj_locks[sorted_oi[u - 1]]);
            }
            cpu_irq_restore(flags);
            rc = NC_FULL;
            break;
        }
        sched_park_prepare_deadline(wchan, abs_deadline_ns);
        for (u = n_unique; u > 0; u--) {
            spin_unlock(&nc_obj_locks[sorted_oi[u - 1]]);
        }
        cpu_irq_restore(flags);

        pr = sched_park_commit(wchan, 0);

        /* Deregister from all objects before inspecting results or returning. */
        flags = cpu_irq_save();
        for (u = 0; u < n_unique; u++) {
            spin_lock(&nc_obj_locks[sorted_oi[u]]);
        }
        for (u = 0; u < n_unique; u++) {
            nc_wait_reg_del(&nc_objects[sorted_oi[u]], wchan);
        }
        for (u = n_unique; u > 0; u--) {
            spin_unlock(&nc_obj_locks[sorted_oi[u - 1]]);
        }
        cpu_irq_restore(flags);

        if (pr == PARK_INTR_RESULT) {
            rc = NC_INTR;
            break;
        }
        if (pr == PARK_TIMEOUT_RESULT) {
            timed_flag = 1;
        }
    }

    for (i = 0; i < count; i++) {
        nc_unpin_object(item_oi[i]);
    }
    return rc;
}

/* ---------------------------------------------------------------------------
 * Boot regression suite: kernel-only objects, sharded locks, deadlines,
 * rich byte+cap messages, and multi-object wait_any.
 * ------------------------------------------------------------------------- */
extern void kprintf(const char *fmt, ...);
extern u32 kthread_create(u64 entry, u64 arg);
extern void kthread_exit(void);
extern void sched_yield(void);
extern u64 sched_clock_ticks(void);
extern u32 sched_thread_state(u32 id);
extern u32 smp_cpu_count(void);
extern u32 sched_cfs_rb_check(void);

static volatile u32 nc_test_lock;
static u64 nc_test_handles[4];
static u64 nc_selftest_handles[NC_HANDLES];
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

static u32 nc_start_barrier(void) {
    u64 until = sched_clock_ticks() + 800;
    u64 flags = cpu_irq_save();
    spin_lock(&nc_test_lock);
    nc_test_ready = nc_test_ready + 1;
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
    if (cpu < 8) nc_test_send_hits[cpu] = nc_test_send_hits[cpu] + 1;
    spin_unlock(&nc_test_lock);
    cpu_irq_restore(flags);
}
static void nc_record(u64 value, u32 done, u32 error) {
    u64 flags = cpu_irq_save();
    u32 cpu = smp_cpuid();
    spin_lock(&nc_test_lock);
    if (value) {
        nc_test_mask |= 1u << cpu;
        if (cpu < 8) nc_test_recv_hits[cpu] = nc_test_recv_hits[cpu] + 1;
        if (value > 512 || nc_test_seen[value - 1]) nc_test_errors = nc_test_errors + 1;
        else { nc_test_seen[value - 1] = 1; nc_test_count = nc_test_count + 1; }
    }
    nc_test_done = nc_test_done + done;
    nc_test_errors = nc_test_errors + error;
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
    spin_lock(&nc_reg_lock);
    for (i = 0; i < NC_OBJECTS; i++) if (nc_objects[i].kind) empty = 0;
    for (i = 0; i < NC_HANDLES; i++) if (nc_handles[i].live) empty = 0;
    spin_unlock(&nc_reg_lock);
    cpu_irq_restore(flags);
    return empty;
}
static u32 nc_expect(u32 condition, u32 tag) {
    if (!condition) kprintf("[NATIVE] expectation %u failed\n", (u64)tag);
    return condition;
}

static u64 nm_handle;
static volatile u32 nm_stage;
static volatile u32 nm_rc[3];
static volatile u32 nm_order[3];
static volatile u32 nm_order_n;
static u32 nm_stat[4];
static u64 nm_wchan[3];

static u32 nm_enqueue_count(void) {
    u32 st[4];
    if (native_mutex_stat(nm_handle, st) != 0) {
        return 0;
    }
    return st[3];
}

static void nm_wait_enqueues(u32 n, u32 timeout_ticks) {
    u64 until = sched_clock_ticks() + timeout_ticks;
    while (nm_enqueue_count() < n && sched_clock_ticks() < until) {
        sched_yield();
    }
}

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
            sched_park_abort();
            return;
        }
        (void)sched_park_commit((u64)&nm_wchan[id], 0);
    }
}

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

static void nm_close_owner(u64 arg) {
    (void)arg;
    nm_rc[0] = (u32)native_mutex_lock_intr(nm_handle, 1);
    nm_stage = 1;
    native_handle_close(nm_handle);
    kthread_exit();
}

void native_object_selftest(void) {
    u64 tx, rx, dup, value, old;
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
        ok &= nc_expect((native_channel_create(&nc_selftest_handles[2 * i],
                                               &nc_selftest_handles[2 * i + 1]) == 0), 17);
    ok &= nc_expect((native_channel_create(&dup, &value) == NC_FULL), 18);
    for (i = 0; i < 2 * NC_OBJECTS; i++)
        ok &= nc_expect((native_handle_close(nc_selftest_handles[i]) == 0), 19);
    ok &= nc_expect((native_handle_close(0) == NC_BAD_HANDLE), 20);
    ok &= nc_expect((native_channel_create(&tx, &tx) == NC_INVAL), 21);
    ok &= nc_expect((native_channel_recv(old, 0, 0) == NC_INVAL), 22);
    /* Exhaust handles with duplicates; failed pair allocation must roll back
     * its first handle, and the object must not leak. Leave exactly one free. */
    if (native_channel_create(&tx, &rx) != 0) goto fail;
    n = 0;
    while (n < NC_HANDLES - 3 && native_handle_dup(tx, NC_SEND, &nc_selftest_handles[n]) == 0) n++;
    ok &= nc_expect((n == NC_HANDLES - 3), 23);
    ok &= nc_expect((native_channel_create(&dup, &value) == NC_FULL), 24);
    for (i = 0; i < n; i++) ok &= nc_expect((native_handle_close(nc_selftest_handles[i]) == 0), 25);
    ok &= nc_expect((native_handle_close(tx) == 0), 26);
    ok &= nc_expect((native_handle_close(rx) == 0), 27);
    ok &= nc_expect((nc_empty()), 28);
    if (!ok) goto fail;
    kprintf("      [PASS] native handles: rights/stale/FIFO/full/EOF/peer-close/rollback/no-leak\n");

    if (native_channel_create(&tx, &rx) != 0) goto fail;
    nc_test_done = 0; nc_test_revoked = 0;
    wid = kthread_create((u64)&nc_revoked_worker, rx);
    if (wid == 0xFFFFFFFFu) goto fail;
    until = sched_clock_ticks() + 800;
    while (sched_thread_state(wid) != 3 && sched_clock_ticks() < until) sched_yield();
    ok &= nc_expect((sched_thread_state(wid) == 3), 29);
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
    ok &= nc_expect((native_mutex_unlock(tx) == 0), 87);
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

    /* ---- Deadlines, rich byte/cap messages, and wait_any select ---- */
    {
        u64 s1, r1, s2, r2, cs, cr;
        u8 sbuf[32];
        u8 rbuf[32];
        struct nc_msg_req mreq;
        struct nc_wait_entry went[3];
        u32 win = 99;
        u64 now_ns;
        ok &= nc_expect((native_channel_create(&s1, &r1) == 0), 120);
        ok &= nc_expect((native_channel_create(&s2, &r2) == 0), 121);
        ok &= nc_expect((native_channel_create(&cs, &cr) == 0), 122);
        /* Overflow deadline rejected; finite deadline times out cleanly. */
        ok &= nc_expect((native_channel_recv_timed(r1, &value, 0x8000000000000000ULL) == NC_INVAL), 123);
        now_ns = sched_monotonic_ns();
        ok &= nc_expect((native_channel_recv_timed(r1, &value, now_ns + 20000000ULL) == NC_TIMEDOUT), 124);
        /* Bounded byte message + attached handle, plus discard on close. */
        for (i = 0; i < 20; i++) sbuf[i] = (u8)(0x30 + i);
        mreq.bytes = sbuf; mreq.byte_len = 20; mreq.ncaps = 1;
        mreq.cap_handles[0] = cs; mreq.cap_rights[0] = NC_SEND;
        mreq.caller_pid = 0;
        ok &= nc_expect((native_chan_send_msg(s2, &mreq, 0) == 0), 125);
        /* Multi-object wait_any: r1 empty, r2 ready -> index 1 wins, then fairness rotation. */
        went[0].nh = r1; went[0].events = NATW_RECV; went[0].revents = 0;
        went[1].nh = r2; went[1].events = NATW_RECV; went[1].revents = 0;
        ok &= nc_expect((native_wait_any(went, 2, ~0ULL, &win) == 0 && win == 1
                         && went[1].revents == NATW_RECV), 126);
        /* Both ready -> rotating cursor visits both 0 and 1 across two calls. */
        ok &= nc_expect((native_channel_send(s1, 0xCAFEULL, 0) == 0), 127);
        {
            u32 w_a = 99, w_b = 99;
            went[0].nh = r1; went[0].events = NATW_RECV;
            went[1].nh = r2; went[1].events = NATW_RECV;
            ok &= nc_expect((native_wait_any(went, 2, 0, &w_a) == 0), 128);
            went[0].nh = r1; went[0].events = NATW_RECV;
            went[1].nh = r2; went[1].events = NATW_RECV;
            ok &= nc_expect((native_wait_any(went, 2, 0, &w_b) == 0), 129);
            ok &= nc_expect((w_a != w_b), 130);
        }
        ok &= nc_expect((native_channel_recv(r1, &value, 0) == 0 && value == 0xCAFEULL), 131);
        /* Short buffer peek does NOT consume the message. */
        mreq.bytes = rbuf; mreq.byte_len = 10; mreq.ncaps = 1; mreq.caller_pid = 0;
        ok &= nc_expect((native_chan_recv_msg(r2, &mreq, 0) == NC_MSGSIZE), 132);
        mreq.bytes = rbuf; mreq.byte_len = 32; mreq.ncaps = 4; mreq.caller_pid = 0;
        ok &= nc_expect((native_chan_recv_msg(r2, &mreq, 0) == 0
                         && mreq.byte_len == 20 && mreq.ncaps == 1), 133);
        for (i = 0; i < 20; i++) ok &= nc_expect((rbuf[i] == (u8)(0x30 + i)), 134);
        /* Closing r2 while a capability is queued discards that capability cleanly. */
        mreq.bytes = sbuf; mreq.byte_len = 4; mreq.ncaps = 1;
        mreq.cap_handles[0] = mreq.cap_handles[0]; /* re-queue received cs */
        mreq.cap_rights[0] = NC_SEND; mreq.caller_pid = 0;
        ok &= nc_expect((native_chan_send_msg(s2, &mreq, 0) == 0), 135);
        ok &= nc_expect((native_handle_close(r2) == 0), 136);
        /* With cs discarded by r2's close, cr sees EOF immediately! */
        ok &= nc_expect((native_channel_recv(cr, &value, 0) == NC_EOF), 137);
        ok &= nc_expect((native_handle_close(cr) == 0), 138);
        ok &= nc_expect((native_handle_close(s2) == 0), 139);
        ok &= nc_expect((native_handle_close(s1) == 0), 140);
        ok &= nc_expect((native_handle_close(r1) == 0), 141);
        if (!ok) goto fail;
        kprintf("      [PASS] native sharded locks, deadlines, byte+cap messages, wait_any fairness\n");
    }

    ok &= nc_expect((nc_empty()), 116);
    if (!ok) goto fail;
    kprintf("[NATIVE] PASS kernel-only object/channel suite\n");
    return;
 fail:
    kprintf("[NATIVE] FAIL kernel-only object/channel suite (done=%u errors=%u count=%u mask=0x%x)\n",
            (u64)nc_test_done, (u64)nc_test_errors, (u64)nc_test_count, (u64)nc_test_mask);
}
