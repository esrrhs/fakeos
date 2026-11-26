package kernel;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int64_t i64;
typedef types.int32_t s32;

/* ---------------------------------------------------------------------------
 * eventfd(2) + epoll(2)-lite (P1: modern readiness multiplexing).
 *
 * eventfd: a kernel-side u64 counter behind a standard fd (read/write/
 * poll/close/dup2/fork reuse the fd-layer dispatch — no new fd mechanics).
 * EFD_SEMAPHORE reads decrement by one; EFD_NONBLOCK turns empty-read and
 * overflow-write into EAGAIN instead of parking. Blocking paths follow the
 * park-admission contract under the table spinlock (commit releases it).
 *
 * epoll: an interest list of (fd, events, data) over the SHARED readiness
 * predicates (fd_ready_read/fd_ready_write from sysfile.c). Waiting reuses
 * the select(2)/poll(2) model: readiness producers call select_kick(), every
 * waiter wakes and re-scans its own list (level-triggered). Parking follows
 * the select/poll admission pattern — readiness is a predicate checked
 * outside any condition lock, so the park intent is registered first and a
 * concurrent producer latches the notification (PARK_WOKEN) instead of the
 * wake being lost. Stale entries (registered fd closed or reused) are
 * dropped during the scan.
 *
 * Limits: 8 eventfds, 8 epoll instances, 16 items per instance — the usual
 * compile-time object-table bounds. Handle tag bits: FH_EVENTFD_BASE
 * 0x10000000, FH_EPOLL_BASE 0x08000000 (pipe 0x40000000, sock 0x20000000,
 * fs 0x80000000 already taken). Lock order: ev_lock -> (pipe/sock/fs locks
 * inside fd_ready_* scans) and ev_lock -> run_lock; nothing in pipe/sock/fs
 * ever calls back into this module, so no inversion.
 * ------------------------------------------------------------------------- */

enum {
    EVFD_MAX = 8,
    EP_MAX = 8,
    EP_ITEMS = 16,
    EP_MAX_EVENTS = 32,
    FH_EVENTFD_BASE = 0x10000000u,
    FH_EPOLL_BASE = 0x08000000u,
    EVFD_COUNT_MAX = 0xFFFFFFFFFFFFFFFEULL,
    EFD_NONBLOCK_F = 0x800,                 /* Linux EFD_NONBLOCK */
    EFD_SEMAPHORE_F = 1,                    /* Linux EFD_SEMAPHORE */
    EPC_ADD = 1, EPC_MOD = 2, EPC_DEL = 3,  /* Linux EPOLL_CTL_* */
    EPIN = 0x0001,                          /* Linux EPOLLIN */
    EPOUT = 0x0004,                         /* Linux EPOLLOUT */
    EV_BAD = -9,                            /* EBADF */
    EV_NOENT = -2,                          /* ENOENT */
    EV_EXIST = -17,                         /* EEXIST */
    EV_AGAIN = -11,                         /* EAGAIN */
    EV_INVAL = -22,                         /* EINVAL */
    EV_INTR = -4,                           /* EINTR */
    EV_FULL = -23                           /* ENFILE/ENOSPC */
};

struct evfd {
    u32 used;
    u32 flags;
    u32 refs;
    u64 count;
};
struct ep_item {
    u32 used;
    u32 fd;
    u32 fh;
    u32 events;
    u64 data;
};
struct epoll_obj {
    u32 used;
    u32 refs;
    u32 nitems;
    u32 wait_token;
    struct ep_item items[EP_ITEMS];
};

static struct evfd evfds[EVFD_MAX];
static struct epoll_obj eps[EP_MAX];
static volatile u32 ev_lock;

extern void spin_lock(volatile u32 *lock);
extern void spin_unlock(volatile u32 *lock);
extern u64 cpu_irq_save(void);
extern void cpu_irq_restore(u64 flags);
extern void sched_wakeup(u64 chan);
extern void sched_park_prepare(u64 chan);
extern void sched_timer_arm_ticks(u64 abs_ticks);
extern u32 sched_park_commit_unlock_spin(u64 chan, volatile u32 *lock, u32 nonrestartable);
extern u32 sched_park_commit(u64 chan, u32 nonrestartable);
extern void sched_park_abort(void);
extern void sched_yield(void);
extern void sched_check_signals(void);
extern u32 sched_should_eintr_timer(void);
extern u64 sched_clock_ticks(void);
extern u64 sched_current_as_h(void);
extern u32 sched_fd_get(u32 idx);
extern void sched_fd_set(u32 idx, u32 fh);
extern u32 sched_fd_alloc(void);
extern void select_kick(void);
extern u32 fd_ready_read(u32 fh);
extern u32 fd_ready_write(u32 fh);
enum { EV_PARK_INTR = 2 };                  /* matches sched.c PARK_INTERRUPTED */
enum { EV_FD_EMPTY = 0xFFFFFFFFu };

/* Shared user event layout: struct epoll_event { u32 events; u64 data; }
 * packed to 12 bytes (fakecc: explicit byte writes, no struct copy). */
enum { EV_EVENT_SIZE = 12 };

u32 eventfd_fh_is(u32 fh) {
    return (fh & FH_EVENTFD_BASE) != 0 && (fh & 0xE0000000u) == 0;
}
u32 epoll_fh_is(u32 fh) {
    return (fh & FH_EPOLL_BASE) != 0 && (fh & 0xF0000000u) == 0;
}
static u32 evfd_slot(u32 fh) {
    return fh & ~FH_EVENTFD_BASE;
}
static u32 ep_slot(u32 fh) {
    return fh & ~FH_EPOLL_BASE;
}

static void evfd_put8(char *kbuf, u64 v) {
    kbuf[0] = (char)(v & 0xFFu);
    kbuf[1] = (char)((v >> 8) & 0xFFu);
    kbuf[2] = (char)((v >> 16) & 0xFFu);
    kbuf[3] = (char)((v >> 24) & 0xFFu);
    kbuf[4] = (char)((v >> 32) & 0xFFu);
    kbuf[5] = (char)((v >> 40) & 0xFFu);
    kbuf[6] = (char)((v >> 48) & 0xFFu);
    kbuf[7] = (char)((v >> 56) & 0xFFu);
}
static u64 evfd_get8(const char *kbuf) {
    return (u64)(unsigned char)kbuf[0]
         | ((u64)(unsigned char)kbuf[1] << 8)
         | ((u64)(unsigned char)kbuf[2] << 16)
         | ((u64)(unsigned char)kbuf[3] << 24)
         | ((u64)(unsigned char)kbuf[4] << 32)
         | ((u64)(unsigned char)kbuf[5] << 40)
         | ((u64)(unsigned char)kbuf[6] << 48)
         | ((u64)(unsigned char)kbuf[7] << 56);
}

static u32 evfd_alloc(u64 initval, u32 flags, u32 *fh_out) {
    u32 i;
    for (i = 0; i < EVFD_MAX; i++) {
        struct evfd *o = &evfds[i];
        if (!o->used) {
            o->used = 1;
            o->flags = flags;
            o->refs = 1;
            o->count = initval;
            *fh_out = FH_EVENTFD_BASE | i;
            return 1;
        }
    }
    return 0;
}
static u32 ep_alloc(u32 *fh_out) {
    u32 i;
    for (i = 0; i < EP_MAX; i++) {
        struct epoll_obj *o = &eps[i];
        if (!o->used) {
            o->used = 1;
            o->refs = 1;
            o->nitems = 0;
            o->wait_token = 0;
            {
                u32 j;
                for (j = 0; j < EP_ITEMS; j++) o->items[j].used = 0;
            }
            *fh_out = FH_EPOLL_BASE | i;
            return 1;
        }
    }
    return 0;
}

u32 eventfd_retain(u32 fh) {
    u32 s;
    if (!eventfd_fh_is(fh)) return 0;
    s = evfd_slot(fh);
    if (s < EVFD_MAX && evfds[s].used) {
        evfds[s].refs = evfds[s].refs + 1;
        return 1;
    }
    return 0;
}
u32 epoll_retain(u32 fh) {
    u32 s;
    if (!epoll_fh_is(fh)) return 0;
    s = ep_slot(fh);
    if (s < EP_MAX && eps[s].used) {
        eps[s].refs = eps[s].refs + 1;
        return 1;
    }
    return 0;
}
u32 eventfd_close(u32 fh) {
    u32 s;
    if (!eventfd_fh_is(fh)) return 0;
    s = evfd_slot(fh);
    if (s >= EVFD_MAX || !evfds[s].used) return 0;
    if (evfds[s].refs > 1) {
        evfds[s].refs = evfds[s].refs - 1;
        return 1;
    }
    evfds[s].used = 0;
    evfds[s].refs = 0;
    evfds[s].count = 0;
    /* Waiters hold their own fd refs, so they were not the closer; wake
     * blocked readers/writers so they re-validate promptly anyway. */
    sched_wakeup((u64)&evfds[s].count);
    select_kick();
    return 1;
}
u32 epoll_close(u32 fh) {
    u32 s;
    if (!epoll_fh_is(fh)) return 0;
    s = ep_slot(fh);
    if (s >= EP_MAX || !eps[s].used) return 0;
    if (eps[s].refs > 1) {
        eps[s].refs = eps[s].refs - 1;
        return 1;
    }
    eps[s].used = 0;
    eps[s].refs = 0;
    eps[s].nitems = 0;
    sched_wakeup((u64)&eps[s].wait_token);
    return 1;
}

/* Readiness predicates for the fd layer. */
u32 eventfd_can_read(u32 fh) {
    u32 s = evfd_slot(fh);
    if (s >= EVFD_MAX || !evfds[s].used) return 0;
    return evfds[s].count != 0;
}
u32 eventfd_can_write(u32 fh) {
    u32 s = evfd_slot(fh);
    if (s >= EVFD_MAX || !evfds[s].used) return 0;
    return evfds[s].count <= EVFD_COUNT_MAX - 1;
}

/* Drop stale entries (registered fd closed or its handle reused — the
 * Linux auto-detach behaviour for this bounded slice). MUST be called under
 * ev_lock: it mutates the item list. */
static void ep_prune(struct epoll_obj *ep) {
    u32 j;
    for (j = 0; j < EP_ITEMS; j++) {
        struct ep_item *it = &ep->items[j];
        u32 fh;
        if (!it->used) continue;
        fh = sched_fd_get(it->fd);
        if (fh == EV_FD_EMPTY || fh != it->fh) {
            it->used = 0;
            ep->nitems = ep->nitems - 1;
        }
    }
}

/* Read-only readiness scan: fd_ready_* may take pipe/sock SLEEPING locks,
 * so this NEVER runs under ev_lock (same shape as the select/poll loops).
 * Races with a concurrent epoll_ctl resolve via the wake-then-rescan model:
 * the next scan sees the updated list. */
static u32 ep_scan(struct epoll_obj *ep) {
    u32 j;
    u32 ready = 0;
    for (j = 0; j < EP_ITEMS; j++) {
        struct ep_item *it = &ep->items[j];
        u32 fh;
        if (!it->used) continue;
        fh = sched_fd_get(it->fd);
        if (fh == EV_FD_EMPTY || fh != it->fh) continue;
        if ((it->events & EPIN) != 0 && fd_ready_read(fh)) {
            ready = ready + 1;
            continue;
        }
        if ((it->events & EPOUT) != 0 && fd_ready_write(fh)) {
            ready = ready + 1;
        }
    }
    return ready;
}

/* Copy ready items into the user event array (12 bytes each); read-only,
 * lock-free like ep_scan. The caller validated the user range. */
static u32 ep_fill(struct epoll_obj *ep, u64 events_u, u32 maxev) {
    u32 j;
    u32 n = 0;
    for (j = 0; j < EP_ITEMS && n < maxev; j++) {
        struct ep_item *it = &ep->items[j];
        u32 fh;
        u32 mask = 0;
        if (!it->used) continue;
        fh = sched_fd_get(it->fd);
        if (fh == EV_FD_EMPTY || fh != it->fh) continue;
        if ((it->events & EPIN) != 0 && fd_ready_read(fh)) mask |= EPIN;
        if ((it->events & EPOUT) != 0 && fd_ready_write(fh)) mask |= EPOUT;
        if (mask == 0) continue;
        *(volatile u32 *)(events_u + (u64)n * EV_EVENT_SIZE) = mask;
        *(volatile u64 *)(events_u + (u64)n * EV_EVENT_SIZE + 4) = it->data;
        n = n + 1;
    }
    return n;
}

/* ---- syscalls ---- */

u64 sys_eventfd2(u64 initval, u64 flags) {
    u32 fh = 0;
    u32 fd;
    u32 ok;
    if (initval > EVFD_COUNT_MAX) return (u64)EV_INVAL;
    if ((flags & ~(u64)(EFD_NONBLOCK_F | EFD_SEMAPHORE_F)) != 0) {
        return (u64)EV_INVAL;
    }
    {
        u64 f = cpu_irq_save();
        spin_lock(&ev_lock);
        ok = evfd_alloc(initval, (u32)flags, &fh);
        spin_unlock(&ev_lock);
        cpu_irq_restore(f);
    }
    if (!ok) return (u64)EV_FULL;
    fd = sched_fd_alloc();
    if (fd == EV_FD_EMPTY) {
        eventfd_close(fh);
        return (u64)EV_FULL;
    }
    sched_fd_set(fd, fh);
    return (u64)fd;
}

u64 sys_epoll_create1(u64 flags) {
    u32 fh = 0;
    u32 fd;
    u32 ok;
    if ((flags & ~(u64)0x80000u) != 0) return (u64)EV_INVAL; /* CLOEXEC ok */
    {
        u64 f = cpu_irq_save();
        spin_lock(&ev_lock);
        ok = ep_alloc(&fh);
        spin_unlock(&ev_lock);
        cpu_irq_restore(f);
    }
    if (!ok) return (u64)EV_FULL;
    fd = sched_fd_alloc();
    if (fd == EV_FD_EMPTY) {
        epoll_close(fh);
        return (u64)EV_FULL;
    }
    sched_fd_set(fd, fh);
    return (u64)fd;
}

/* epoll_ctl(epfd, op, fd, event): event = {u32 events; u64 data}. */
u64 sys_epoll_ctl(u64 epfd, u64 op, u64 fd, u64 ev_u) {
    u64 as = sched_current_as_h();
    u64 f;
    u32 epfh;
    u32 tfh;
    u32 events = 0;
    u64 data = 0;
    u32 s;
    struct epoll_obj *ep;
    u64 rc = 0;
    u32 wake = 0;
    if (as == 0) return (u64)EV_INVAL;
    epfh = sched_fd_get((u32)epfd);
    if (!epoll_fh_is(epfh)) return (u64)EV_BAD;
    s = ep_slot(epfh);
    if (op != EPC_ADD && op != EPC_MOD && op != EPC_DEL) return (u64)EV_INVAL;
    if ((s32)fd < 0 || fd > 0x7FFFFFFFu) return (u64)EV_INVAL;
    if (op != EPC_DEL) {
        if (ev_u == 0 || (ev_u & 3ULL) != 0) return (u64)EV_INVAL;
        if (!mem.as_user_range_ok(as, ev_u, EV_EVENT_SIZE)) return (u64)EV_INVAL;
        events = *(volatile u32 *)ev_u;
        data = *(volatile u64 *)(ev_u + 4);
        if ((events & ~(u32)(EPIN | EPOUT)) != 0) return (u64)EV_INVAL;
        if (events == 0) return (u64)EV_INVAL;
    }
    tfh = sched_fd_get((u32)fd);
    if (tfh == EV_FD_EMPTY) return (u64)EV_BAD;
    f = cpu_irq_save();
    spin_lock(&ev_lock);
    if (s >= EP_MAX || !eps[s].used) {
        spin_unlock(&ev_lock);
        cpu_irq_restore(f);
        return (u64)EV_BAD;
    }
    ep = &eps[s];
    if (op == EPC_ADD) {
        u32 j;
        u32 free_j = EP_ITEMS;
        for (j = 0; j < EP_ITEMS; j++) {
            struct ep_item *it = &ep->items[j];
            if (it->used && it->fd == (u32)fd) {
                rc = (u64)EV_EXIST;
                break;
            }
            if (!it->used && free_j == EP_ITEMS) free_j = j;
        }
        if (rc == 0) {
            if (ep->nitems >= EP_ITEMS || free_j == EP_ITEMS) {
                rc = (u64)EV_FULL;
            } else {
                struct ep_item *it = &ep->items[free_j];
                it->used = 1;
                it->fd = (u32)fd;
                it->fh = tfh;
                it->events = events;
                it->data = data;
                ep->nitems = ep->nitems + 1;
                wake = 1;
            }
        }
    } else if (op == EPC_MOD) {
        u32 j;
        u32 found = 0;
        for (j = 0; j < EP_ITEMS; j++) {
            struct ep_item *it = &ep->items[j];
            if (it->used && it->fd == (u32)fd) {
                it->events = events;
                it->data = data;
                found = 1;
                wake = 1;
                break;
            }
        }
        if (!found) rc = (u64)EV_NOENT;
    } else {
        u32 j;
        u32 found = 0;
        for (j = 0; j < EP_ITEMS; j++) {
            struct ep_item *it = &ep->items[j];
            if (it->used && it->fd == (u32)fd) {
                it->used = 0;
                ep->nitems = ep->nitems - 1;
                found = 1;
                break;
            }
        }
        if (!found) rc = (u64)EV_NOENT;
    }
    spin_unlock(&ev_lock);
    cpu_irq_restore(f);
    /* Deliver already-ready registrations PROMPTLY and outside ev_lock:
     * ep_scan takes the target objects' own (sleeping) locks. A parked
     * waiter would otherwise never see a ready-but-stale registration. */
    if (wake && rc == 0) {
        if (ep_scan(&eps[s]) > 0) {
            sched_wakeup((u64)&eps[s].wait_token);
        }
    }
    return rc;
}

/* epoll_wait(epfd, events, maxevents, timeout_ms): level-triggered; timeout
 * < 0 blocks indefinitely, 0 is non-blocking, >0 is a 100 Hz tick budget. */
u64 sys_epoll_wait(u64 epfd, u64 events_u, u64 maxevents, u64 timeout_ms) {
    u64 as = sched_current_as_h();
    u64 deadline = 0;
    u32 block = 0;
    u32 s;
    struct epoll_obj *ep;
    u32 n;
    if (as == 0) return (u64)EV_INVAL;
    {
        u32 epfh = sched_fd_get((u32)epfd);
        if (!epoll_fh_is(epfh)) return (u64)EV_BAD;
        s = ep_slot(epfh);
    }
    if (maxevents == 0 || maxevents > EP_MAX_EVENTS) return (u64)EV_INVAL;
    if ((events_u & 3ULL) != 0) return (u64)EV_INVAL;
    if (!mem.as_user_range_ok(as, events_u, maxevents * EV_EVENT_SIZE)) {
        return (u64)EV_INVAL;
    }
    if ((i64)timeout_ms < 0) {
        block = 1;
    } else if (timeout_ms == 0) {
        block = 0;
    } else {
        u64 ticks = timeout_ms / 10u + (timeout_ms % 10u != 0) + 1;
        block = 1;
        deadline = sched_clock_ticks();
        if (ticks > ~0ULL - deadline) return (u64)EV_INVAL;
        deadline += ticks;
    }
    for (;;) {
        {
            u64 f = cpu_irq_save();
            spin_lock(&ev_lock);
            if (s >= EP_MAX || !eps[s].used) {
                spin_unlock(&ev_lock);
                cpu_irq_restore(f);
                return (u64)EV_BAD;
            }
            ep = &eps[s];
            ep_prune(ep);
            spin_unlock(&ev_lock);
            cpu_irq_restore(f);
        }
        n = ep_scan(ep);
        if (n > 0) {
            return (u64)ep_fill(ep, events_u, (u32)maxevents);
        }
        if (!block) return 0;
        if ((i64)timeout_ms >= 0 && sched_clock_ticks() >= deadline) return 0;
        sched_check_signals();
        if (sched_should_eintr_timer()) return (u64)EV_INTR;
        /* Same admission shape as select/poll: register the park intent
         * BEFORE sleeping; a readiness producer's select_kick() either finds
         * the published sleeper or latches park_notify (PARK_WOKEN). */
        sched_park_prepare((u64)&ep->wait_token);
        if ((i64)timeout_ms >= 0) {
            sched_timer_arm_ticks(deadline);
        }
        if (sched_park_commit((u64)&ep->wait_token, 1) == EV_PARK_INTR) {
            return (u64)EV_INTR;
        }
    }
}

/* fd-layer readiness for the epoll handle itself (poll on epfd).
 * Read-only, lock-free: fd_ready_* take the targets' sleeping locks. */
u32 epoll_can_read(u32 fh) {
    u32 s = ep_slot(fh);
    if (s >= EP_MAX || !eps[s].used) return 0;
    return ep_scan(&eps[s]) > 0;
}
u32 epoll_can_write(u32 fh) {
    (void)fh;
    return 1;
}

/* eventfd fd-layer read/write (kernel kbuf, pipe_read style). */
u64 eventfd_read(u32 fh, char *kbuf, u64 len) {
    u64 flags;
    u32 s;
    struct evfd *o;
    u64 val;
    if (!eventfd_fh_is(fh) || kbuf == 0 || len < 8) return (u64)EV_INVAL;
    flags = cpu_irq_save();
    spin_lock(&ev_lock);
    s = evfd_slot(fh);
    if (s >= EVFD_MAX || !evfds[s].used) {
        spin_unlock(&ev_lock);
        cpu_irq_restore(flags);
        return (u64)EV_BAD;
    }
    o = &evfds[s];
    for (;;) {
        if (o->count != 0) {
            if ((o->flags & EFD_SEMAPHORE_F) != 0) {
                val = 1;
                o->count = o->count - 1;
            } else {
                val = o->count;
                o->count = 0;
            }
            evfd_put8(kbuf, val);
            /* Wake writers that were blocked on counter space. */
            sched_wakeup((u64)&o->count);
            spin_unlock(&ev_lock);
            cpu_irq_restore(flags);
            return 8;
        }
        if ((o->flags & EFD_NONBLOCK_F) != 0) {
            spin_unlock(&ev_lock);
            cpu_irq_restore(flags);
            return (u64)EV_AGAIN;
        }
        sched_park_prepare((u64)&o->count);
        if (sched_park_commit_unlock_spin((u64)&o->count, &ev_lock, 1)
            == EV_PARK_INTR) {
            cpu_irq_restore(flags);
            return (u64)EV_INTR;
        }
        spin_lock(&ev_lock);
        s = evfd_slot(fh);
        if (s >= EVFD_MAX || !evfds[s].used) {
            spin_unlock(&ev_lock);
            cpu_irq_restore(flags);
            return (u64)EV_BAD;
        }
        o = &evfds[s];
    }
}
u64 eventfd_write(u32 fh, const char *kbuf, u64 len) {
    u64 flags;
    u32 s;
    struct evfd *o;
    u64 val;
    if (!eventfd_fh_is(fh) || kbuf == 0 || len < 8) return (u64)EV_INVAL;
    val = evfd_get8(kbuf);
    if (val == 0) return (u64)EV_INVAL;
    flags = cpu_irq_save();
    spin_lock(&ev_lock);
    s = evfd_slot(fh);
    if (s >= EVFD_MAX || !evfds[s].used) {
        spin_unlock(&ev_lock);
        cpu_irq_restore(flags);
        return (u64)EV_BAD;
    }
    o = &evfds[s];
    for (;;) {
        if (o->count <= EVFD_COUNT_MAX - val) {
            o->count = o->count + val;
            sched_wakeup((u64)&o->count);
            select_kick();
            spin_unlock(&ev_lock);
            cpu_irq_restore(flags);
            return 8;
        }
        if ((o->flags & EFD_NONBLOCK_F) != 0) {
            spin_unlock(&ev_lock);
            cpu_irq_restore(flags);
            return (u64)EV_AGAIN;
        }
        sched_park_prepare((u64)&o->count);
        if (sched_park_commit_unlock_spin((u64)&o->count, &ev_lock, 1)
            == EV_PARK_INTR) {
            cpu_irq_restore(flags);
            return (u64)EV_INTR;
        }
        spin_lock(&ev_lock);
        s = evfd_slot(fh);
        if (s >= EVFD_MAX || !evfds[s].used) {
            spin_unlock(&ev_lock);
            cpu_irq_restore(flags);
            return (u64)EV_BAD;
        }
        o = &evfds[s];
    }
}
