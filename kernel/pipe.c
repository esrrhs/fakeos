package kernel;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* ---------------------------------------------------------------------------
 * Anonymous pipes backed by the scheduler wait queues.
 *
 * pipe() allocates one ring buffer and two open ends (read + write). Ends are
 * tagged with FH_PIPE_BASE so sysfile/sched can dispatch like Ext2 handles.
 * Readers sleep when empty; writers sleep when full; closing the last writer
 * delivers EOF, and closing the last reader fails further writes.
 * ------------------------------------------------------------------------- */

enum {
    PIPE_SIZE     = 4096,
    MAX_PIPES     = 16,
    MAX_POPENS    = 32,
    FH_PIPE_BASE  = 0x40000000u,
    FH_BAD        = 0
};

extern void sched_sleep(u64 chan);
extern void sched_wakeup(u64 chan);
extern void select_kick(void);
extern u32 sched_should_eintr(void);

struct pipe {
    u32 used;
    u32 head;                 /* next byte to read */
    u32 tail;                 /* next byte to write */
    u32 count;
    u32 nreaders;             /* live read-end opens (refs collapsed) */
    u32 nwriters;
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
    pidx = alloc_pipe_buf();
    if (pidx == 0xFFFFFFFF) {
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
        return 0;
    }
    pipes[pidx].nreaders = 1;
    pipes[pidx].nwriters = 1;
    *rfh = FH_PIPE_BASE | r;
    *wfh = FH_PIPE_BASE | w;
    return 1;
}

void pipe_retain(u32 fh) {
    u32 s;
    if (!pipe_fh_is(fh)) {
        return;
    }
    s = pipe_slot(fh);
    if (s >= MAX_POPENS || !popens[s].used) {
        return;
    }
    popens[s].refs++;
}

u32 pipe_close(u32 fh) {
    u32 s;
    u32 pidx;
    struct pipe *p;
    if (!pipe_fh_is(fh)) {
        return 0;
    }
    s = pipe_slot(fh);
    if (s >= MAX_POPENS || !popens[s].used) {
        return 0;
    }
    if (popens[s].refs > 1) {
        popens[s].refs--;
        return 1;
    }
    pidx = popens[s].pipe_idx;
    p = &pipes[pidx];
    if (popens[s].writable) {
        if (p->nwriters > 0) {
            p->nwriters--;
        }
        /* Last writer gone: wake readers so they observe EOF. */
        if (p->nwriters == 0) {
            sched_wakeup((u64)p);
        select_kick();
        }
    } else {
        if (p->nreaders > 0) {
            p->nreaders--;
        }
        if (p->nreaders == 0) {
            sched_wakeup((u64)p);
        select_kick();
        }
    }
    popens[s].used = 0;
    popens[s].refs = 0;
    if (p->nreaders == 0 && p->nwriters == 0) {
        p->used = 0;
    }
    return 1;
}

u32 pipe_can_read(u32 fh) {
    u32 s;
    u32 idx;
    if (!pipe_fh_is(fh)) return 0;
    s = pipe_slot(fh);
    if (s >= MAX_POPENS) return 0;
    if (!popens[s].used) return 0;
    if (popens[s].writable) return 0;
    idx = popens[s].pipe_idx;
    if (pipes[idx].count > 0) return 1;
    if (pipes[idx].nwriters == 0) return 1;
    return 0;
}

u32 pipe_can_write(u32 fh) {
    u32 s;
    u32 idx;
    if (!pipe_fh_is(fh)) return 0;
    s = pipe_slot(fh);
    if (s >= MAX_POPENS) return 0;
    if (!popens[s].used) return 0;
    if (!popens[s].writable) return 0;
    idx = popens[s].pipe_idx;
    if (pipes[idx].count < PIPE_SIZE) return 1;
    if (pipes[idx].nreaders == 0) return 1;
    return 0;
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
    for (;;) {
        while (p->count == 0) {
            if (p->nwriters == 0) {
                return done;          /* EOF */
            }
            /* Mesa: sleep then re-check. Channel is the pipe object. */
            sched_sleep((u64)p);
            /* Fatal / non-SA_RESTART custom handler -> EINTR (handler runs
             * on syscall exit via sigframe, rax restored after sigreturn). */
            if (sched_should_eintr()) {
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
        sched_wakeup((u64)p);
        select_kick();
        if (done > 0) {
            return done;
        }
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
    while (done < len) {
        while (p->count >= PIPE_SIZE) {
            if (p->nreaders == 0) {
                return done != 0 ? done : (u64)(-1);   /* broken pipe */
            }
            sched_sleep((u64)p);
            if (sched_should_eintr()) {
                return done != 0 ? done : (u64)(-4);
            }
            if (p->nreaders == 0) {
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
        sched_wakeup((u64)p);
        select_kick();
    }
    return done;
}
