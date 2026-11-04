/* fakeos userland - nice: run COMMAND with adjusted niceness (thin POSIX).
 * Forms: nice
 *        nice [-n adjustment] COMMAND [ARG...]
 *        nice -adjustment COMMAND [ARG...]   (GNU short form)
 * No COMMAND: print the calling process niceness (GNU-compatible).
 * With COMMAND: add adjustment (default +10) via nice(2) (fakeos nr 503),
 * then exec. COMMAND without '/' resolves as /bin/COMMAND (PATH-lite).
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_nice(long inc);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern char *strcpy(char *dst, const char *src);

enum {
    ARG_SLOT = 128,
    DEFAULT_ADJ = 10
};

static void usage(void) {
    u_write(2, "usage: nice [-n adjustment] COMMAND [ARG...]\n", 45);
}

/* Print signed long as decimal + newline on stdout (no printf/va_list). */
static void print_long_nl(long v) {
    char tmp[24];
    char out[24];
    int i = 0;
    int j = 0;
    int neg = 0;
    unsigned long u;
    if (v < 0) {
        neg = 1;
        u = (unsigned long)(-(v + 1)) + 1UL;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) {
        out[j++] = '0';
    } else {
        while (u > 0) {
            tmp[i++] = (char)('0' + (u % 10UL));
            u = u / 10UL;
        }
        while (i > 0) out[j++] = tmp[--i];
    }
    if (neg) {
        /* shift right and insert '-' */
        int k;
        for (k = j; k > 0; k--) out[k] = out[k - 1];
        out[0] = '-';
        j++;
    }
    out[j++] = '\n';
    u_write(1, out, (unsigned long)j);
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

int main(int argc, char **argv) {
    long adj = DEFAULT_ADJ;
    long cur;
    int ai = 1;
    char pathbuf[ARG_SLOT];
    char *envp[1];

    /* bare `nice`: print current niceness. */
    if (argc == 1) {
        cur = u_nice(0);
        print_long_nl(cur);
        return 0;
    }

    if (argv[1][0] == '-' && argv[1][1] == 'n' && argv[1][2] == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        if (!parse_long(argv[2], &adj)) {
            u_write(2, "nice: invalid adjustment\n", 25);
            return 1;
        }
        ai = 3;
    } else if (argv[1][0] == '-' && argv[1][1] != 0
               && ((argv[1][1] >= '0' && argv[1][1] <= '9')
                   || argv[1][1] == '+' || argv[1][1] == '-')) {
        /* GNU short form: nice -5 CMD / nice --10 CMD / nice -+5 CMD */
        if (!parse_long(argv[1] + 1, &adj)) {
            usage();
            return 1;
        }
        ai = 2;
    } else if (argv[1][0] == '-') {
        usage();
        return 1;
    } else {
        /* nice COMMAND — default +10 */
        ai = 1;
    }

    if (ai >= argc) {
        /* adjustment with no COMMAND: apply and print new niceness. */
        cur = u_nice(adj);
        print_long_nl(cur);
        return 0;
    }

    if (resolve_cmd(argv[ai], pathbuf) != 0) {
        u_write(2, "nice: command too long\n", 23);
        return 1;
    }

    (void)u_nice(adj);

    envp[0] = 0;
    if (u_execve(pathbuf, argv + ai, envp) < 0) {
        u_write(2, "nice: exec failed\n", 18);
        u_exit(127);
    }
    return 127;
}
