package kernel;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* ---------------------------------------------------------------------------
 * Anonymous pipes: ring buffer + sleeping locks.
 *
 * Each pipe holds a kmutex around head/tail/count/nreaders/nwriters and
 * separate wait_queue_heads for readers and writers. Blocking uses
 * wait_queue_sleep_unlock so the unlock+sleep is atomic under the run
 * lock (no lost wakeup between kmutex_unlock and sleep). Ends are tagged
 * with FH_PIPE_BASE for sysfile/sched dispatch.
 * ------------------------------------------------------------------------- */

enum {
    PIPE_SIZE     = 4096,
    MAX_PIPES     = 16,
    MAX_POPENS    = 32,
    FH_PIPE_BASE  = 0x40000000u,
    FH_BAD        = 0
};

struct kmutex {
    u32 locked;
    u32 owner;
};

struct wait_queue_head {
    u32 pad;
};

extern void kmutex_init(struct kmutex *m);
extern void kmutex_lock(struct kmutex *m);
extern void kmutex_unlock(struct kmutex *m);
extern void wait_queue_init(struct wait_queue_head *wq);
extern void wait_queue_wake(struct wait_queue_head *wq);
extern void wait_queue_sleep_unlock(struct wait_queue_head *wq, struct kmutex *m);
extern void select_kick(void);
extern u32 sched_should_eintr(void);

struct pipe {
    u32 used;
    u32 head;                 /* next byte to read */
    u32 tail;                 /* next byte to write */
    u32 count;
    u32 nreaders;             /* live read-end opens (refs collapsed) */
    u32 nwriters;
    struct kmutex lock;
    struct wait_queue_head rwait;
    struct wait_queue_head wwait;
    char data[PIPE_SIZE];
};

struct pipe_open {
    u32 used;
    u32 pipe_idx;
    u32 refs;
    u32 writable;             /* 0 = read end, 1 = write end */
};

static struct pipe pipes[MAX_PIPES];
static struct pipe_open popens[MAX_POPENS];
static struct kmutex pipes_table_lock;
static u32 pipes_table_inited;

static void pipes_table_ensure(void) {
    if (!pipes_table_inited) {
        kmutex_init(&pipes_table_lock);
        pipes_table_inited = 1;
    }
}

u32 pipe_fh_is(u32 fh) {
    return (fh & FH_PIPE_BASE) != 0;
}

static u32 pipe_slot(u32 fh) {
    return fh & ~FH_PIPE_BASE;
}

static u32 alloc_pipe_buf(void) {
    u32 i;
    for (i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            pipes[i].used = 1;
            pipes[i].head = 0;
            pipes[i].tail = 0;
            pipes[i].count = 0;
            pipes[i].nreaders = 0;
            pipes[i].nwriters = 0;
            kmutex_init(&pipes[i].lock);
            wait_queue_init(&pipes[i].rwait);
            wait_queue_init(&pipes[i].wwait);
            return i;
        }
    }
    return 0xFFFFFFFF;
}

static u32 alloc_popen(u32 pidx, u32 writable) {
    u32 i;
    for (i = 1; i < MAX_POPENS; i++) {
        if (!popens[i].used) {
            popens[i].used = 1;
            popens[i].pipe_idx = pidx;
            popens[i].refs = 1;
            popens[i].writable = writable;
            return i;
        }
    }
    return 0;
}

/* Create a pipe; on success *rfh / *wfh receive tagged open handles. */
u32 pipe_alloc(u32 *rfh, u32 *wfh) {
    u32 pidx;
    u32 r;
    u32 w;
    if (rfh == 0 || wfh == 0) {
        return 0;
    }
    pipes_table_ensure();
    kmutex_lock(&pipes_table_lock);
    pidx = alloc_pipe_buf();
    if (pidx == 0xFFFFFFFF) {
        kmutex_unlock(&pipes_table_lock);
        return 0;
    }
    r = alloc_popen(pidx, 0);
    w = alloc_popen(pidx, 1);
    if (r == 0 || w == 0) {
        if (r) {
            popens[r].used = 0;
        }
        if (w) {
            popens[w].used = 0;
        }
        pipes[pidx].used = 0;
        kmutex_unlock(&pipes_table_lock);
        return 0;
    }
    pipes[pidx].nreaders = 1;
    pipes[pidx].nwriters = 1;
    *rfh = FH_PIPE_BASE | r;
    *wfh = FH_PIPE_BASE | w;
    kmutex_unlock(&pipes_table_lock);
    return 1;
}

void pipe_retain(u32 fh) {
    u32 s;
    if (!pipe_fh_is(fh)) {
        return;
    }
    pipes_table_ensure();
    kmutex_lock(&pipes_table_lock);
    s = pipe_slot(fh);
    if (s < MAX_POPENS && popens[s].used) {
        popens[s].refs++;
    }
    kmutex_unlock(&pipes_table_lock);
}

u32 pipe_close(u32 fh) {
    u32 s;
    u32 pidx;
    u32 writable;
    u32 fully_free;
    struct pipe *p;
    if (!pipe_fh_is(fh)) {
        return 0;
    }
    pipes_table_ensure();
    kmutex_lock(&pipes_table_lock);
    s = pipe_slot(fh);
    if (s >= MAX_POPENS || !popens[s].used) {
        kmutex_unlock(&pipes_table_lock);
        return 0;
    }
    if (popens[s].refs > 1) {
        popens[s].refs--;
        kmutex_unlock(&pipes_table_lock);
        return 1;
    }
    pidx = popens[s].pipe_idx;
    writable = popens[s].writable;
    popens[s].used = 0;
    popens[s].refs = 0;
    kmutex_unlock(&pipes_table_lock);

    p = &pipes[pidx];
    kmutex_lock(&p->lock);
    if (writable) {
        if (p->nwriters > 0) {
            p->nwriters--;
        }
        /* Last writer gone: wake readers so they observe EOF. */
        if (p->nwriters == 0) {
            wait_queue_wake(&p->rwait);
        }
    } else {
        if (p->nreaders > 0) {
            p->nreaders--;
        }
        if (p->nreaders == 0) {
            wait_queue_wake(&p->wwait);
        }
    }
    fully_free = (p->nreaders == 0 && p->nwriters == 0) ? 1 : 0;
    kmutex_unlock(&p->lock);
    select_kick();

    if (fully_free) {
        kmutex_lock(&pipes_table_lock);
        p->used = 0;
        kmutex_unlock(&pipes_table_lock);
    }
    return 1;
}

u32 pipe_can_read(u32 fh) {
    u32 s;
    u32 idx;
    u32 ok = 0;
    if (!pipe_fh_is(fh)) return 0;
    s = pipe_slot(fh);
    if (s >= MAX_POPENS) return 0;
    if (!popens[s].used) return 0;
    if (popens[s].writable) return 0;
    idx = popens[s].pipe_idx;
    kmutex_lock(&pipes[idx].lock);
    if (pipes[idx].count > 0) ok = 1;
    else if (pipes[idx].nwriters == 0) ok = 1;
    kmutex_unlock(&pipes[idx].lock);
    return ok;
}

u32 pipe_can_write(u32 fh) {
    u32 s;
    u32 idx;
    u32 ok = 0;
    if (!pipe_fh_is(fh)) return 0;
    s = pipe_slot(fh);
    if (s >= MAX_POPENS) return 0;
    if (!popens[s].used) return 0;
    if (!popens[s].writable) return 0;
    idx = popens[s].pipe_idx;
    kmutex_lock(&pipes[idx].lock);
    if (pipes[idx].count < PIPE_SIZE) ok = 1;
    else if (pipes[idx].nreaders == 0) ok = 1;
    kmutex_unlock(&pipes[idx].lock);
    return ok;
}

u64 pipe_read(u32 fh, char *kbuf, u64 len) {
    u32 s;
    struct pipe *p;
    u64 done = 0;
    if (!pipe_fh_is(fh) || kbuf == 0) {
        return (u64)(-1);
    }
    s = pipe_slot(fh);
    if (s >= MAX_POPENS || !popens[s].used || popens[s].writable) {
        return (u64)(-1);
    }
    p = &pipes[popens[s].pipe_idx];
    if (len == 0) {
        return 0;
    }
    kmutex_lock(&p->lock);
    for (;;) {
        while (p->count == 0) {
            if (p->nwriters == 0) {
                kmutex_unlock(&p->lock);
                return done;          /* EOF */
            }
            /* Mesa: atomic unlock+sleep, re-check after re-lock. */
            wait_queue_sleep_unlock(&p->rwait, &p->lock);
            if (sched_should_eintr()) {
                kmutex_unlock(&p->lock);
                return done != 0 ? done : (u64)(-4);
            }
        }
        while (done < len && p->count > 0) {
            kbuf[done] = p->data[p->head];
            p->head++;
            if (p->head >= PIPE_SIZE) {
                p->head = 0;
            }
            p->count--;
            done++;
        }
        /* Data consumed: wake any blocked writers. */
        wait_queue_wake(&p->wwait);
        kmutex_unlock(&p->lock);
        select_kick();
        if (done > 0) {
            return done;
        }
        kmutex_lock(&p->lock);
    }
}

u64 pipe_write(u32 fh, const char *kbuf, u64 len) {
    u32 s;
    struct pipe *p;
    u64 done = 0;
    if (!pipe_fh_is(fh) || kbuf == 0) {
        return (u64)(-1);
    }
    s = pipe_slot(fh);
    if (s >= MAX_POPENS || !popens[s].used || !popens[s].writable) {
        return (u64)(-1);
    }
    p = &pipes[popens[s].pipe_idx];
    if (len == 0) {
        return 0;
    }
    kmutex_lock(&p->lock);
    while (done < len) {
        while (p->count >= PIPE_SIZE) {
            if (p->nreaders == 0) {
                kmutex_unlock(&p->lock);
                return done != 0 ? done : (u64)(-1);   /* broken pipe */
            }
            wait_queue_sleep_unlock(&p->wwait, &p->lock);
            if (sched_should_eintr()) {
                kmutex_unlock(&p->lock);
                return done != 0 ? done : (u64)(-4);
            }
            if (p->nreaders == 0) {
                kmutex_unlock(&p->lock);
                return done != 0 ? done : (u64)(-1);
            }
        }
        while (done < len && p->count < PIPE_SIZE) {
            p->data[p->tail] = kbuf[done];
            p->tail++;
            if (p->tail >= PIPE_SIZE) {
                p->tail = 0;
            }
            p->count++;
            done++;
        }
        wait_queue_wake(&p->rwait);
    }
    kmutex_unlock(&p->lock);
    select_kick();
    return done;
}
