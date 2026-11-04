/* fakeos userland - kill: send a signal to processes (thin shell/POSIX mirror).
 * Forms: kill [-SIG] PID...
 *        kill -l [SIGNUM|SIGNAME]...
 * Default SIG is 15 (SIGTERM). -SIG is a non-negative decimal (0..31).
 * PID may be negative to target a process group (-PGID), matching kill(2).
 * kill -l lists names (1..31); with args, print name↔number (accepts SIG
 * prefix). External form for xargs/scripts. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_kill(long pid, long sig);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);

enum {
    SIGTERM = 15,
    SIG_MAX = 31
};

/* Short names without SIG prefix; index = signal number. Empty = unused. */
static const char *sig_names[32] = {
    "",      /* 0 */
    "HUP",   /* 1 */
    "INT",   /* 2 */
    "QUIT",  /* 3 */
    "ILL",   /* 4 */
    "TRAP",  /* 5 */
    "ABRT",  /* 6 */
    "BUS",   /* 7 */
    "FPE",   /* 8 */
    "KILL",  /* 9 */
    "USR1",  /* 10 */
    "SEGV",  /* 11 */
    "USR2",  /* 12 */
    "PIPE",  /* 13 */
    "ALRM",  /* 14 */
    "TERM",  /* 15 */
    "STKFLT",/* 16 */
    "CHLD",  /* 17 */
    "CONT",  /* 18 */
    "STOP",  /* 19 */
    "TSTP",  /* 20 */
    "TTIN",  /* 21 */
    "TTOU",  /* 22 */
    "URG",   /* 23 */
    "XCPU",  /* 24 */
    "XFSZ",  /* 25 */
    "VTALRM",/* 26 */
    "PROF",  /* 27 */
    "WINCH", /* 28 */
    "IO",    /* 29 */
    "PWR",   /* 30 */
    "SYS"    /* 31 */
};

static void usage(void) {
    u_write(2, "usage: kill [-l [SIG]...] | [-SIG] PID...\n", 42);
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

static int eq_ignore_case(const char *a, const char *b) {
    int i;
    if (a == 0 || b == 0) return 0;
    for (i = 0; ; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
        if (ca == 0) return 1;
    }
}

/* Resolve SIGNAME / SIGSIGNAME / number -> 0..31; -1 on failure. */
static int lookup_sig(const char *s) {
    long n;
    int i;
    const char *name = s;
    if (s == 0 || s[0] == 0) return -1;
    if (parse_long(s, &n)) {
        if (n < 0 || n > SIG_MAX) return -1;
        return (int)n;
    }
    if ((s[0] == 'S' || s[0] == 's') &&
        (s[1] == 'I' || s[1] == 'i') &&
        (s[2] == 'G' || s[2] == 'g') && s[3] != 0) {
        name = s + 3;
    }
    for (i = 1; i <= SIG_MAX; i++) {
        if (sig_names[i][0] == 0) continue;
        if (eq_ignore_case(name, sig_names[i])) return i;
    }
    return -1;
}

static int do_list(int argc, char **argv, int ai) {
    int i;
    int rc = 0;
    if (ai >= argc) {
        /* Print 1..31 known names, space-separated (bash-ish thin). */
        int first = 1;
        for (i = 1; i <= SIG_MAX; i++) {
            if (sig_names[i][0] == 0) continue;
            if (!first) u_write(1, " ", 1);
            u_write(1, sig_names[i], strlen(sig_names[i]));
            first = 0;
        }
        u_write(1, "\n", 1);
        return 0;
    }
    for (i = ai; i < argc; i++) {
        long n;
        int sig;
        /* Number -> name; name -> number. */
        if (parse_long(argv[i], &n)) {
            if (n < 0 || n > SIG_MAX || sig_names[(int)n][0] == 0) {
                printf("kill: %s: invalid signal specification\n", argv[i]);
                rc = 1;
                continue;
            }
            printf("%s\n", sig_names[(int)n]);
        } else {
            sig = lookup_sig(argv[i]);
            if (sig < 0) {
                printf("kill: %s: invalid signal specification\n", argv[i]);
                rc = 1;
                continue;
            }
            printf("%d\n", sig);
        }
    }
    return rc;
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

    /* kill -l [SIG...] */
    if (strcmp(argv[1], "-l") == 0) {
        return do_list(argc, argv, 2);
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
