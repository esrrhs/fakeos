/* fakeos userland - time: run COMMAND and report real elapsed time (thin).
 * Forms: time COMMAND [ARG...]
 *        time -p COMMAND [ARG...]
 * Uses clock_gettime(CLOCK_MONOTONIC) around fork/wait. Reports real time
 * on stderr; -p uses POSIX "real N.NN" (user/sys stubbed as 0.00 — no
 * rusage yet). COMMAND without '/' resolves as /bin/COMMAND (PATH-lite).
 * Compact argv for EXEC_MAX_ARGC=8. Exit status mirrors the child.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern long u_clock_gettime(long clockid, void *tp);
extern void u_exit(long code);
extern char *strcpy(char *dst, const char *src);

enum {
    ARG_SLOT = 128,
    CLOCK_MONOTONIC = 1
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

static void usage(void) {
    u_write(2, "usage: time [-p] COMMAND [ARG...]\n", 34);
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

/* Append signed decimal into buf at *pi. */
static void append_dec(char *buf, int *pi, long v) {
    char tmp[24];
    int i = 0;
    int neg = 0;
    unsigned long u;
    if (v < 0) {
        neg = 1;
        u = (unsigned long)(-(v + 1)) + 1UL;
    } else {
        u = (unsigned long)v;
    }
    if (neg) buf[(*pi)++] = '-';
    if (u == 0) {
        buf[(*pi)++] = '0';
        return;
    }
    while (u > 0) {
        tmp[i++] = (char)('0' + (u % 10UL));
        u = u / 10UL;
    }
    while (i > 0) buf[(*pi)++] = tmp[--i];
}

static void append_cstr(char *buf, int *pi, const char *s) {
    while (*s) buf[(*pi)++] = *s++;
}

/* Emit one "LABEL N.NN\n" line to stderr. */
static void emit_line(const char *label, long sec, long nsec) {
    char buf[64];
    int i = 0;
    long hs = nsec / 10000000L;
    if (hs < 0) hs = 0;
    if (hs > 99) hs = 99;
    append_cstr(buf, &i, label);
    append_dec(buf, &i, sec);
    buf[i++] = '.';
    buf[i++] = (char)('0' + (hs / 10));
    buf[i++] = (char)('0' + (hs % 10));
    buf[i++] = '\n';
    u_write(2, buf, (unsigned long)i);
}

int main(int argc, char **argv) {
    int ai = 1;
    int posix = 0;
    long pid;
    long status;
    long reaped;
    char pathbuf[ARG_SLOT];
    char *envp[1];
    struct timespec t0;
    struct timespec t1;
    long dsec;
    long dnsec;

    if (argc < 2) {
        usage();
        return 1;
    }
    if (argv[1][0] == '-' && argv[1][1] == 'p' && argv[1][2] == 0) {
        posix = 1;
        ai = 2;
        if (ai >= argc) {
            usage();
            return 1;
        }
    } else if (argv[1][0] == '-') {
        usage();
        return 1;
    }

    if (resolve_cmd(argv[ai], pathbuf) != 0) {
        u_write(2, "time: command too long\n", 23);
        return 1;
    }

    envp[0] = 0;
    t0.tv_sec = 0;
    t0.tv_nsec = 0;
    t1.tv_sec = 0;
    t1.tv_nsec = 0;
    if (u_clock_gettime(CLOCK_MONOTONIC, &t0) < 0) {
        u_write(2, "time: clock_gettime failed\n", 27);
        return 1;
    }

    pid = u_fork();
    if (pid < 0) {
        u_write(2, "time: fork failed\n", 18);
        return 1;
    }
    if (pid == 0) {
        if (u_execve(pathbuf, argv + ai, envp) < 0) {
            u_write(2, "time: exec failed\n", 18);
            u_exit(127);
        }
        u_exit(127);
    }

    reaped = u_wait4(pid, &status, 0, 0);
    if (u_clock_gettime(CLOCK_MONOTONIC, &t1) < 0) {
        u_write(2, "time: clock_gettime failed\n", 27);
        return 1;
    }
    if (reaped != pid) {
        u_write(2, "time: wait failed\n", 18);
        return 1;
    }

    dsec = t1.tv_sec - t0.tv_sec;
    dnsec = t1.tv_nsec - t0.tv_nsec;
    if (dnsec < 0) {
        dsec--;
        dnsec += 1000000000L;
    }
    if (dsec < 0) {
        dsec = 0;
        dnsec = 0;
    }

    emit_line("real ", dsec, dnsec);
    if (posix) {
        emit_line("user ", 0, 0);
        emit_line("sys ", 0, 0);
    }
    return status_exit(status);
}
