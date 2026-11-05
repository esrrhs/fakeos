/* fakeos userland - timeout: run COMMAND with a time limit (thin).
 * Forms: timeout [-f|--foreground] [-s SIG] [-k DUR|--kill-after=DUR]
 *                SECONDS COMMAND [ARG...]
 * SECONDS/DUR are non-negative decimal integers. Uses fork + nanosleep poll +
 * kill(sig) — no alarm(2). Default signal is SIGKILL (9). -k DUR waits DUR
 * seconds after the first signal, then sends SIGKILL if still running (GNU
 * kill-after). Without -k, a short ~2s grace then SIGKILL if needed.
 * Default: put COMMAND in its own process group and signal -pgid (GNU).
 * -f/--foreground: keep COMMAND in timeout's process group; signal the
 * child pid only. Exit 124 on timeout; else child status. COMMAND without
 * '/' is /bin/COMMAND. Compact argv for EXEC_MAX_ARGC=8 (glued -kN/-sN/-f).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern long u_kill(long pid, long sig);
extern long u_setpgid(long pid, long pgid);
extern long u_nanosleep(const void *req, void *rem);
extern void u_exit(long code);
extern char *strcpy(char *dst, const char *src);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);

enum {
    ARG_SLOT = 128,
    WNOHANG = 1,
    SIGKILL = 9,
    SIG_MAX = 31,
    EXIT_TIMEOUT = 124,
    TICKS_PER_SEC = 20
};

static void usage(void) {
    u_write(2, "usage: timeout [-f] [-s SIG] [-k DUR] SECONDS COMMAND [ARG...]\n", 62);
}

static int parse_secs(const char *s, long *out) {
    long v = 0;
    int i = 0;
    if (s == 0 || s[0] == 0) return 0;
    if (s[0] == '+') {
        i = 1;
        if (s[i] == 0) return 0;
    }
    if (s[i] < '0' || s[i] > '9') return 0;
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    *out = v;
    return 1;
}

static int parse_sig(const char *s, long *out) {
    long v = 0;
    int i = 0;
    if (s == 0 || s[0] == 0) return 0;
    if (s[0] == '+') {
        i = 1;
        if (s[i] == 0) return 0;
    }
    if (s[i] < '0' || s[i] > '9') return 0;
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    if (v < 0 || v > SIG_MAX) return 0;
    *out = v;
    return 1;
}

static int resolve_cmd(const char *cmd, char *pathbuf) {
    unsigned long n;
    if (cmd == 0 || cmd[0] == 0) return -1;
    for (n = 0; cmd[n]; n++) {
        if (cmd[n] == '/') {
            if (n + 1 >= ARG_SLOT) return -1;
            strcpy(pathbuf, cmd);
            return 0;
        }
    }
    if (5 + n + 1 >= ARG_SLOT) return -1;
    strcpy(pathbuf, "/bin/");
    strcpy(pathbuf + 5, cmd);
    return 0;
}

static int status_exit(long status) {
    if ((status & 0x7f) == 0) {
        return (int)((status >> 8) & 0xff);
    }
    return 128 + (int)(status & 0x7f);
}

/* Parse leading -f/--foreground, -s/--signal, -k/--kill-after.
 * Returns argv index of SECONDS, or -1. kill_after_out: -1 = default short grace.
 * foreground_out: 1 = keep child in caller's process group. */
static int parse_opts(int argc, char **argv, long *sig_out, long *kill_after_out,
                      int *foreground_out) {
    int ai = 1;
    long sig = SIGKILL;
    long kill_after = -1;
    int foreground = 0;
    while (ai < argc) {
        const char *a = argv[ai];
        int j;
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) {
            ai++;
            break;
        }
        if (strcmp(a, "--foreground") == 0) {
            foreground = 1;
            ai++;
            continue;
        }
        if (strcmp(a, "--signal") == 0) {
            if (ai + 1 >= argc || !parse_sig(argv[ai + 1], &sig)) return -1;
            ai += 2;
            continue;
        }
        if (a[0] == '-' && a[1] == '-' && a[2] == 's' && a[3] == 'i' &&
            a[4] == 'g' && a[5] == 'n' && a[6] == 'a' && a[7] == 'l' &&
            a[8] == '=') {
            if (!parse_sig(a + 9, &sig)) return -1;
            ai++;
            continue;
        }
        if (strcmp(a, "--kill-after") == 0) {
            if (ai + 1 >= argc || !parse_secs(argv[ai + 1], &kill_after)) return -1;
            ai += 2;
            continue;
        }
        if (a[0] == '-' && a[1] == '-' && a[2] == 'k' && a[3] == 'i' &&
            a[4] == 'l' && a[5] == 'l' && a[6] == '-' && a[7] == 'a' &&
            a[8] == 'f' && a[9] == 't' && a[10] == 'e' && a[11] == 'r' &&
            a[12] == '=') {
            if (!parse_secs(a + 13, &kill_after)) return -1;
            ai++;
            continue;
        }
        if (a[1] == '-') return -1; /* unknown long option */

        /* Short cluster: -f, -sN, -kN, -fs9, -fk1, etc. */
        for (j = 1; a[j]; j++) {
            char o = a[j];
            if (o == 'f') {
                foreground = 1;
            } else if (o == 's') {
                if (a[j + 1] != 0) {
                    if (!parse_sig(a + j + 1, &sig)) return -1;
                    j = (int)strlen(a) - 1;
                } else {
                    ai++;
                    if (ai >= argc || !parse_sig(argv[ai], &sig)) return -1;
                }
            } else if (o == 'k') {
                if (a[j + 1] != 0) {
                    if (!parse_secs(a + j + 1, &kill_after)) return -1;
                    j = (int)strlen(a) - 1;
                } else {
                    ai++;
                    if (ai >= argc || !parse_secs(argv[ai], &kill_after)) return -1;
                }
            } else {
                return -1;
            }
        }
        ai++;
    }
    *sig_out = sig;
    *kill_after_out = kill_after;
    *foreground_out = foreground;
    return ai;
}

static int wait_reap(long pid, long *status, long ticks) {
    long ts[2];
    long reaped;
    long left = ticks;
    while (left > 0) {
        reaped = u_wait4(pid, status, WNOHANG, 0);
        if (reaped == pid) return 1;
        if (reaped < 0) return -1;
        ts[0] = 0;
        ts[1] = 50000000L;
        u_nanosleep(ts, 0);
        left--;
    }
    reaped = u_wait4(pid, status, WNOHANG, 0);
    if (reaped == pid) return 1;
    if (reaped < 0) return -1;
    return 0;
}

int main(int argc, char **argv) {
    long sec;
    long pid;
    long status;
    long reaped;
    char pathbuf[ARG_SLOT];
    char *envp[1];
    long ts[2];
    long ticks_left;
    long grace_ticks;
    int ai;
    long sig;
    long kill_after;
    int foreground;
    char **cmd_argv;
    int wr;
    long kill_target; /* pid or -pgid */

    ai = parse_opts(argc, argv, &sig, &kill_after, &foreground);
    if (ai < 0 || ai + 1 >= argc) {
        usage();
        return 1;
    }
    if (!parse_secs(argv[ai], &sec)) {
        u_write(2, "timeout: invalid duration\n", 26);
        return 1;
    }
    if (resolve_cmd(argv[ai + 1], pathbuf) != 0) {
        u_write(2, "timeout: command too long\n", 26);
        return 1;
    }
    cmd_argv = argv + ai + 1;

    envp[0] = 0;
    pid = u_fork();
    if (pid < 0) {
        u_write(2, "timeout: fork failed\n", 21);
        return 1;
    }
    if (pid == 0) {
        if (!foreground) {
            /* New process group (GNU default); ignore setpgid failure. */
            u_setpgid(0, 0);
        }
        if (u_execve(pathbuf, cmd_argv, envp) < 0) {
            u_write(2, "timeout: exec failed\n", 21);
            u_exit(127);
        }
        u_exit(127);
    }
    if (!foreground) {
        /* Parent side of the setpgid race. */
        u_setpgid(pid, pid);
        kill_target = -pid;
    } else {
        kill_target = pid;
    }

    ticks_left = sec * TICKS_PER_SEC;
    for (;;) {
        reaped = u_wait4(pid, &status, WNOHANG, 0);
        if (reaped == pid) {
            return status_exit(status);
        }
        if (reaped < 0) {
            u_write(2, "timeout: wait failed\n", 21);
            return 1;
        }
        if (ticks_left <= 0) {
            break;
        }
        ts[0] = 0;
        ts[1] = 50000000L; /* 50ms */
        u_nanosleep(ts, 0);
        ticks_left--;
    }

    u_kill(kill_target, sig);
    if (kill_after >= 0) {
        grace_ticks = kill_after * TICKS_PER_SEC;
        if (grace_ticks == 0) grace_ticks = 1; /* one poll tick for -k0 */
    } else {
        grace_ticks = 40; /* ~2s default short grace */
    }
    wr = wait_reap(pid, &status, grace_ticks);
    if (wr == 1) return EXIT_TIMEOUT;
    if (wr < 0) {
        u_write(2, "timeout: wait failed\n", 21);
        return 1;
    }
    if (sig != SIGKILL) {
        u_kill(kill_target, SIGKILL);
        wr = wait_reap(pid, &status, 40);
        if (wr == 1) return EXIT_TIMEOUT;
        if (wr < 0) {
            u_write(2, "timeout: wait failed\n", 21);
            return 1;
        }
    }
    u_wait4(pid, &status, 0, 0);
    return EXIT_TIMEOUT;
}
