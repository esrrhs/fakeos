package kernel;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t s32;
typedef types.int64_t s64;

/* Process-relative native capability namespace (migration gate 2).
 *
 * A user handle is (generation << 16) | (slot + 1) into the owning process's
 * table. It is never a global object ID, fd, or a value accepted from another
 * process's namespace: a forged or foreign handle either misses this table
 * (EBADF) or resolves to a different object the caller legitimately owns.
 * Each entry owns exactly ONE kernel-native handle (installed via
 * native_handle_dup on inherit/dup), so closing a capability closes that
 * handle; independent duplicates delay the final endpoint close. Native
 * handle values never cross back to user space.
 *
 * STORAGE: the table lives in the process owner record (arch section 3:
 * process { native_cap_table }), so all tasks of a process share it by
 * identity, not by pid integer — PIDs never wrap/recycle, owner slots do.
 * Generations never wrap back into reuse: close, fork-rollback and process
 * exit all bump a slot's generation (capping at NAT_GEN_MAX, which
 * permanently retires the slot), so a stale value can never validate again.
 * The residual foreign-guessing surface is comparable to POSIX fd numbers
 * and is a documented bounded-slice limitation, not a claimed secure ABI.
 *
 * LOCKING: sched_nat_acquire pins the owner table under sched_run_lock;
 * native.c operations (nc_lock, may sleep) run strictly OUTSIDE it, so the
 * lock order stays nat/nc paths: nc_lock -> run_lock never inverted. Every
 * table mutation is a bounded non-sleeping section; blocking send/recv
 * re-validate the copied native handle under nc_lock, so a concurrent close
 * resolves to NC_BAD_HANDLE there. Park admission follows the
 * sched_park_prepare/commit contract: peer producers and endpoint closes
 * hold nc_lock, signals hold run_lock, and commit refuses to publish sleep
 * when fatal cancellation is pending.
 *
 * LIFECYCLE: fork installs independent native-handle duplicates into the
 * child table inside sched_clone_user BEFORE the child is published
 * (all-or-nothing; failure rolls the whole child back). The final task exit
 * closes every remaining capability. Capabilities survive exec: they belong
 * to the process, not the image. No capability-in-message transfer yet;
 * endpoints reach children only through fork inheritance (documented).
 *
 * Bounds: NAT_CAPS per process; native.c pools bound globally (ENFILE).
 *
 * See docs/native-kernel-architecture.md sections 3-4 and
 * docs/evidence/native-caps-20261121/RESULT.md. */

/* MUST match sched.c struct nat_cap and NAT_CAPS/NAT_GEN_MAX exactly. */
struct nat_cap {
    u32 gen;                                /* 0 = virgin; NAT_GEN_MAX = retired */
    u32 rights;                             /* NATC_SEND / NATC_RECV / mutex bits */
    u64 handle;                             /* kernel-native handle (private) */
};
enum {
    NATC_SEND = 1, NATC_RECV = 2,           /* same bit values as NC_SEND/NC_RECV */
    NATC_MLOCK = 4, NATC_MUNLOCK = 8, NATC_MWAIT = 16,
    NAT_CAPS = 8,                           /* per-process bound */
    NAT_GEN_MAX = 0xFFFFu,                  /* exhausted generation retires slot */

    NATC_BAD_HANDLE = -9,                   /* EBADF */
    NATC_RIGHTS     = -1,                   /* EPERM */
    NATC_FULL       = -23,                  /* ENFILE */
    NATC_AGAIN      = -11,                  /* EAGAIN */
    NATC_CLOSED     = -32,                  /* EPIPE */
    NATC_EOF        = 0,                    /* recv: drained, no live sender */
    NATC_INVAL      = -22,                  /* EINVAL */
    NATC_INTR       = -4,                   /* EINTR */
    NATC_DEADLK     = -35,                  /* EDEADLK: self-lock */
    NATC_OWNER_DIED = -130,                 /* EOWNERDIED: robust recovery needed */
    NATC_NOT_RECOVERABLE = -131             /* ENOTRECOVERABLE: poisoned */
};

extern u32 sched_current_pid(void);
extern u64 sched_current_as_h(void);
extern void kprintf(const char *fmt, ...);
extern u32 sched_nat_acquire(u32 pid, struct nat_cap **caps_out, u32 **used_out);
extern void sched_nat_release(u32 token);
extern s32 native_channel_create(u64 *send, u64 *recv);
extern s32 native_handle_dup(u64 handle, u32 rights, u64 *out);
extern s32 native_handle_close(u64 handle);
extern s32 native_channel_send_intr(u64 handle, u64 value, u32 block);
extern s32 native_channel_recv_intr(u64 handle, u64 *value, u32 block);
extern s32 native_mutex_create(u64 *out);
extern s32 native_mutex_lock_intr(u64 handle, u32 block);
extern s32 native_mutex_unlock(u64 handle);
extern s32 native_mutex_consistent(u64 handle);
extern s32 native_mutex_stat(u64 handle, u32 *out);

/* Bump-with-retirement: the released generation can never validate again. */
static void nat_retire_gen(u32 *gen) {
    if (*gen < NAT_GEN_MAX) {
        *gen = *gen + 1;
    }
}

/* Installable iff not retired. Bumping on install invalidates every handle
 * that named this slot before, including across owner-slot reuse. */
static u64 nat_install(struct nat_cap *caps, u32 *used, u64 handle, u32 rights) {
    u32 i;
    for (i = 0; i < NAT_CAPS; i++) {
        struct nat_cap *c = &caps[i];
        /* Free = not live (install always sets a nonzero right; close, exit
         * and attach-zeroing always clear them) and not retired. Bumping on
         * install invalidates every handle that named this slot before. */
        if (c->rights == 0 && c->gen != NAT_GEN_MAX) {
            nat_retire_gen(&c->gen);
            c->rights = rights;
            c->handle = handle;
            *used = *used + 1;
            return ((u64)c->gen << 16) | (u64)(i + 1);
        }
    }
    return 0;
}

static void nat_free_slot(struct nat_cap *caps, u32 *used, u32 slot) {
    struct nat_cap *c = &caps[slot];
    c->rights = 0;
    c->handle = 0;
    nat_retire_gen(&c->gen);
    *used = *used - 1;
}

/* Copy-out lookup: validates (slot, gen) and returns the delegated native
 * handle. native.c re-validates that handle under nc_lock, so a concurrent
 * close resolves to NC_BAD_HANDLE there. */
static s64 nat_lookup(u32 pid, u64 cap, u32 want_rights) {
    u32 token;
    u32 slot;
    u32 gen;
    s64 rc;
    u32 *used;
    struct nat_cap *caps;
    slot = (u32)(cap & 0xFFFFu);
    gen = (u32)(cap >> 16);
    if (pid == 0) return NATC_INVAL;
    if (slot == 0 || slot > NAT_CAPS) return NATC_BAD_HANDLE;
    token = sched_nat_acquire(pid, &caps, &used);
    if (token == 0) return NATC_INVAL;
    {
        struct nat_cap *c = &caps[slot - 1];
        if (c->gen == 0 || c->gen == NAT_GEN_MAX || c->gen != gen) {
            rc = NATC_BAD_HANDLE;
        } else if ((c->rights & want_rights) != want_rights) {
            rc = NATC_RIGHTS;
        } else {
            rc = (s64)c->handle;
        }
    }
    sched_nat_release(token);
    return rc;
}

/* Map native.c result codes to the user ABI. */
static s64 nat_map(s32 rc) {
    if (rc == 0) return 0;
    if (rc == 1) return NATC_EOF;           /* NC_EOF */
    if (rc == -1) return NATC_BAD_HANDLE;   /* NC_BAD_HANDLE */
    if (rc == -2) return NATC_RIGHTS;       /* NC_RIGHTS */
    if (rc == -3) return NATC_FULL;         /* NC_FULL */
    if (rc == -4) return NATC_AGAIN;        /* NC_AGAIN */
    if (rc == -5) return NATC_CLOSED;       /* NC_CLOSED */
    if (rc == -8) return NATC_INTR;         /* NC_INTR */
    if (rc == -9) return NATC_DEADLK;       /* NC_DEADLK */
    if (rc == -10) return NATC_OWNER_DIED;  /* NC_OWNER_DIED */
    if (rc == -11) return NATC_NOT_RECOVERABLE;
    return NATC_INVAL;
}

/* nat_channel_create(send_out, recv_out): two endpoints, one channel.
 * Partial allocation rolls back before anything is published. */
s64 sys_nat_channel_create(u64 send_out, u64 recv_out) {
    u64 as;
    u64 nsend = 0;
    u64 nrecv = 0;
    u32 pid;
    s32 rc;
    u64 hsend = 0;
    u64 hrecv = 0;
    u32 token;
    u32 *used;
    struct nat_cap *caps;
    u64 sslot;
    u64 rslot;
    if ((send_out & 3ULL) != 0 || (recv_out & 3ULL) != 0) {
        return (u64)NATC_INVAL;
    }
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, send_out, 8)
        || !mem.as_user_range_ok(as, recv_out, 8)) {
        return (u64)NATC_INVAL;
    }
    pid = sched_current_pid();
    token = sched_nat_acquire(pid, &caps, &used);
    if (token == 0) return (u64)NATC_INVAL;
    if (*used + 2 > NAT_CAPS) {
        sched_nat_release(token);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);
    rc = native_channel_create(&nsend, &nrecv);
    if (rc != 0) return (u64)nat_map(rc);
    token = sched_nat_acquire(pid, &caps, &used);
    sslot = nat_install(caps, used, nsend, NATC_SEND);
    rslot = nat_install(caps, used, nrecv, NATC_RECV);
    if (sslot == 0 || rslot == 0) {
        if (sslot != 0) nat_free_slot(caps, used, (u32)(sslot & 0xFFFFu) - 1);
        if (rslot != 0) nat_free_slot(caps, used, (u32)(rslot & 0xFFFFu) - 1);
        sched_nat_release(token);
        native_handle_close(nsend);
        native_handle_close(nrecv);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);
    *(volatile u64 *)send_out = sslot;
    *(volatile u64 *)recv_out = rslot;
    return 0;
}

/* nat_send(cap, value, block): one copied u64 message; block 0 or 1.
 * The value is a scalar: never dereferenced, never followed as a pointer. */
s64 sys_nat_send(u64 cap, u64 value, u64 block) {
    s64 h;
    s32 rc;
    if (block > 1) return (u64)NATC_INVAL;
    h = nat_lookup(sched_current_pid(), cap, NATC_SEND);
    if (h < 0) return (u64)h;
    rc = native_channel_send_intr((u64)h, value, (u32)block);
    return (u64)nat_map(rc);
}

/* nat_recv(cap, value_out, block): returns 1 with the message copied out,
 * 0 (EOF) once drained with no live sender, or a negative errno. */
s64 sys_nat_recv(u64 cap, u64 value_out, u64 block) {
    u64 as;
    s64 h;
    s32 rc;
    u64 value = 0;
    if ((value_out & 3ULL) != 0) return (u64)NATC_INVAL;
    if (block > 1) return (u64)NATC_INVAL;
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, value_out, 8)) {
        return (u64)NATC_INVAL;
    }
    h = nat_lookup(sched_current_pid(), cap, NATC_RECV);
    if (h < 0) return (u64)h;
    rc = native_channel_recv_intr((u64)h, &value, (u32)block);
    if (rc == 0) {
        *(volatile u64 *)value_out = value;
        return 1;
    }
    return (u64)nat_map(rc);
}

/* nat_dup(cap, rights): in-process duplicate; rights must be a nonempty
 * subset of the source (attenuation only). Returns the new handle value. */
s64 sys_nat_dup(u64 cap, u64 rights) {
    s64 h;
    s32 rc;
    u64 nh = 0;
    u32 want;
    u32 pid;
    u32 token;
    u32 *used;
    struct nat_cap *caps;
    u64 slot;
    want = (u32)rights;
    if (want == 0
        || (want & ~(u32)(NATC_SEND | NATC_RECV | NATC_MLOCK | NATC_MUNLOCK
                          | NATC_MWAIT)) != 0) {
        return (u64)NATC_INVAL;
    }
    pid = sched_current_pid();
    h = nat_lookup(pid, cap, want);
    if (h < 0) return (u64)h;
    rc = native_handle_dup((u64)h, want, &nh);
    if (rc != 0) return (u64)nat_map(rc);
    token = sched_nat_acquire(pid, &caps, &used);
    slot = nat_install(caps, used, nh, want);
    if (slot == 0) {
        sched_nat_release(token);
        native_handle_close(nh);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);
    return (s64)slot;
}

/* nat_close(cap): closes the owned native handle (revoking blocked
 * operations on it) and frees the table slot for future installs. */
s64 sys_nat_close(u64 cap) {
    s64 h;
    u32 slot;
    u32 gen;
    u32 token;
    u32 *used;
    struct nat_cap *caps;
    u32 pid;
    s32 rc;
    pid = sched_current_pid();
    slot = (u32)(cap & 0xFFFFu);
    gen = (u32)(cap >> 16);
    if (pid == 0) return (u64)NATC_INVAL;
    if (slot == 0 || slot > NAT_CAPS) return (u64)NATC_BAD_HANDLE;
    token = sched_nat_acquire(pid, &caps, &used);
    if (token == 0) return (u64)NATC_INVAL;
    {
        struct nat_cap *c = &caps[slot - 1];
        if (c->gen == 0 || c->gen == NAT_GEN_MAX || c->gen != gen) {
            sched_nat_release(token);
            return (u64)NATC_BAD_HANDLE;
        }
        h = (s64)c->handle;
        nat_free_slot(caps, used, slot - 1);
    }
    sched_nat_release(token);
    rc = native_handle_close((u64)h);
    if (rc != 0) return (u64)nat_map(rc);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Native robust mutex syscalls (architecture section 6). The mutex object
 * lives in the same native core as channels; the capability table entry is
 * installed exactly like a channel endpoint, so fork inheritance, dup
 * attenuation, generation retirement and final-exit close all reuse the
 * existing machinery. Lock returns 0 or NATC_OWNER_DIED (-130) when the
 * previous owner died; the new owner must consistent() before a normal
 * unlock, or that unlock poisons the mutex (NATC_NOT_RECOVERABLE).
 * ------------------------------------------------------------------------- */
s64 sys_nat_mutex_create(u64 out) {
    u64 as;
    u64 nh = 0;
    u32 pid;
    u32 token;
    u32 *used;
    struct nat_cap *caps;
    u64 slot;
    if ((out & 3ULL) != 0) return (u64)NATC_INVAL;
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, out, 8)) return (u64)NATC_INVAL;
    pid = sched_current_pid();
    token = sched_nat_acquire(pid, &caps, &used);
    if (token == 0) return (u64)NATC_INVAL;
    if (*used + 1 > NAT_CAPS) {
        sched_nat_release(token);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);
    if (native_mutex_create(&nh) != 0) return (u64)NATC_FULL;
    token = sched_nat_acquire(pid, &caps, &used);
    slot = nat_install(caps, used, nh,
                       NATC_MLOCK | NATC_MUNLOCK | NATC_MWAIT);
    if (slot == 0) {
        sched_nat_release(token);
        native_handle_close(nh);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);
    *(volatile u64 *)out = slot;
    return 0;
}

/* nat_mutex_lock(cap, block): block 0 = try (NATC_AGAIN when held), 1 = park
 * under the wait-admission contract until handed, released, revoked or
 * cancelled. NATC_OWNER_DIED means acquired AS RECOVERY. */
s64 sys_nat_mutex_lock(u64 cap, u64 block) {
    s64 h;
    s32 rc;
    if (block > 1) return (u64)NATC_INVAL;
    h = nat_lookup(sched_current_pid(), cap,
                   block ? (u32)(NATC_MLOCK | NATC_MWAIT) : (u32)NATC_MLOCK);
    if (h < 0) return (u64)h;
    rc = native_mutex_lock_intr((u64)h, (u32)block);
    return (u64)nat_map(rc);
}

s64 sys_nat_mutex_unlock(u64 cap) {
    s64 h;
    s32 rc;
    h = nat_lookup(sched_current_pid(), cap, NATC_MUNLOCK);
    if (h < 0) return (u64)h;
    rc = native_mutex_unlock((u64)h);
    return (u64)nat_map(rc);
}

s64 sys_nat_mutex_consistent(u64 cap) {
    s64 h;
    s32 rc;
    h = nat_lookup(sched_current_pid(), cap, NATC_MUNLOCK);
    if (h < 0) return (u64)h;
    rc = native_mutex_consistent((u64)h);
    return (u64)nat_map(rc);
}

/* nat_mutex_stat(cap, out): out[0]=state out[1]=waiters out[2]=owner pid. */
s64 sys_nat_mutex_stat(u64 cap, u64 out) {
    u64 as;
    s64 h;
    s32 rc;
    u32 st[3];
    if ((out & 3ULL) != 0) return (u64)NATC_INVAL;
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, out, 12)) return (u64)NATC_INVAL;
    h = nat_lookup(sched_current_pid(), cap, NATC_MWAIT);
    if (h < 0) return (u64)h;
    rc = native_mutex_stat((u64)h, st);
    if (rc != 0) return (u64)nat_map(rc);
    *(volatile u32 *)out = st[0];
    *(volatile u32 *)(out + 4) = st[1];
    *(volatile u32 *)(out + 8) = st[2];
    return 0;
}

/* fork inheritance, called from sched_clone_user BEFORE the child is
 * published. Each live parent capability is re-homed AT THE SAME SLOT WITH
 * THE SAME HANDLE VALUE but backed by an independent native-handle
 * duplicate: user code in the child addresses inherited endpoints through
 * the values it copied at fork time, so slots and generations must carry
 * over verbatim; only the delegated kernel-native handle is new. The child
 * table is fresh (attach zeroed rights), so same-slot same-gen installs
 * cannot collide with anything the child could observe. All-or-nothing:
 * any failure closes every duplicate created so far and fails, and the
 * caller rolls the whole child back. Phase B runs without run_lock
 * (nc_lock only). */
s32 nat_fork_install(u32 parent, u32 child) {
    u32 i;
    s32 rc = 0;
    u32 created = 0;
    u64 handles[NAT_CAPS];
    u32 rights[NAT_CAPS];
    u32 gens[NAT_CAPS];
    u32 slots[NAT_CAPS];
    u64 dups[NAT_CAPS];
    u32 token;
    u32 *used;
    struct nat_cap *caps;
    if (parent == 0 || child == 0 || parent == child) return -1;
    token = sched_nat_acquire(parent, &caps, &used);
    if (token == 0) return -1;
    for (i = 0; i < NAT_CAPS; i++) {
        struct nat_cap *c = &caps[i];
        /* Live = the same definition nat_install uses for free (rights != 0);
         * a freed slot keeps a bumped generation but owns no handle. */
        if (c->rights == 0 || c->gen == 0 || c->gen == NAT_GEN_MAX) continue;
        handles[created] = c->handle;
        rights[created] = c->rights;
        gens[created] = c->gen;
        slots[created] = i;
        created = created + 1;
    }
    sched_nat_release(token);
    for (i = 0; i < created; i++) {
        u64 nh = 0;
        if (native_handle_dup(handles[i], rights[i], &nh) != 0) {
            rc = -1;
            break;
        }
        dups[i] = nh;
    }
    if (rc == 0) {
        token = sched_nat_acquire(child, &caps, &used);
        if (token == 0) rc = -1;
    }
    if (rc == 0) {
        for (i = 0; i < created; i++) {
            struct nat_cap *c = &caps[slots[i]];
            if (c->rights != 0) {           /* fresh table: never live */
                rc = -1;
                native_handle_close(dups[i]);
                break;
            }
            c->gen = gens[i];
            c->rights = rights[i];
            c->handle = dups[i];
            *used = *used + 1;
        }
        sched_nat_release(token);
        if (rc != 0) {
            /* Undo every installed duplicate; the child is rolled back
             * wholesale (never published), so nothing can observe them. */
            while (i > 0) {
                native_handle_close(dups[i - 1]);
                i = i - 1;
            }
        }
    } else {
        while (i > 0) {
            native_handle_close(dups[i - 1]);
            i = i - 1;
        }
    }
    return rc;
}

/* Final process teardown: the last task exit closes every remaining
 * capability and retires its generations. Phase 1 retires under run_lock,
 * phase 2 closes the copied native handles outside it (nc_lock only). */
void nat_process_exit(u32 pid) {
    u32 i;
    u32 token;
    u32 *used;
    struct nat_cap *caps;
    u64 closing[NAT_CAPS];
    u32 n = 0;
    token = sched_nat_acquire(pid, &caps, &used);
    if (token == 0) return;
    for (i = 0; i < NAT_CAPS; i++) {
        struct nat_cap *c = &caps[i];
        if (c->gen != 0 && c->gen != NAT_GEN_MAX) {
            closing[n] = c->handle;
            n = n + 1;
        }
        if (c->gen != 0) {
            nat_retire_gen(&c->gen);
            c->rights = 0;
            c->handle = 0;
        }
    }
    *used = 0;
    sched_nat_release(token);
    for (i = 0; i < n; i++) {
        native_handle_close(closing[i]);
    }
}
