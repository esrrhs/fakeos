/* fakeos userland - kill: send a signal to processes (thin shell/POSIX mirror).
 * Forms: kill [-SIG] PID...
 * Default SIG is 15 (SIGTERM). SIG is a non-negative decimal (0..31).
 * PID may be negative to target a process group (-PGID), matching kill(2).
 * External form for xargs/scripts that exec kill without the shell builtin.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_kill(long pid, long sig);
extern void printf(const char *fmt, ...);

enum {
    SIGTERM = 15,
    SIG_MAX = 31
};

static void usage(void) {
    u_write(2, "usage: kill [-SIG] PID...\n", 26);
}

/* Parse signed decimal (optional leading +/-). Returns 1 on success. */
static int parse_long(const char *s, long *out) {
    long v = 0;
    int neg = 0;
    int i = 0;
    if (s == 0 || s[0] == 0) return 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (s[i] == 0) return 0;
    } else if (s[0] == '+') {
        i = 1;
        if (s[i] == 0) return 0;
    }
    if (s[i] < '0' || s[i] > '9') return 0;
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    *out = neg ? -v : v;
    return 1;
}

int main(int argc, char **argv) {
    long sig = SIGTERM;
    int ai = 1;
    int i;
    int rc = 0;
    int any = 0;

    if (argc < 2) {
        usage();
        return 1;
    }

    /* Optional -SIG (must be followed by at least one PID). */
    if (argv[1][0] == '-' && argv[1][1] != 0 && argc >= 3) {
        long s;
        /* Distinguish -SIG from a negative PID when alone would be ambiguous;
         * require a following operand so "kill -9 1" works and "kill -1" is
         * treated as PID -1 with default SIGTERM if we ever allow single-arg
         * negative — with argc>=3 this branch is only the signal form. */
        if (!parse_long(argv[1] + 1, &s) || s < 0 || s > SIG_MAX) {
            usage();
            return 1;
        }
        sig = s;
        ai = 2;
    }

    if (ai >= argc) {
        usage();
        return 1;
    }

    for (i = ai; i < argc; i++) {
        long pid;
        if (!parse_long(argv[i], &pid)) {
            printf("kill: %s: invalid pid\n", argv[i]);
            rc = 1;
            continue;
        }
        any = 1;
        if (u_kill(pid, sig) != 0) {
            printf("kill: (%ld) failed\n", pid);
            rc = 1;
        }
    }
    if (!any) {
        usage();
        return 1;
    }
    return rc;
}
