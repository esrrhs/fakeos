package kernel;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);

/* ---------------------------------------------------------------------------
 * Process exit records and wait4().
 *
 * There is no full zombie struct: when a process exits its address space and
 * kernel stack are reclaimed immediately, while one small exit record
 * (pid/ppid/code) and its DEAD TCB slot are retained until the parent waits.
 * The wait releases both. Children whose parent exits first are reparented
 * to pid 1 (init, which keeps running as /bin/sh).
 *
 * Blocking is implemented by yield-polling (no blocked TCB state exists in
 * this scheduler); WNOHANG returns immediately.
 * ------------------------------------------------------------------------- */

enum {
    MAX_EXIT = 32,
    WNOHANG  = 1
};

struct exit_record {
    u32 used;
    u32 pid;
    u32 ppid;
    u32 code;                       /* low 8 bits, as delivered by exit */
};

static struct exit_record exit_table[MAX_EXIT];

/* sched.c */
extern u32 sched_current_pid(void);
extern u32 sched_current_ppid(void);
extern u64 sched_current_as_h(void);
extern void sched_yield(void);
extern u32 sched_has_live_child(u32 ppid);
extern u32 sched_reap_dead(u32 pid);
extern void sched_reparent_children(u32 old_ppid, u32 new_ppid);

void proc_record_exit(u32 pid, u32 ppid, u64 code) {
    u32 i;
    for (i = 0; i < MAX_EXIT; i++) {
        if (!exit_table[i].used) {
            exit_table[i].used = 1;
            exit_table[i].pid = pid;
            exit_table[i].ppid = ppid;
            exit_table[i].code = (u32)(code & 0xFF);
            return;
        }
    }
    kprintf("[PROC] exit record table full, pid %u status lost\n", (u64)pid);
}

/* Re-point orphaned exit records when a parent terminates first. */
void proc_reparent_exits(u32 old_ppid, u32 new_ppid) {
    u32 i;
    for (i = 0; i < MAX_EXIT; i++) {
        if (exit_table[i].used && exit_table[i].ppid == old_ppid) {
            exit_table[i].ppid = new_ppid;
        }
    }
}

/* Fetch and clear a matching record for parent ppid. want_pid == (u32)-1
 * accepts any child (wait4(-1, ...)). Returns 1 when one was consumed. */
static u32 take_exit(u32 want_pid, u32 ppid, u32 *out_pid, u32 *out_code) {
    u32 i;
    for (i = 0; i < MAX_EXIT; i++) {
        struct exit_record *r = &exit_table[i];
        if (r->used && r->ppid == ppid
            && (want_pid == (u32)(-1) || r->pid == want_pid)) {
            *out_pid = r->pid;
            *out_code = r->code;
            r->used = 0;
            return 1;
        }
    }
    return 0;
}

/* wait4(pid, &status, options, ruspace):
 *   pid > 0  wait that exact child
 *   pid == -1 wait any child
 * Returns the reaped pid, 0 with WNOHANG when nothing is ready, or (u64)-1
 * when the caller has no children (ECHILD). */
u64 sys_wait4(u64 pid, u64 status_u, u64 options, u64 ruspace) {
    u32 parent = sched_current_pid();
    u32 want = (u32)pid;
    u64 as = sched_current_as_h();
    (void)ruspace;

    for (;;) {
        u32 got_pid = 0;
        u32 got_code = 0;
        if (take_exit(want, parent, &got_pid, &got_code)) {
            if (status_u != 0 && mem.as_user_range_ok(as, status_u, 8)) {
                volatile u64 *sp = (volatile u64 *)status_u;
                *sp = (u64)got_code << 8;
            }
            sched_reap_dead(got_pid);
            return (u64)got_pid;
        }
        /* No matching record: if no live child exists either, the caller
         * has nothing to wait for. */
        if (!sched_has_live_child(parent)) {
            return (u64)(-1);
        }
        if (options & WNOHANG) {
            return 0;
        }
        sched_yield();
    }
}
