/* fakeos userland - timeout: run COMMAND with a time limit (thin).
 * Forms: timeout [-s SIG|--signal SIG|--signal=SIG] SECONDS COMMAND [ARG...]
 * SECONDS is a non-negative decimal integer. Uses fork + nanosleep poll +
 * kill(sig) — no alarm(2). Default signal is SIGKILL (9). Exit 124 on
 * timeout; else child status. COMMAND without '/' is /bin/COMMAND.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern long u_kill(long pid, long sig);
extern long u_nanosleep(const void *req, void *rem);
extern void u_exit(long code);
extern char *strcpy(char *dst, const char *src);
extern int strcmp(const char *a, const char *b);

enum {
    ARG_SLOT = 128,
    WNOHANG = 1,
    SIGKILL = 9,
    SIG_MAX = 31,
    EXIT_TIMEOUT = 124,
    TICKS_PER_SEC = 20
};

static void usage(void) {
    u_write(2, "usage: timeout [-s SIG] SECONDS COMMAND [ARG...]\n", 49);
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

/* Parse leading -s/--signal; returns new argv index of SECONDS, or -1. */
static int parse_opts(int argc, char **argv, long *sig_out) {
    int ai = 1;
    long sig = SIGKILL;
    while (ai < argc) {
        const char *a = argv[ai];
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) {
            ai++;
            break;
        }
        if (strcmp(a, "-s") == 0) {
            if (ai + 1 >= argc || !parse_sig(argv[ai + 1], &sig)) return -1;
            ai += 2;
            continue;
        }
        if (a[1] == 's' && a[2] != 0 && a[1] != '-') {
            /* glued -sN (not --...) */
            if (!parse_sig(a + 2, &sig)) return -1;
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
        return -1;
    }
    *sig_out = sig;
    return ai;
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
    int i;
    int ai;
    long sig;
    char **cmd_argv;

    ai = parse_opts(argc, argv, &sig);
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
        if (u_execve(pathbuf, cmd_argv, envp) < 0) {
            u_write(2, "timeout: exec failed\n", 21);
            u_exit(127);
        }
        u_exit(127);
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

    u_kill(pid, sig);
    for (i = 0; i < 40; i++) {
        reaped = u_wait4(pid, &status, WNOHANG, 0);
        if (reaped == pid) return EXIT_TIMEOUT;
        ts[0] = 0;
        ts[1] = 50000000L;
        u_nanosleep(ts, 0);
    }
    if (sig != SIGKILL) {
        u_kill(pid, SIGKILL);
        for (i = 0; i < 40; i++) {
            reaped = u_wait4(pid, &status, WNOHANG, 0);
            if (reaped == pid) return EXIT_TIMEOUT;
            ts[0] = 0;
            ts[1] = 50000000L;
            u_nanosleep(ts, 0);
        }
    }
    u_wait4(pid, &status, 0, 0);
    return EXIT_TIMEOUT;
}
