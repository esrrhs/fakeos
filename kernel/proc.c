package kernel;
import arch;
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
 * Blocking uses sched_sleep on a per-parent wait channel; the exiting child
 * wakes the parent via sched_wakeup. WNOHANG returns immediately.
 * ------------------------------------------------------------------------- */

enum {
    MAX_EXIT = 32,
    WNOHANG    = 1,
    WUNTRACED  = 2
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
extern u32 sched_sigaction(u32 sig, u64 handler, u64 restorer, u64 flags, u64 *old_out, u64 *old_flags_out);
extern u32 sched_should_eintr(void);
extern u32 sched_sig_get_flags(u32 sig, u64 *flags_out);
extern u32 sched_sigreturn(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi);
extern void sched_check_signals_frame(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi);
extern u32 sched_sig_get(u32 sig, u64 *handler_out);
extern u32 sched_current_ppid(void);
extern u64 sched_current_as_h(void);
extern void sched_sleep_cli(u64 chan);
extern u64 sched_wait4_chan(u32 parent_pid);
extern u32 sched_has_live_child(u32 ppid);
extern u32 sched_is_live_child(u32 ppid, u32 pid);
extern u32 sched_reap_dead(u32 pid);
extern void sched_reparent_children(u32 old_ppid, u32 new_ppid);
extern u32 sched_signal_send(u32 pid, u32 sig);
extern u32 sched_pid_exists(u32 pid);
extern void sched_check_signals(void);
extern u32 sched_has_pending_fatal(void);
extern u32 sched_take_stopped(u32 ppid, u32 want_pid, u32 *out_pid, u32 *out_sig);
extern u32 sched_stopped_exists(u32 ppid, u32 want_pid);
extern void tty_set_fg(u32 pid);
extern u32 tty_get_fg(void);

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

/* Non-consuming existence check for a matching exit record. */
static u32 exit_exists(u32 want_pid, u32 ppid) {
    u32 i;
    for (i = 0; i < MAX_EXIT; i++) {
        struct exit_record *r = &exit_table[i];
        if (r->used && r->ppid == ppid
            && (want_pid == (u32)(-1) || r->pid == want_pid)) {
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
        u32 got_sig = 0;
        /* Exact wait for a pid that is neither a live child nor a pending
         * record fails immediately instead of blocking forever, even if the
         * caller happens to have other children. */
        if (want != (u32)(-1) && !exit_exists(want, parent)
            && !sched_is_live_child(parent, want)
            && !((options & WUNTRACED) && sched_stopped_exists(parent, want))) {
            return (u64)(-1);
        }
        /* Stopped children (Ctrl-Z / SIGTSTP) when WUNTRACED is set. */
        if ((options & WUNTRACED)
            && sched_take_stopped(parent, want, &got_pid, &got_sig)) {
            if (status_u != 0 && mem.as_user_range_ok(as, status_u, 8)) {
                volatile u64 *sp = (volatile u64 *)status_u;
                *sp = ((u64)got_sig << 8) | 0x7f;
            }
            return (u64)got_pid;
        }
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
        /* Close the lost-wakeup window: re-check under cli, then sleep
         * without dropping IRQs until after the thread is marked sleeping. */
        {
            u64 chan = sched_wait4_chan(parent);
            arch.cpu_cli();
            if (exit_exists(want, parent)
                || ((options & WUNTRACED) && sched_stopped_exists(parent, want))) {
                arch.cpu_sti();
                continue;
            }
            if (!sched_has_live_child(parent)) {
                arch.cpu_sti();
                return (u64)(-1);
            }
            /* sched_sleep_cli: mark sleeping + switch; returns with IRQs off. */
            sched_sleep_cli(chan);
            arch.cpu_sti();
            if (sched_should_eintr()) {
                return (u64)-4;         /* EINTR */
            }
        }
    }
}

enum {
    SIGINT_N  = 2,
    SIGKILL_N = 9,
    SIGTERM_N = 15,
    SIGCHLD_N = 17,
    SC_ERR    = (u64)-1
};

/* kill(pid, sig): pid>0 exact process; sig==0 existence check.
 * Returns 0 on success, -1 on failure. */
u64 sys_kill(u64 pid, u64 sig) {
    u32 p = (u32)pid;
    u32 s = (u32)sig;
    if (pid == 0 || pid > 0x7FFFFFFFUL) {
        /* No pgrp broadcast yet; reject 0 / negative. */
        return SC_ERR;
    }
    if (s > 31) {
        return SC_ERR;
    }
    if (s == 0) {
        return sched_pid_exists(p) ? 0 : SC_ERR;
    }
    if (!sched_signal_send(p, s)) {
        return SC_ERR;
    }
    /* If we signalled ourselves, deliver before returning. */
    if (p == sched_current_pid()) {
        sched_check_signals();
    }
    return 0;
}

/* Custom: set TTY foreground process for Ctrl-C (SIGINT) delivery. */
u64 sys_tty_setfg(u64 pid) {
    tty_set_fg((u32)pid);
    return 0;
}

/* Linux nr 13: rt_sigaction(sig, act, oldact, sigsetsize).
 * act layout: sa_handler(8) sa_flags(8) sa_restorer(8) sa_mask(8).
 * Custom handlers require a restorer (libc installs one). */
u64 sys_rt_sigaction(u64 sig, u64 act_u, u64 old_u, u64 setsize) {
    u64 as = sched_current_as_h();
    u64 handler = 0;
    u64 restorer = 0;
    u64 flags = 0;
    u64 old = 0;
    u64 oldf = 0;
    u32 st;
    (void)setsize;
    if (as == 0) {
        return (u64)(-1);
    }
    st = sched_sig_get((u32)sig, &old);
    if (st != 0) {
        return (u64)(-22);
    }
    st = sched_sig_get_flags((u32)sig, &oldf);
    if (st != 0) {
        return (u64)(-22);
    }
    if (act_u != 0) {
        if (!mem.as_user_range_ok(as, act_u, 32)) {
            return (u64)(-14);
        }
        handler = *(volatile u64 *)act_u;
        flags = *(volatile u64 *)(act_u + 8);
        restorer = *(volatile u64 *)(act_u + 16);
        st = sched_sigaction((u32)sig, handler, restorer, flags, 0, 0);
        if (st != 0) {
            return (u64)(-22);
        }
    }
    if (old_u != 0) {
        if (!mem.as_user_range_ok(as, old_u, 32)) {
            return (u64)(-14);
        }
        *(volatile u64 *)old_u = old;
        *(volatile u64 *)(old_u + 8) = oldf;
        *(volatile u64 *)(old_u + 16) = 0;
        *(volatile u64 *)(old_u + 24) = 0;
    }
    return 0;
}

