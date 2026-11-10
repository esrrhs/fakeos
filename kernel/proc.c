package kernel;
import arch;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int64_t i64;

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
    WNOHANG     = 1,
    WUNTRACED   = 2,
    WCONTINUED  = 8
};

struct exit_record {
    u32 used;
    u32 pid;
    u32 ppid;
    u32 code;                       /* low 8 bits, as delivered by exit */
};

static struct exit_record exit_table[MAX_EXIT];
static volatile u32 exit_lock;
extern void spin_lock(volatile u32 *lock);
extern void spin_unlock(volatile u32 *lock);
extern u64 cpu_irq_save(void);
extern void cpu_irq_restore(u64 flags);
extern void sched_yield(void);
extern void sched_sleep_unlock_spin(u64 chan, volatile u32 *condition_lock);


/* sched.c */
extern u32 sched_current_pid(void);
extern u32 sched_sigaction(u32 sig, u64 handler, u64 restorer, u64 flags, u64 *old_out, u64 *old_flags_out);
extern u32 sched_should_eintr(void);
extern u32 sched_sig_get_flags(u32 sig, u64 *flags_out);
extern u32 sched_sigreturn(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi,
                           u64 *f_rax);
extern void sched_check_signals_frame(u64 *f_rip, u64 *f_rflags, u64 *f_rsp, u64 *f_rdi,
                                      u64 *f_rax);
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
extern u32 sched_take_continued(u32 ppid, u32 want_pid, u32 *out_pid);
extern u32 sched_continued_exists(u32 ppid, u32 want_pid);
extern u32 sched_current_pgid(void);
extern u32 sched_signal_pgrp(u32 pgid, u32 sig);
extern u32 sched_setpgid(u32 pid, u32 pgid);
extern u32 sched_setsid(void);
extern u32 sched_get_pgid(u32 pid);
extern u32 sched_get_sid(u32 pid);
extern void tty_set_fg(u32 pid);
extern u32 tty_get_fg(void);
extern u64 tty_jobctl_tcsetpgrp_check(void);

static void proc_record_exit_locked(u32 pid, u32 ppid, u64 code) {
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

void proc_record_exit(u32 pid, u32 ppid, u64 code) {
    u64 flags = cpu_irq_save();
    spin_lock(&exit_lock);
    proc_record_exit_locked(pid, ppid, code);
    spin_unlock(&exit_lock);
    cpu_irq_restore(flags);
}


/* Re-point orphaned exit records when a parent terminates first. */
static void proc_reparent_exits_locked(u32 old_ppid, u32 new_ppid) {
    u32 i;
    for (i = 0; i < MAX_EXIT; i++) {
        if (exit_table[i].used && exit_table[i].ppid == old_ppid) {
            exit_table[i].ppid = new_ppid;
        }
    }
}

void proc_reparent_exits(u32 old_ppid, u32 new_ppid) {
    u64 flags = cpu_irq_save();
    spin_lock(&exit_lock);
    proc_reparent_exits_locked(old_ppid, new_ppid);
    spin_unlock(&exit_lock);
    cpu_irq_restore(flags);
}


/* Fetch and clear a matching record for parent ppid. want_pid == (u32)-1
 * accepts any child (wait4(-1, ...)). Returns 1 when one was consumed. */
static u32 take_exit_locked(u32 want_pid, u32 ppid, u32 *out_pid, u32 *out_code) {
    u32 i;
    for (i = 0; i < MAX_EXIT; i++) {
        struct exit_record *r = &exit_table[i];
        if (r->used && r->ppid == ppid
            && (want_pid == (u32)(-1) || r->pid == want_pid)) {
            /* Exit records precede cleanup. Do not consume status before
             * the child has switched off its stack and is actually DEAD. */
            if (!sched_reap_dead(r->pid)) continue;
            *out_pid = r->pid;
            *out_code = r->code;
            r->used = 0;
            return 1;
        }
    }
    return 0;
}

static u32 take_exit(u32 want_pid, u32 ppid, u32 *out_pid, u32 *out_code) {
    u64 flags = cpu_irq_save();
    u32 result;
    spin_lock(&exit_lock);
    result = take_exit_locked(want_pid, ppid, out_pid, out_code);
    spin_unlock(&exit_lock);
    cpu_irq_restore(flags);
    return result;
}


/* Non-consuming existence check for a matching exit record. */
static u32 exit_exists_locked(u32 want_pid, u32 ppid) {
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

static u32 exit_exists(u32 want_pid, u32 ppid) {
    u64 flags = cpu_irq_save();
    u32 result;
    spin_lock(&exit_lock);
    result = exit_exists_locked(want_pid, ppid);
    spin_unlock(&exit_lock);
    cpu_irq_restore(flags);
    return result;
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
            && !((options & WUNTRACED) && sched_stopped_exists(parent, want))
            && !((options & WCONTINUED) && sched_continued_exists(parent, want))) {
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
        /* Continued children when WCONTINUED is set (status 0xffff). */
        if ((options & WCONTINUED)
            && sched_take_continued(parent, want, &got_pid)) {
            if (status_u != 0 && mem.as_user_range_ok(as, status_u, 8)) {
                volatile u64 *sp = (volatile u64 *)status_u;
                *sp = 0xffff;
            }
            return (u64)got_pid;
        }
        if (take_exit(want, parent, &got_pid, &got_code)) {
            if (status_u != 0 && mem.as_user_range_ok(as, status_u, 8)) {
                volatile u64 *sp = (volatile u64 *)status_u;
                *sp = (u64)got_code << 8;
            }
            return (u64)got_pid;
        }
        /* A recorded exit still finishing teardown is not waitable yet.
         * Yield rather than burning a CPU or consuming its status too early.
         * The normal no-record path below retains blocking wait semantics. */
        if (exit_exists(want, parent)) {
            if (options & WNOHANG) return 0;
            sched_yield();
            continue;
        }
        /* No matching record: if no live child exists either, the caller
         * has nothing to wait for. */
        if (!sched_has_live_child(parent)) {
            return (u64)(-1);
        }
        if (options & WNOHANG) {
            return 0;
        }
        /* IRQ masking alone does not exclude a producer on another CPU.
         * Hold the exit condition lock until run_lock has armed this sleep. */
        {
            u64 chan = sched_wait4_chan(parent);
            arch.cpu_cli();
            spin_lock(&exit_lock);
            if (exit_exists_locked(want, parent)
                || ((options & WUNTRACED) && sched_stopped_exists(parent, want))
                || ((options & WCONTINUED) && sched_continued_exists(parent, want))) {
                spin_unlock(&exit_lock);
                arch.cpu_sti();
                continue;
            }
            if (!sched_has_live_child(parent)) {
                spin_unlock(&exit_lock);
                arch.cpu_sti();
                return (u64)(-1);
            }
            sched_sleep_unlock_spin(chan, &exit_lock);
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

/* kill(pid, sig):
 *   pid > 0  : exact process
 *   pid == 0 : caller's process group
 *   pid < 0  : process group -pid
 * sig==0 is an existence / pgrp membership probe.
 * Returns 0 on success, -1 on failure. */
u64 sys_kill(u64 pid, u64 sig) {
    u32 s = (u32)sig;
    u32 self = sched_current_pid();
    if (s > 31) {
        return SC_ERR;
    }
    if ((i64)pid < 0) {
        u32 pg = (u32)(-(i64)pid);
        u32 n;
        if (pg == 0) {
            return SC_ERR;
        }
        if (s == 0) {
            return sched_signal_pgrp(pg, 0) ? 0 : SC_ERR;
        }
        n = sched_signal_pgrp(pg, s);
        if (n == 0) {
            return SC_ERR;
        }
        if (sched_current_pgid() == pg) {
            sched_check_signals();
        }
        return 0;
    }
    if (pid == 0) {
        u32 pg = sched_current_pgid();
        u32 n;
        if (pg == 0) {
            return SC_ERR;
        }
        if (s == 0) {
            return sched_signal_pgrp(pg, 0) ? 0 : SC_ERR;
        }
        n = sched_signal_pgrp(pg, s);
        if (n == 0) {
            return SC_ERR;
        }
        sched_check_signals();
        return 0;
    }
    {
        u32 p = (u32)pid;
        if (s == 0) {
            return sched_pid_exists(p) ? 0 : SC_ERR;
        }
        if (!sched_signal_send(p, s)) {
            return SC_ERR;
        }
        if (p == self) {
            sched_check_signals();
        }
        return 0;
    }
}

u64 sys_setpgid(u64 pid, u64 pgid) {
    return sched_setpgid((u32)pid, (u32)pgid) ? SC_ERR : 0;
}

u64 sys_setsid(void) {
    u32 r = sched_setsid();
    return r == 0 ? SC_ERR : (u64)r;
}

u64 sys_getpgid(u64 pid) {
    u32 g = sched_get_pgid((u32)pid);
    return g == 0 ? SC_ERR : (u64)g;
}

u64 sys_getsid(u64 pid) {
    u32 s = sched_get_sid((u32)pid);
    return s == 0 ? SC_ERR : (u64)s;
}


/* ioctl(2) TTY: TIOCGPGRP / TIOCSPGRP / TCGETS / TCSETS / TIOCGWINSZ.
 * TCGETS/TCSETS read/write real termios (ICANON/ECHO/ISIG/c_cc).
 * TIOCGWINSZ returns fixed VGA 80x25 (no SIGWINCH). */
u64 sys_ioctl(u64 fd, u64 req, u64 arg) {
    u64 as = sched_current_as_h();
    enum {
        TCGETS_R = 0x5401,
        TCSETS_R = 0x5402,
        TIOCGPGRP_R = 0x540F,
        TIOCSPGRP_R = 0x5410,
        TIOCGWINSZ_R = 0x5413,
        ENOTTY_NEG = (u64)-25
    };
    (void)fd;
    if (req == TIOCGPGRP_R) {
        if (as == 0 || !mem.as_user_range_ok(as, arg, 4)) {
            return (u64)(-1);
        }
        *(volatile u32 *)arg = tty_get_fg();
        return 0;
    }
    if (req == TIOCSPGRP_R) {
        u32 pg;
        u64 jc;
        if (as == 0 || !mem.as_user_range_ok(as, arg, 4)) {
            return (u64)(-1);
        }
        pg = *(volatile u32 *)arg;
        jc = tty_jobctl_tcsetpgrp_check();
        if (jc != 0) {
            return jc;
        }
        if (pg != 0 && sched_signal_pgrp(pg, 0) == 0) {
            return (u64)(-1);
        }
        tty_set_fg(pg);
        return 0;
    }
    if (req == TCGETS_R) {
        u8 tmp[64];
        u32 i;
        extern void tty_termios_get(u8 *dst, u32 n);
        if (as == 0 || !mem.as_user_range_ok(as, arg, 64)) {
            return (u64)(-1);
        }
        tty_termios_get(tmp, 64);
        for (i = 0; i < 64; i++) {
            ((volatile u8 *)arg)[i] = tmp[i];
        }
        return 0;
    }
    if (req == TCSETS_R) {
        u8 tmp[64];
        u32 i;
        extern void tty_termios_set(const u8 *src, u32 n);
        if (as == 0 || !mem.as_user_range_ok(as, arg, 64)) {
            return (u64)(-1);
        }
        for (i = 0; i < 64; i++) {
            tmp[i] = ((volatile u8 *)arg)[i];
        }
        tty_termios_set(tmp, 64);
        return 0;
    }
    /* Fixed VGA text size (80x25). No dynamic resize / SIGWINCH yet. */
    if (req == TIOCGWINSZ_R) {
        volatile u8 *p;
        u32 i;
        if (as == 0 || !mem.as_user_range_ok(as, arg, 8)) {
            return (u64)(-1);
        }
        p = (volatile u8 *)arg;
        for (i = 0; i < 8; i++) {
            p[i] = 0;
        }
        /* little-endian ws_row=25, ws_col=80 */
        p[0] = 25;
        p[2] = 80;
        return 0;
    }
    return ENOTTY_NEG;
}

/* tcsetpgrp-ish (custom nr 501): set TTY foreground process group.
 * Background callers get SIGTTOU (basics). pgid 0 clears. Non-zero pgid must
 * refer to a live process group. Also exposed as u_tcsetpgrp. */
u64 sys_tty_setfg(u64 pgid) {
    u64 jc;
    jc = tty_jobctl_tcsetpgrp_check();
    if (jc != 0) {
        return jc;
    }
    if (pgid != 0) {
        /* Accept if any live task has this pgid (leader or member). */
        if (sched_signal_pgrp((u32)pgid, 0) == 0) {
            return (u64)(-1);
        }
    }
    tty_set_fg((u32)pgid);
    return 0;
}

/* tcgetpgrp-ish (custom nr 502): return foreground pgid (0 if none). */
u64 sys_tty_getfg(u64 fd) {
    (void)fd; /* single console; fd reserved for future per-tty */
    return (u64)tty_get_fg();
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

