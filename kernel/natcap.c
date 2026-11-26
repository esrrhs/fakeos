package kernel;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t  s32;
typedef types.int64_t  s64;

/* ---------------------------------------------------------------------------
 * Process-relative native capability namespace (architecture sections 3-6).
 *
 * A user handle is (generation << 16) | (slot + 1) into the owning process's
 * 32-slot capability table. Capabilities cover:
 *   - Native channels (scalar + bounded byte payload + DUP/MOVE cap transfer)
 *   - Native robust mutexes (with direct handoff + monotonic deadlines)
 *   - Native task & process handles (JOIN / CANCEL / INSPECT)
 *   - Multi-object wait_any multiplexing across capabilities
 * ------------------------------------------------------------------------- */

/* MUST match sched.c struct nat_cap and NAT_CAPS/NAT_GEN_MAX exactly. */
struct nat_cap {
    u32 gen;
    u32 rights;
    u64 handle;
};

enum {
    NATC_SEND     = 1,
    NATC_RECV     = 2,
    NATC_MLOCK    = 4,
    NATC_MUNLOCK  = 8,
    NATC_MWAIT    = 16,
    NATC_TRANSFER = 32,
    NATC_JOIN     = 64,
    NATC_CANCEL   = 128,
    NATC_INSPECT  = 256,
    NATC_ALL_RIGHTS = 511,

    NAT_CAPS    = 32,
    NAT_GEN_MAX = 0xFFFFu,

    NAT_MSG_MAX_BYTES = 256,
    NAT_MSG_MAX_CAPS  = 4,
    NAT_WAIT_MAX      = 16,

    NAT_XFER_DUP  = 0,
    NAT_XFER_MOVE = 1,

    NATW_RECV   = 1,
    NATW_SEND   = 2,
    NATW_MUTEX  = 4,
    NATW_DONE   = 8,

    NATC_BAD_HANDLE      = -9,    /* EBADF */
    NATC_RIGHTS          = -1,    /* EPERM */
    NATC_FULL            = -23,   /* ENFILE */
    NATC_AGAIN           = -11,   /* EAGAIN */
    NATC_CLOSED          = -32,   /* EPIPE */
    NATC_EOF             = 0,     /* recv: drained, no live sender */
    NATC_INVAL           = -22,   /* EINVAL */
    NATC_INTR            = -4,    /* EINTR */
    NATC_DEADLK          = -35,   /* EDEADLK */
    NATC_OWNER_DIED      = -130,  /* EOWNERDIED */
    NATC_NOT_RECOVERABLE = -131,  /* ENOTRECOVERABLE */
    NATC_TIMEDOUT        = -110,  /* ETIMEDOUT */
    NATC_MSGSIZE         = -90    /* EMSGSIZE */
};

/* Mirrors kernel/native.c struct nc_msg_req and struct nc_wait_entry. */
struct nc_msg_req {
    u8 *bytes;
    u32 byte_len;
    u32 ncaps;
    u64 cap_handles[NAT_MSG_MAX_CAPS];
    u32 cap_rights[NAT_MSG_MAX_CAPS];
    u32 caller_pid;
    u32 move_count;
    u32 src_slots[NAT_MSG_MAX_CAPS];
    u32 src_gens[NAT_MSG_MAX_CAPS];
    u32 src_modes[NAT_MSG_MAX_CAPS];
    u64 moved_old_nh[NAT_MSG_MAX_CAPS];
    u64 recv_installed_caps[NAT_MSG_MAX_CAPS];
};

struct nc_wait_entry {
    u64 nh;
    u32 events;
    u32 revents;
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
extern s32 native_channel_send_timed(u64 handle, u64 value, u64 abs_deadline_ns);
extern s32 native_channel_recv_timed(u64 handle, u64 *value, u64 abs_deadline_ns);
extern s32 native_chan_send_msg(u64 handle, struct nc_msg_req *req, u64 abs_deadline_ns);
extern s32 native_chan_recv_msg(u64 handle, struct nc_msg_req *req, u64 abs_deadline_ns);
extern s32 native_mutex_create(u64 *out);
extern s32 native_mutex_lock_intr(u64 handle, u32 block);
extern s32 native_mutex_lock_timed(u64 handle, u64 abs_deadline_ns);
extern s32 native_mutex_unlock(u64 handle);
extern s32 native_mutex_consistent(u64 handle);
extern s32 native_mutex_stat(u64 handle, u32 *out);
extern s32 native_wait_any(struct nc_wait_entry *entries, u32 count,
                           u64 abs_deadline_ns, u32 *win_out);
extern s32 native_exec_obj_create(u32 kind, u64 target_id, u64 *out_nh);
extern s32 native_exec_obj_wait(u64 handle, u64 *result_out, u64 abs_deadline_ns);
extern s32 native_exec_obj_target(u64 handle, u32 want_kind, u32 want_rights, u64 *target_out);
extern u32 sched_alloc_pid(void);
extern u32 sched_clone_thread(u64 fn, u64 user_stack, u64 arg, u32 child_tid);
extern u32 sched_live_process_count(void);
extern u32 sched_fork_proc_limit(void);
extern u32 sched_signal_send(u32 pid, u32 sig);
extern u32 sched_cancel_task_tid(u32 tid);

struct uproc_spawn_img {
    u64 path_u;
    u64 argv_u;
    u64 envp_u;
    u64 out_entry;
    u64 out_sp;
    u64 out_as_h;
    char cmdline[128];
    u32 cmdline_len;
    char environ[256];
    u32 environ_len;
};

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

extern u32 uproc_prepare_spawn_image(struct uproc_spawn_img *img);
extern u32 sched_spawn_native_prepare(struct sched_spawn_req *req);
extern void sched_spawn_native_abort(u32 slot, u32 child_pid);
extern void sched_spawn_native_publish(u32 slot);

static void nat_retire_gen(u32 *gen) {
    if (*gen < NAT_GEN_MAX) {
        *gen = *gen + 1;
    }
}

static u32 nat_free_slot_count(struct nat_cap *caps) {
    u32 i;
    u32 n = 0;
    for (i = 0; i < NAT_CAPS; i++) {
        if (caps[i].rights == 0 && caps[i].gen != NAT_GEN_MAX) {
            n = n + 1;
        }
    }
    return n;
}

u64 nat_install_in_table(struct nat_cap *caps, u32 *used, u64 handle, u32 rights) {
    u32 i;
    for (i = 0; i < NAT_CAPS; i++) {
        struct nat_cap *c = &caps[i];
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

void nat_free_slot_in_table(struct nat_cap *caps, u32 *used, u32 slot) {
    struct nat_cap *c = &caps[slot];
    c->rights = 0;
    c->handle = 0;
    nat_retire_gen(&c->gen);
    *used = *used - 1;
}

s64 nat_lookup_cap(u32 pid, u64 cap, u32 want_rights) {
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
        if (c->rights == 0 || c->gen == 0 || c->gen == NAT_GEN_MAX || c->gen != gen) {
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

s64 nat_map_rc(s32 rc) {
    if (rc == 0) return 0;
    if (rc == 1) return NATC_EOF;
    if (rc == -1) return NATC_BAD_HANDLE;
    if (rc == -2) return NATC_RIGHTS;
    if (rc == -3) return NATC_FULL;
    if (rc == -4) return NATC_AGAIN;
    if (rc == -5) return NATC_CLOSED;
    if (rc == -8) return NATC_INTR;
    if (rc == -9) return NATC_DEADLK;
    if (rc == -10) return NATC_OWNER_DIED;
    if (rc == -11) return NATC_NOT_RECOVERABLE;
    if (rc == -12) return NATC_TIMEDOUT;
    if (rc == -13) return NATC_MSGSIZE;
    return NATC_INVAL;
}

/* Called from native_chan_send_msg under o->obj_lock -> sched_run_lock:
 * All-or-nothing verification + MOVE retirement of sender capabilities. */
s32 nat_commit_send_caps(struct nc_msg_req *req) {
    u32 token;
    u32 *used;
    struct nat_cap *caps;
    u32 k;
    u32 m = 0;
    token = sched_nat_acquire(req->caller_pid, &caps, &used);
    if (token == 0) return -1;
    for (k = 0; k < req->ncaps; k++) {
        u32 sl = req->src_slots[k];
        struct nat_cap *c;
        u32 need = req->cap_rights[k] | NATC_TRANSFER;
        if (sl >= NAT_CAPS) {
            sched_nat_release(token);
            return -1;
        }
        c = &caps[sl];
        if (c->rights == 0 || c->gen != req->src_gens[k]) {
            sched_nat_release(token);
            return -1;  /* NC_BAD_HANDLE: concurrently closed */
        }
        if ((c->rights & need) != need) {
            sched_nat_release(token);
            return -2;  /* NC_RIGHTS */
        }
    }
    for (k = 0; k < req->ncaps; k++) {
        if (req->src_modes[k] == NAT_XFER_MOVE) {
            u32 sl = req->src_slots[k];
            req->moved_old_nh[m] = caps[sl].handle;
            m = m + 1;
            nat_free_slot_in_table(caps, used, sl);
        }
    }
    req->move_count = m;
    sched_nat_release(token);
    return 0;
}

/* Called from native_chan_recv_msg under o->obj_lock -> sched_run_lock:
 * All-or-nothing installation of attached capabilities into receiver table. */
s32 nat_commit_recv_caps(struct nc_msg_req *req) {
    u32 token;
    u32 *used;
    struct nat_cap *caps;
    u32 k;
    token = sched_nat_acquire(req->caller_pid, &caps, &used);
    if (token == 0) return -6;
    if (nat_free_slot_count(caps) < req->ncaps) {
        sched_nat_release(token);
        return -3; /* NC_FULL -> ENFILE, message left untouched in queue */
    }
    for (k = 0; k < req->ncaps; k++) {
        req->recv_installed_caps[k] = nat_install_in_table(
            caps, used, req->cap_handles[k], req->cap_rights[k]);
    }
    sched_nat_release(token);
    return 0;
}

s64 sys_nat_channel_create(u64 send_out, u64 recv_out) {
    u64 as;
    u64 nsend = 0;
    u64 nrecv = 0;
    u32 pid;
    s32 rc;
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
    if (nat_free_slot_count(caps) < 2) {
        sched_nat_release(token);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);
    rc = native_channel_create(&nsend, &nrecv);
    if (rc != 0) return (u64)nat_map_rc(rc);
    token = sched_nat_acquire(pid, &caps, &used);
    if (token == 0) {
        native_handle_close(nsend);
        native_handle_close(nrecv);
        return (u64)NATC_INVAL;
    }
    sslot = nat_install_in_table(caps, used, nsend, NATC_SEND | NATC_TRANSFER);
    rslot = nat_install_in_table(caps, used, nrecv, NATC_RECV | NATC_TRANSFER);
    if (sslot == 0 || rslot == 0) {
        if (sslot != 0) nat_free_slot_in_table(caps, used, (u32)(sslot & 0xFFFFu) - 1);
        if (rslot != 0) nat_free_slot_in_table(caps, used, (u32)(rslot & 0xFFFFu) - 1);
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

s64 sys_nat_send(u64 cap, u64 value, u64 block) {
    s64 h;
    s32 rc;
    if (block > 1) return (u64)NATC_INVAL;
    h = nat_lookup_cap(sched_current_pid(), cap, NATC_SEND);
    if (h < 0) return (u64)h;
    rc = native_channel_send_intr((u64)h, value, (u32)block);
    return (u64)nat_map_rc(rc);
}

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
    h = nat_lookup_cap(sched_current_pid(), cap, NATC_RECV);
    if (h < 0) return (u64)h;
    rc = native_channel_recv_intr((u64)h, &value, (u32)block);
    if (rc == 0) {
        *(volatile u64 *)value_out = value;
        return 1;
    }
    return (u64)nat_map_rc(rc);
}

/* Deadline-aware scalar channel syscalls (abs_deadline_ns: 0=poll, ~0ULL=inf). */
s64 sys_nat_send_timed(u64 cap, u64 value, u64 abs_deadline_ns) {
    s64 h;
    s32 rc;
    h = nat_lookup_cap(sched_current_pid(), cap, NATC_SEND);
    if (h < 0) return (u64)h;
    rc = native_channel_send_timed((u64)h, value, abs_deadline_ns);
    return (u64)nat_map_rc(rc);
}

s64 sys_nat_recv_timed(u64 cap, u64 value_out, u64 abs_deadline_ns) {
    u64 as;
    s64 h;
    s32 rc;
    u64 value = 0;
    if ((value_out & 3ULL) != 0) return (u64)NATC_INVAL;
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, value_out, 8)) {
        return (u64)NATC_INVAL;
    }
    h = nat_lookup_cap(sched_current_pid(), cap, NATC_RECV);
    if (h < 0) return (u64)h;
    rc = native_channel_recv_timed((u64)h, &value, abs_deadline_ns);
    if (rc == 0) {
        *(volatile u64 *)value_out = value;
        return 1;
    }
    return (u64)nat_map_rc(rc);
}

/* Bounded byte message + two-phase capability transfer (DUP / MOVE).
 * User descriptor at desc_u (32 bytes):
 *   [0]  u64 bytes_ptr
 *   [8]  u64 bytes_len   (0..256)
 *   [16] u64 caps_ptr    (send: {u64 cap, u32 rights, u32 mode}[]; recv: u64[])
 *   [24] u64 caps_count  (0..4) */
s64 sys_nat_chan_send_msg(u64 cap, u64 desc_u, u64 abs_deadline_ns) {
    u64 as;
    u32 pid;
    s64 ch_nh;
    u64 bytes_ptr;
    u64 bytes_len;
    u64 caps_ptr;
    u64 caps_count;
    u8 kbytes[NAT_MSG_MAX_BYTES];
    u64 xfer_caps[NAT_MSG_MAX_CAPS];
    u64 src_nh[NAT_MSG_MAX_CAPS];
    struct nc_msg_req req;
    u32 k;
    u32 j;
    s32 rc;

    if ((desc_u & 7ULL) != 0) return (u64)NATC_INVAL;
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, desc_u, 32)) {
        return (u64)NATC_INVAL;
    }
    bytes_ptr  = *(volatile u64 *)(desc_u + 0);
    bytes_len  = *(volatile u64 *)(desc_u + 8);
    caps_ptr   = *(volatile u64 *)(desc_u + 16);
    caps_count = *(volatile u64 *)(desc_u + 24);
    if (bytes_len > NAT_MSG_MAX_BYTES || caps_count > NAT_MSG_MAX_CAPS) {
        return (u64)NATC_INVAL;
    }
    if (bytes_len > 0) {
        if (bytes_ptr == 0 || !mem.as_user_range_ok(as, bytes_ptr, bytes_len)) {
            return (u64)NATC_INVAL;
        }
        /* Fault/copy user bytes into kernel staging OUTSIDE all spinlocks. */
        for (k = 0; k < (u32)bytes_len; k++) {
            kbytes[k] = ((volatile u8 *)bytes_ptr)[k];
        }
    }
    if (caps_count > 0) {
        if (caps_ptr == 0 || (caps_ptr & 3ULL) != 0
            || !mem.as_user_range_ok(as, caps_ptr, caps_count * 16)) {
            return (u64)NATC_INVAL;
        }
        for (k = 0; k < (u32)caps_count; k++) {
            u64 icap = *(volatile u64 *)(caps_ptr + (u64)k * 16);
            u32 irights = *(volatile u32 *)(caps_ptr + (u64)k * 16 + 8);
            u32 imode = *(volatile u32 *)(caps_ptr + (u64)k * 16 + 12);
            if ((irights & ~(u32)NATC_ALL_RIGHTS) != 0
                || (irights & ~(u32)NATC_TRANSFER) == 0
                || (imode != NAT_XFER_DUP && imode != NAT_XFER_MOVE)) {
                return (u64)NATC_INVAL;
            }
            xfer_caps[k] = icap;
            req.cap_rights[k] = irights;
            req.src_modes[k] = imode;
            req.src_slots[k] = (u32)(icap & 0xFFFFu) - 1;
            req.src_gens[k] = (u32)(icap >> 16);
            req.cap_handles[k] = 0;
            /* Moving the channel endpoint we are sending on or duplicating a
             * MOVE source in the same message is ambiguous -> EINVAL. */
            if (imode == NAT_XFER_MOVE && icap == cap) {
                return (u64)NATC_INVAL;
            }
        }
        for (k = 0; k < (u32)caps_count; k++) {
            for (j = k + 1; j < (u32)caps_count; j++) {
                if (req.src_slots[k] == req.src_slots[j]
                    && (req.src_modes[k] == NAT_XFER_MOVE
                        || req.src_modes[j] == NAT_XFER_MOVE)) {
                    return (u64)NATC_INVAL;
                }
            }
        }
    }

    pid = sched_current_pid();
    ch_nh = nat_lookup_cap(pid, cap, NATC_SEND);
    if (ch_nh < 0) return (u64)ch_nh;

    /* Phase A: validate source capabilities and pre-allocate native handle
     * duplicates BEFORE acquiring any channel lock. */
    if (caps_count > 0) {
        u32 token;
        u32 *used;
        struct nat_cap *caps;
        token = sched_nat_acquire(pid, &caps, &used);
        if (token == 0) return (u64)NATC_INVAL;
        for (k = 0; k < (u32)caps_count; k++) {
            u32 sl = req.src_slots[k];
            u32 gen = req.src_gens[k];
            u32 need = req.cap_rights[k] | NATC_TRANSFER;
            struct nat_cap *c;
            if (sl >= NAT_CAPS) {
                sched_nat_release(token);
                return (u64)NATC_BAD_HANDLE;
            }
            c = &caps[sl];
            if (c->rights == 0 || c->gen == 0 || c->gen == NAT_GEN_MAX
                || c->gen != gen) {
                sched_nat_release(token);
                return (u64)NATC_BAD_HANDLE;
            }
            if ((c->rights & need) != need) {
                sched_nat_release(token);
                return (u64)NATC_RIGHTS;
            }
            src_nh[k] = c->handle;
        }
        sched_nat_release(token);

        for (k = 0; k < (u32)caps_count; k++) {
            u64 nh = 0;
            s32 drc = native_handle_dup(src_nh[k], req.cap_rights[k], &nh);
            if (drc != 0) {
                while (k > 0) {
                    k = k - 1;
                    native_handle_close(req.cap_handles[k]);
                }
                return (u64)nat_map_rc(drc);
            }
            req.cap_handles[k] = nh;
        }
    }

    req.bytes = kbytes;
    req.byte_len = (u32)bytes_len;
    req.ncaps = (u32)caps_count;
    req.caller_pid = pid;
    req.move_count = 0;

    rc = native_chan_send_msg((u64)ch_nh, &req, abs_deadline_ns);
    if (rc != 0) {
        for (k = 0; k < (u32)caps_count; k++) {
            if (req.cap_handles[k] != 0) {
                native_handle_close(req.cap_handles[k]);
            }
        }
        return (u64)nat_map_rc(rc);
    }
    /* Close sender's retired native handles for any committed MOVE items. */
    for (k = 0; k < req.move_count; k++) {
        native_handle_close(req.moved_old_nh[k]);
    }
    return 0;
}

s64 sys_nat_chan_recv_msg(u64 cap, u64 desc_u, u64 abs_deadline_ns) {
    u64 as;
    u32 pid;
    s64 ch_nh;
    u64 bytes_ptr;
    u64 max_bytes;
    u64 caps_ptr;
    u64 max_caps;
    u8 kbytes[NAT_MSG_MAX_BYTES];
    struct nc_msg_req req;
    u32 k;
    s32 rc;

    if ((desc_u & 7ULL) != 0) return (u64)NATC_INVAL;
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, desc_u, 32)) {
        return (u64)NATC_INVAL;
    }
    bytes_ptr = *(volatile u64 *)(desc_u + 0);
    max_bytes = *(volatile u64 *)(desc_u + 8);
    caps_ptr  = *(volatile u64 *)(desc_u + 16);
    max_caps  = *(volatile u64 *)(desc_u + 24);
    if (max_bytes > 0) {
        u64 check_len = max_bytes > NAT_MSG_MAX_BYTES ? NAT_MSG_MAX_BYTES : max_bytes;
        if (bytes_ptr == 0 || !mem.as_user_range_ok(as, bytes_ptr, check_len)) {
            return (u64)NATC_INVAL;
        }
    }
    if (max_caps > 0) {
        u64 check_caps = max_caps > NAT_MSG_MAX_CAPS ? NAT_MSG_MAX_CAPS : max_caps;
        if (caps_ptr == 0 || (caps_ptr & 7ULL) != 0
            || !mem.as_user_range_ok(as, caps_ptr, check_caps * 8)) {
            return (u64)NATC_INVAL;
        }
    }

    pid = sched_current_pid();
    ch_nh = nat_lookup_cap(pid, cap, NATC_RECV);
    if (ch_nh < 0) return (u64)ch_nh;

    req.bytes = kbytes;
    req.byte_len = max_bytes > NAT_MSG_MAX_BYTES ? NAT_MSG_MAX_BYTES : (u32)max_bytes;
    req.ncaps = max_caps > NAT_MSG_MAX_CAPS ? NAT_MSG_MAX_CAPS : (u32)max_caps;
    req.caller_pid = pid;

    rc = native_chan_recv_msg((u64)ch_nh, &req, abs_deadline_ns);
    if (rc == 1) {
        /* EOF: drained and no live sender. */
        *(volatile u64 *)(desc_u + 8) = 0;
        *(volatile u64 *)(desc_u + 24) = 0;
        return 0;
    }
    if (rc != 0) {
        return (u64)nat_map_rc(rc);
    }
    /* Copy payload and installed capability handles out with IRQs ON. */
    for (k = 0; k < req.byte_len; k++) {
        ((volatile u8 *)bytes_ptr)[k] = kbytes[k];
    }
    for (k = 0; k < req.ncaps; k++) {
        ((volatile u64 *)caps_ptr)[k] = req.recv_installed_caps[k];
    }
    *(volatile u64 *)(desc_u + 8) = (u64)req.byte_len;
    *(volatile u64 *)(desc_u + 24) = (u64)req.ncaps;
    return 1;
}

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
    if ((want & ~(u32)NATC_TRANSFER) == 0
        || (want & ~(u32)NATC_ALL_RIGHTS) != 0) {
        return (u64)NATC_INVAL;
    }
    pid = sched_current_pid();
    h = nat_lookup_cap(pid, cap, want);
    if (h < 0) return (u64)h;
    rc = native_handle_dup((u64)h, want, &nh);
    if (rc != 0) return (u64)nat_map_rc(rc);
    token = sched_nat_acquire(pid, &caps, &used);
    if (token == 0) {
        native_handle_close(nh);
        return (u64)NATC_INVAL;
    }
    slot = nat_install_in_table(caps, used, nh, want);
    if (slot == 0) {
        sched_nat_release(token);
        native_handle_close(nh);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);
    return (s64)slot;
}

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
        if (c->rights == 0 || c->gen == 0 || c->gen == NAT_GEN_MAX
            || c->gen != gen) {
            sched_nat_release(token);
            return (u64)NATC_BAD_HANDLE;
        }
        h = (s64)c->handle;
        nat_free_slot_in_table(caps, used, slot - 1);
    }
    sched_nat_release(token);
    rc = native_handle_close((u64)h);
    if (rc != 0) return (u64)nat_map_rc(rc);
    return 0;
}

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
    if (nat_free_slot_count(caps) < 1) {
        sched_nat_release(token);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);
    if (native_mutex_create(&nh) != 0) return (u64)NATC_FULL;
    token = sched_nat_acquire(pid, &caps, &used);
    if (token == 0) {
        native_handle_close(nh);
        return (u64)NATC_INVAL;
    }
    slot = nat_install_in_table(
        caps, used, nh, NATC_MLOCK | NATC_MUNLOCK | NATC_MWAIT | NATC_TRANSFER);
    if (slot == 0) {
        sched_nat_release(token);
        native_handle_close(nh);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);
    *(volatile u64 *)out = slot;
    return 0;
}

s64 sys_nat_mutex_lock(u64 cap, u64 block) {
    s64 h;
    s32 rc;
    if (block > 1) return (u64)NATC_INVAL;
    h = nat_lookup_cap(sched_current_pid(), cap,
                       block ? (u32)(NATC_MLOCK | NATC_MWAIT) : (u32)NATC_MLOCK);
    if (h < 0) return (u64)h;
    rc = native_mutex_lock_intr((u64)h, (u32)block);
    return (u64)nat_map_rc(rc);
}

s64 sys_nat_mutex_lock_timed(u64 cap, u64 abs_deadline_ns) {
    s64 h;
    s32 rc;
    h = nat_lookup_cap(sched_current_pid(), cap,
                       abs_deadline_ns != 0
                           ? (u32)(NATC_MLOCK | NATC_MWAIT)
                           : (u32)NATC_MLOCK);
    if (h < 0) return (u64)h;
    rc = native_mutex_lock_timed((u64)h, abs_deadline_ns);
    return (u64)nat_map_rc(rc);
}

s64 sys_nat_mutex_unlock(u64 cap) {
    s64 h;
    s32 rc;
    h = nat_lookup_cap(sched_current_pid(), cap, NATC_MUNLOCK);
    if (h < 0) return (u64)h;
    rc = native_mutex_unlock((u64)h);
    return (u64)nat_map_rc(rc);
}

s64 sys_nat_mutex_consistent(u64 cap) {
    s64 h;
    s32 rc;
    h = nat_lookup_cap(sched_current_pid(), cap, NATC_MUNLOCK);
    if (h < 0) return (u64)h;
    rc = native_mutex_consistent((u64)h);
    return (u64)nat_map_rc(rc);
}

s64 sys_nat_mutex_stat(u64 cap, u64 out) {
    u64 as;
    s64 h;
    s32 rc;
    u32 st[4];
    if ((out & 3ULL) != 0) return (u64)NATC_INVAL;
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, out, 12)) return (u64)NATC_INVAL;
    h = nat_lookup_cap(sched_current_pid(), cap, NATC_MWAIT);
    if (h < 0) return (u64)h;
    rc = native_mutex_stat((u64)h, st);
    if (rc != 0) return (u64)nat_map_rc(rc);
    *(volatile u32 *)out = st[0];
    *(volatile u32 *)(out + 4) = st[1];
    *(volatile u32 *)(out + 8) = st[2];
    return 0;
}

/* nat_wait_any(items_u, count, abs_deadline_ns):
 * Each item at items_u + i*16 is { u64 cap; u32 events; u32 revents; }.
 * Returns winning index (0..count-1) on readiness, or negative errno. */
s64 sys_nat_wait_any(u64 items_u, u64 count, u64 abs_deadline_ns) {
    u64 as;
    u32 pid;
    u32 i;
    u32 win = 0;
    s32 rc;
    struct nc_wait_entry entries[NAT_WAIT_MAX];

    if (count == 0 || count > NAT_WAIT_MAX || (items_u & 7ULL) != 0) {
        return (u64)NATC_INVAL;
    }
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, items_u, count * 16)) {
        return (u64)NATC_INVAL;
    }
    pid = sched_current_pid();
    for (i = 0; i < (u32)count; i++) {
        u64 icap = *(volatile u64 *)(items_u + (u64)i * 16);
        u32 ev = *(volatile u32 *)(items_u + (u64)i * 16 + 8);
        u32 want_r = 0;
        s64 nh;
        if (ev == 0 || (ev & ~(u32)(NATW_RECV | NATW_SEND | NATW_MUTEX | NATW_DONE)) != 0) {
            return (u64)NATC_INVAL;
        }
        if (ev & NATW_RECV) want_r = want_r | NATC_RECV;
        if (ev & NATW_SEND) want_r = want_r | NATC_SEND;
        if (ev & NATW_MUTEX) want_r = want_r | NATC_MWAIT;
        if (ev & NATW_DONE) want_r = want_r | NATC_JOIN;
        nh = nat_lookup_cap(pid, icap, want_r);
        if (nh < 0) return (u64)nh;
        entries[i].nh = (u64)nh;
        entries[i].events = ev;
        entries[i].revents = 0;
    }

    rc = native_wait_any(entries, (u32)count, abs_deadline_ns, &win);
    for (i = 0; i < (u32)count; i++) {
        *(volatile u32 *)(items_u + (u64)i * 16 + 12) = entries[i].revents;
    }
    if (rc != 0) {
        return (u64)nat_map_rc(rc);
    }
    return (s64)win;
}

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
            if (c->rights != 0) {
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
        if (c->rights != 0 && c->gen != 0 && c->gen != NAT_GEN_MAX) {
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

/* ---------------------------------------------------------------------------
 * Zero-legacy native execution primitives (architecture section 3):
 *   nat_process_spawn(desc_u, cap_out_u)            -> child_pid (>0) or -errno
 *   nat_task_spawn(entry, stack, arg, cap_out_u)    -> child_tid (>0) or -errno
 *   nat_exec_wait(cap, result_out_u, deadline_ns)   -> 0 or -errno
 *   nat_exec_cancel(cap)                            -> 0 or -errno
 * ------------------------------------------------------------------------- */
enum { NC_TASK_KIND = 3, NC_PROC_KIND = 4 };

s64 sys_nat_process_spawn(u64 desc_u, u64 cap_out_u) {
    u64 as;
    u32 parent_pid;
    u64 path_u;
    u64 argv_u;
    u64 envp_u;
    u64 caps_u;
    u64 caps_count;
    u64 spawn_flags;
    u32 src_slots[8];
    u32 src_gens[8];
    u32 src_modes[8];
    u32 dst_rights[8];
    u64 src_nh[8];
    u64 dup_nh[8];
    u64 moved_old_nh[8];
    u32 move_count = 0;
    struct uproc_spawn_img img;
    struct sched_spawn_req sreq;
    u32 child_pid;
    u64 proc_nh = 0;
    u64 proc_cap = 0;
    u32 child_slot;
    u32 k;
    u32 j;

    if ((desc_u & 7ULL) != 0 || (cap_out_u & 7ULL) != 0) {
        return (u64)NATC_INVAL;
    }
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, desc_u, 48)
        || !mem.as_user_range_ok(as, cap_out_u, 8)) {
        return (u64)NATC_INVAL;
    }
    path_u      = *(volatile u64 *)(desc_u + 0);
    argv_u      = *(volatile u64 *)(desc_u + 8);
    envp_u      = *(volatile u64 *)(desc_u + 16);
    caps_u      = *(volatile u64 *)(desc_u + 24);
    caps_count  = *(volatile u64 *)(desc_u + 32);
    spawn_flags = *(volatile u64 *)(desc_u + 40);
    if (caps_count > 8 || (spawn_flags & ~(u64)1ULL) != 0) {
        return (u64)NATC_INVAL;
    }
    if (caps_count > 0) {
        if (caps_u == 0 || (caps_u & 3ULL) != 0
            || !mem.as_user_range_ok(as, caps_u, caps_count * 16)) {
            return (u64)NATC_INVAL;
        }
        for (k = 0; k < (u32)caps_count; k++) {
            u64 icap = *(volatile u64 *)(caps_u + (u64)k * 16);
            u32 irights = *(volatile u32 *)(caps_u + (u64)k * 16 + 8);
            u32 imode = *(volatile u32 *)(caps_u + (u64)k * 16 + 12);
            if ((irights & ~(u32)NATC_ALL_RIGHTS) != 0
                || (irights & ~(u32)NATC_TRANSFER) == 0
                || (imode != NAT_XFER_DUP && imode != NAT_XFER_MOVE)) {
                return (u64)NATC_INVAL;
            }
            src_slots[k] = (u32)(icap & 0xFFFFu) - 1;
            src_gens[k] = (u32)(icap >> 16);
            src_modes[k] = imode;
            dst_rights[k] = irights;
            dup_nh[k] = 0;
        }
        for (k = 0; k < (u32)caps_count; k++) {
            for (j = k + 1; j < (u32)caps_count; j++) {
                if (src_slots[k] == src_slots[j]
                    && (src_modes[k] == NAT_XFER_MOVE
                        || src_modes[j] == NAT_XFER_MOVE)) {
                    return (u64)NATC_INVAL;
                }
            }
        }
    }

    if (sched_live_process_count() + 1 > sched_fork_proc_limit()) {
        return (u64)NATC_AGAIN;
    }

    parent_pid = sched_current_pid();
    {
        u32 token;
        u32 *used;
        struct nat_cap *caps;
        token = sched_nat_acquire(parent_pid, &caps, &used);
        if (token == 0) return (u64)NATC_INVAL;
        if (nat_free_slot_count(caps) < 1) {
            sched_nat_release(token);
            return (u64)NATC_FULL;
        }
        for (k = 0; k < (u32)caps_count; k++) {
            u32 sl = src_slots[k];
            u32 need = dst_rights[k] | NATC_TRANSFER;
            struct nat_cap *c;
            if (sl >= NAT_CAPS) {
                sched_nat_release(token);
                return (u64)NATC_BAD_HANDLE;
            }
            c = &caps[sl];
            if (c->rights == 0 || c->gen == 0 || c->gen == NAT_GEN_MAX
                || c->gen != src_gens[k]) {
                sched_nat_release(token);
                return (u64)NATC_BAD_HANDLE;
            }
            if ((c->rights & need) != need) {
                sched_nat_release(token);
                return (u64)NATC_RIGHTS;
            }
            src_nh[k] = c->handle;
        }
        sched_nat_release(token);
    }

    for (k = 0; k < (u32)caps_count; k++) {
        u64 nh = 0;
        s32 drc = native_handle_dup(src_nh[k], dst_rights[k], &nh);
        if (drc != 0) {
            while (k > 0) {
                k = k - 1;
                native_handle_close(dup_nh[k]);
            }
            return (u64)nat_map_rc(drc);
        }
        dup_nh[k] = nh;
    }

    img.path_u = path_u;
    img.argv_u = argv_u;
    img.envp_u = envp_u;
    if (!uproc_prepare_spawn_image(&img)) {
        for (k = 0; k < (u32)caps_count; k++) native_handle_close(dup_nh[k]);
        return (u64)(-2); /* ENOENT / bad ELF */
    }

    child_pid = sched_alloc_pid();
    if (!child_pid) {
        mem.as_destroy_h(img.out_as_h);
        for (k = 0; k < (u32)caps_count; k++) native_handle_close(dup_nh[k]);
        return (u64)(-12);
    }
    if (native_exec_obj_create(NC_PROC_KIND, (u64)child_pid, &proc_nh) != 0) {
        mem.as_destroy_h(img.out_as_h);
        for (k = 0; k < (u32)caps_count; k++) native_handle_close(dup_nh[k]);
        return (u64)NATC_FULL;
    }

    sreq.entry_rip = img.out_entry;
    sreq.user_rsp = img.out_sp;
    sreq.as_h = img.out_as_h;
    sreq.child_pid = child_pid;
    sreq.parent_pid = parent_pid;
    sreq.install_console = (spawn_flags & 1ULL) != 0 ? 1u : 0u;
    sreq.ncaps = (u32)caps_count;
    for (k = 0; k < (u32)caps_count; k++) {
        sreq.cap_handles[k] = dup_nh[k];
        sreq.cap_rights[k] = dst_rights[k];
    }
    sreq.cmdline = img.cmdline;
    sreq.cmdline_len = img.cmdline_len;
    sreq.environ = img.environ;
    sreq.environ_len = img.environ_len;

    child_slot = sched_spawn_native_prepare(&sreq);
    if (child_slot == 0xFFFFFFFF) {
        native_handle_close(proc_nh);
        {
            extern void native_exec_obj_complete(u32 kind, u64 target_id, u64 exit_val);
            native_exec_obj_complete(NC_PROC_KIND, (u64)child_pid, 0);
        }
        mem.as_destroy_h(img.out_as_h);
        for (k = 0; k < (u32)caps_count; k++) native_handle_close(dup_nh[k]);
        return (u64)(-12);
    }

    /* Commit parent's MOVE retirements and install proc_cap atomically. */
    {
        u32 token;
        u32 *used;
        struct nat_cap *caps;
        s32 fail_rc = 0;
        token = sched_nat_acquire(parent_pid, &caps, &used);
        if (token == 0) {
            fail_rc = NATC_INVAL;
        } else {
            for (k = 0; k < (u32)caps_count; k++) {
                u32 sl = src_slots[k];
                u32 need = dst_rights[k] | NATC_TRANSFER;
                struct nat_cap *c = &caps[sl];
                if (c->rights == 0 || c->gen != src_gens[k]) {
                    fail_rc = NATC_BAD_HANDLE;
                    break;
                }
                if ((c->rights & need) != need) {
                    fail_rc = NATC_RIGHTS;
                    break;
                }
            }
            if (fail_rc == 0) {
                /* Retire MOVE slots first so even a full table has room. */
                for (k = 0; k < (u32)caps_count; k++) {
                    if (src_modes[k] == NAT_XFER_MOVE) {
                        u32 sl = src_slots[k];
                        moved_old_nh[move_count] = caps[sl].handle;
                        move_count = move_count + 1;
                        nat_free_slot_in_table(caps, used, sl);
                    }
                }
                proc_cap = nat_install_in_table(
                    caps, used, proc_nh,
                    NATC_JOIN | NATC_CANCEL | NATC_INSPECT | NATC_TRANSFER);
                if (proc_cap == 0) {
                    /* Cannot happen unless all free slots were retired; roll
                     * back is not possible after MOVE unless checked first, so
                     * check free slots before MOVE above: verified >=1 at entry. */
                    fail_rc = NATC_FULL;
                }
            }
            sched_nat_release(token);
        }
        if (fail_rc != 0) {
            sched_spawn_native_abort(child_slot, child_pid);
            native_handle_close(proc_nh);
            {
                extern void native_exec_obj_complete(u32 kind, u64 target_id, u64 exit_val);
                native_exec_obj_complete(NC_PROC_KIND, (u64)child_pid, 0);
            }
            mem.as_destroy_h(img.out_as_h);
            for (k = 0; k < (u32)caps_count; k++) native_handle_close(dup_nh[k]);
            return (u64)(s64)fail_rc;
        }
    }

    for (k = 0; k < move_count; k++) {
        native_handle_close(moved_old_nh[k]);
    }
    *(volatile u64 *)cap_out_u = proc_cap;
    sched_spawn_native_publish(child_slot);
    return (s64)child_pid;
}

s64 sys_nat_task_spawn(u64 entry, u64 stack, u64 arg, u64 cap_out_u) {
    u64 as;
    u32 pid;
    u32 child_tid;
    u64 task_nh = 0;
    u64 task_cap = 0;
    u32 slot;
    u32 token;
    u32 *used;
    struct nat_cap *caps;

    if (entry == 0 || stack == 0 || (cap_out_u & 7ULL) != 0) {
        return (u64)NATC_INVAL;
    }
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, cap_out_u, 8)) {
        return (u64)NATC_INVAL;
    }
    pid = sched_current_pid();
    token = sched_nat_acquire(pid, &caps, &used);
    if (token == 0) return (u64)NATC_INVAL;
    if (nat_free_slot_count(caps) < 1) {
        sched_nat_release(token);
        return (u64)NATC_FULL;
    }
    sched_nat_release(token);

    child_tid = sched_alloc_pid();
    if (!child_tid) return (u64)(-12);
    if (native_exec_obj_create(NC_TASK_KIND, (u64)child_tid, &task_nh) != 0) {
        return (u64)NATC_FULL;
    }
    token = sched_nat_acquire(pid, &caps, &used);
    if (token != 0) {
        task_cap = nat_install_in_table(
            caps, used, task_nh,
            NATC_JOIN | NATC_CANCEL | NATC_INSPECT | NATC_TRANSFER);
        sched_nat_release(token);
    }
    if (task_cap == 0) {
        extern void native_exec_obj_complete(u32 kind, u64 target_id, u64 exit_val);
        native_handle_close(task_nh);
        native_exec_obj_complete(NC_TASK_KIND, (u64)child_tid, 0);
        return (u64)NATC_FULL;
    }
    /* Write cap_out BEFORE publishing runnable task so the parent and peers
     * observe a valid capability before the task can possibly exit. */
    *(volatile u64 *)cap_out_u = task_cap;
    slot = sched_clone_thread(entry, stack, arg, child_tid);
    if (slot == 0xFFFFFFFF) {
        extern void native_exec_obj_complete(u32 kind, u64 target_id, u64 exit_val);
        *(volatile u64 *)cap_out_u = 0;
        sys_nat_close(task_cap);
        native_exec_obj_complete(NC_TASK_KIND, (u64)child_tid, 0);
        return (u64)(-11);
    }
    return (s64)child_tid;
}

s64 sys_nat_exec_wait(u64 cap, u64 result_out_u, u64 abs_deadline_ns) {
    u64 as;
    s64 nh;
    u64 res = 0;
    s32 rc;
    as = sched_current_as_h();
    if (as == 0) return (u64)NATC_INVAL;
    if (result_out_u != 0) {
        if ((result_out_u & 7ULL) != 0
            || !mem.as_user_range_ok(as, result_out_u, 8)) {
            return (u64)NATC_INVAL;
        }
    }
    nh = nat_lookup_cap(sched_current_pid(), cap, NATC_JOIN);
    if (nh < 0) return (u64)nh;
    rc = native_exec_obj_wait((u64)nh, &res, abs_deadline_ns);
    if (rc != 0) return (u64)nat_map_rc(rc);
    if (result_out_u != 0) {
        *(volatile u64 *)result_out_u = res;
    }
    return 0;
}

s64 sys_nat_exec_cancel(u64 cap) {
    s64 nh;
    u64 target = 0;
    s32 rc;
    nh = nat_lookup_cap(sched_current_pid(), cap, NATC_CANCEL);
    if (nh < 0) return (u64)nh;
    rc = native_exec_obj_target((u64)nh, NC_TASK_KIND, NATC_CANCEL, &target);
    if (rc == 0) {
        sched_cancel_task_tid((u32)target);
        return 0;
    }
    if (rc == 1) return 0; /* already completed */
    rc = native_exec_obj_target((u64)nh, NC_PROC_KIND, NATC_CANCEL, &target);
    if (rc == 0) {
        sched_signal_send((u32)target, 9);
        return 0;
    }
    if (rc == 1) return 0;
    return (u64)nat_map_rc(rc);
}
