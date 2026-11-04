/* fakeos userland - watch: run COMMAND repeatedly (thin).
 * Forms: watch [-n SEC|-nSEC] COMMAND [ARG...]
 * Default interval 2s. Clears screen (ANSI), forks/execs COMMAND, waits,
 * sleeps, loops. COMMAND without '/' resolves as /bin/COMMAND. Compact
 * argv for EXEC_MAX_ARGC=8. Pair with timeout(1) in smokes to bound runs.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern long u_nanosleep(const void *req, void *rem);
extern void u_exit(long code);
extern char *strcpy(char *dst, const char *src);
extern int strcmp(const char *a, const char *b);

enum {
    ARG_SLOT = 128,
    DEFAULT_SEC = 2
};

static void usage(void) {
    u_write(2, "usage: watch [-n SEC] COMMAND [ARG...]\n", 39);
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

/* Parse leading -n / -nSEC / --. Returns argv index of COMMAND, or -1. */
static int parse_opts(int argc, char **argv, long *sec_out) {
    int ai = 1;
    long sec = DEFAULT_SEC;
    while (ai < argc) {
        const char *a = argv[ai];
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) {
            ai++;
            break;
        }
        if (strcmp(a, "-n") == 0) {
            if (ai + 1 >= argc || !parse_secs(argv[ai + 1], &sec)) return -1;
            ai += 2;
            continue;
        }
        if (a[1] == 'n' && a[2] != 0) {
            /* glued -nSEC */
            if (!parse_secs(a + 2, &sec)) return -1;
            ai++;
            continue;
        }
        return -1;
    }
    *sec_out = sec;
    return ai;
}

static void do_clear(void) {
    u_write(1, "\033[2J\033[H", 7);
}

int main(int argc, char **argv) {
    long sec;
    long pid;
    long status;
    char pathbuf[ARG_SLOT];
    char *envp[1];
    long ts[2];
    int ai;
    char **cmd_argv;

    ai = parse_opts(argc, argv, &sec);
    if (ai < 0 || ai >= argc) {
        usage();
        return 1;
    }
    if (resolve_cmd(argv[ai], pathbuf) != 0) {
        u_write(2, "watch: command too long\n", 24);
        return 1;
    }
    cmd_argv = argv + ai;
    envp[0] = 0;

    for (;;) {
        do_clear();
        pid = u_fork();
        if (pid < 0) {
            u_write(2, "watch: fork failed\n", 19);
            return 1;
        }
        if (pid == 0) {
            if (u_execve(pathbuf, cmd_argv, envp) < 0) {
                u_write(2, "watch: exec failed\n", 19);
                u_exit(127);
            }
            u_exit(127);
        }
        if (u_wait4(pid, &status, 0, 0) < 0) {
            u_write(2, "watch: wait failed\n", 19);
            return 1;
        }
        ts[0] = sec;
        ts[1] = 0;
        u_nanosleep(ts, 0);
    }
    return 0;
}
