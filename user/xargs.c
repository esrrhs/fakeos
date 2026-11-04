/* fakeos userland - xargs: build argv from stdin and exec (thin).
 * Forms: xargs [-n N] [-r] [-I REPLACE] [COMMAND [ARGS...]]
 * Default: whitespace-split stdin words; default COMMAND prints args.
 * -I REPLACE: line-oriented; substitute REPLACE in initial ARGS per line
 * (one exec per line; empty lines skipped). Batches honor EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern long strtol_dec(const char *s);
extern void printf(const char *fmt, ...);

enum {
    MAX_ARGC = 8,
    ARG_SLOT = 128,
};

static void usage(void) {
    u_write(2, "usage: xargs [-n N] [-r] [-I R] [COMMAND [ARGS...]]\n", 52);
}

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

/* Resolve COMMAND into pathbuf. Returns 0 on success. */
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

static void emit_default(char **argv, int argc) {
    int i;
    for (i = 0; i < argc; i++) {
        if (i > 0) u_write(1, " ", 1);
        u_write(1, argv[i], strlen(argv[i]));
    }
    u_write(1, "\n", 1);
}

/* Replace all occurrences of needle in src into dst (size ARG_SLOT).
 * Returns 0 on success, -1 on overflow or empty needle. */
static int subst_all(char *dst, const char *src, const char *needle,
                     const char *repl) {
    unsigned long nl;
    unsigned long rl;
    unsigned long di;
    unsigned long si;
    if (needle == 0 || needle[0] == 0) return -1;
    nl = strlen(needle);
    rl = strlen(repl);
    di = 0;
    si = 0;
    while (src[si]) {
        unsigned long k;
        int match = 1;
        for (k = 0; k < nl; k++) {
            if (src[si + k] != needle[k]) {
                match = 0;
                break;
            }
        }
        if (match) {
            unsigned long j;
            if (di + rl >= ARG_SLOT) return -1;
            for (j = 0; j < rl; j++) dst[di++] = repl[j];
            si += nl;
        } else {
            if (di + 1 >= ARG_SLOT) return -1;
            dst[di++] = src[si++];
        }
    }
    dst[di] = 0;
    return 0;
}

static int run_batch(char **argv, int argc, int have_cmd, char *pathbuf) {
    long pid;
    long status;
    long reaped;
    char *envp[1];
    envp[0] = 0;
    if (argc <= 0) return 0;
    if (!have_cmd) {
        emit_default(argv, argc);
        return 0;
    }
    pid = u_fork();
    if (pid < 0) {
        u_write(2, "xargs: fork failed\n", 19);
        return 1;
    }
    if (pid == 0) {
        if (u_execve(pathbuf, argv, envp) < 0) {
            printf("xargs: %s: exec failed\n", pathbuf);
            u_exit(127);
        }
        u_exit(127);
    }
    reaped = u_wait4(pid, &status, 0, 0);
    if (reaped < 0) return 1;
    if ((status & 0x7f) == 0) {
        return (int)((status >> 8) & 0xff);
    }
    return 1;
}

/* Build substituted argv from templates; run one batch. */
static int run_replace(char templates[][ARG_SLOT], int base, int have_cmd,
                       char *pathbuf, const char *replstr, const char *line,
                       char slots[][ARG_SLOT], char **av) {
    int i;
    if (have_cmd) {
        for (i = 0; i < base; i++) {
            if (subst_all(slots[i], templates[i], replstr, line) != 0) {
                u_write(2, "xargs: arg too long\n", 20);
                return 1;
            }
            av[i] = slots[i];
        }
        av[base] = 0;
        return run_batch(av, base, have_cmd, pathbuf);
    }
    /* No COMMAND: print the input line (thin default). */
    u_write(1, line, strlen(line));
    u_write(1, "\n", 1);
    return 0;
}

int main(int argc, char **argv) {
    int max_n = -1;
    int no_run_empty = 0;
    int ai = 1;
    int have_cmd = 0;
    char pathbuf[ARG_SLOT];
    char slots[MAX_ARGC][ARG_SLOT];
    char templates[MAX_ARGC][ARG_SLOT];
    char *av[MAX_ARGC + 1];
    int base = 0;
    int room;
    int filled = 0;
    char rbuf[256];
    char word[ARG_SLOT];
    int wi = 0;
    long n;
    int i;
    int rc = 0;
    int saw_word = 0;
    int use_replace = 0;
    char replstr[ARG_SLOT];

    replstr[0] = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        if (strcmp(argv[ai], "--") == 0) {
            ai++;
            break;
        }
        if (strcmp(argv[ai], "-r") == 0) {
            no_run_empty = 1;
            ai++;
            continue;
        }
        if (strcmp(argv[ai], "-n") == 0) {
            if (ai + 1 >= argc || !is_digits(argv[ai + 1])) {
                usage();
                return 1;
            }
            max_n = (int)strtol_dec(argv[ai + 1]);
            if (max_n < 1) max_n = 1;
            ai += 2;
            continue;
        }
        if (argv[ai][1] == 'n' && argv[ai][2] != 0 && is_digits(argv[ai] + 2)) {
            max_n = (int)strtol_dec(argv[ai] + 2);
            if (max_n < 1) max_n = 1;
            ai++;
            continue;
        }
        if (strcmp(argv[ai], "-I") == 0) {
            if (ai + 1 >= argc || argv[ai + 1][0] == 0) {
                usage();
                return 1;
            }
            if (strlen(argv[ai + 1]) >= ARG_SLOT) {
                u_write(2, "xargs: replace string too long\n", 31);
                return 1;
            }
            strcpy(replstr, argv[ai + 1]);
            use_replace = 1;
            ai += 2;
            continue;
        }
        if (argv[ai][1] == 'I' && argv[ai][2] != 0) {
            if (strlen(argv[ai] + 2) >= ARG_SLOT) {
                u_write(2, "xargs: replace string too long\n", 31);
                return 1;
            }
            strcpy(replstr, argv[ai] + 2);
            use_replace = 1;
            ai++;
            continue;
        }
        usage();
        return 1;
    }

    if (ai < argc) {
        have_cmd = 1;
        if (resolve_cmd(argv[ai], pathbuf) != 0) {
            u_write(2, "xargs: command too long\n", 24);
            return 1;
        }
        if (strlen(argv[ai]) >= ARG_SLOT) {
            u_write(2, "xargs: command too long\n", 24);
            return 1;
        }
        strcpy(slots[0], argv[ai]);
        strcpy(templates[0], argv[ai]);
        av[0] = slots[0];
        base = 1;
        ai++;
        while (ai < argc && base < MAX_ARGC) {
            if (strlen(argv[ai]) >= ARG_SLOT) {
                u_write(2, "xargs: arg too long\n", 20);
                return 1;
            }
            strcpy(slots[base], argv[ai]);
            strcpy(templates[base], argv[ai]);
            av[base] = slots[base];
            base++;
            ai++;
        }
        if (ai < argc) {
            u_write(2, "xargs: too many initial args\n", 29);
            return 1;
        }
    }

    if (use_replace) {
        /* Line mode: one invocation per non-empty line; substitute REPLACE. */
        wi = 0;
        while ((n = u_read(0, rbuf, sizeof(rbuf))) > 0) {
            for (i = 0; i < (int)n; i++) {
                char c = rbuf[i];
                if (c == '\n' || c == '\r') {
                    if (wi > 0) {
                        word[wi] = 0;
                        saw_word = 1;
                        {
                            int st = run_replace(templates, base, have_cmd,
                                                 pathbuf, replstr, word,
                                                 slots, av);
                            if (st != 0) rc = st;
                        }
                        wi = 0;
                    }
                    continue;
                }
                if (wi < ARG_SLOT - 1) {
                    word[wi++] = c;
                }
            }
        }
        if (wi > 0) {
            word[wi] = 0;
            saw_word = 1;
            {
                int st = run_replace(templates, base, have_cmd, pathbuf,
                                     replstr, word, slots, av);
                if (st != 0) rc = st;
            }
        }
        if (!saw_word && !no_run_empty && have_cmd) {
            /* empty stdin with -I: do not run (GNU-like); -r same */
        }
        return rc;
    }

    room = MAX_ARGC - base;
    if (room < 1 && have_cmd) {
        room = 0;
    }
    if (max_n < 0) {
        max_n = room;
        if (max_n < 1) max_n = 1;
    }
    if (max_n > room) max_n = room;
    if (max_n < 1 && room == 0) {
        if (!no_run_empty) {
            av[base] = 0;
            rc = run_batch(av, base, have_cmd, pathbuf);
        }
        return rc;
    }

    filled = 0;
    wi = 0;
    while ((n = u_read(0, rbuf, sizeof(rbuf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = rbuf[i];
            if (is_ws(c)) {
                if (wi > 0) {
                    word[wi] = 0;
                    strcpy(slots[base + filled], word);
                    av[base + filled] = slots[base + filled];
                    filled++;
                    saw_word = 1;
                    wi = 0;
                    if (filled >= max_n) {
                        av[base + filled] = 0;
                        {
                            int st = run_batch(av, base + filled, have_cmd,
                                               pathbuf);
                            if (st != 0) rc = st;
                        }
                        filled = 0;
                    }
                }
                continue;
            }
            if (wi < ARG_SLOT - 1) {
                word[wi++] = c;
            }
        }
    }
    if (wi > 0) {
        word[wi] = 0;
        strcpy(slots[base + filled], word);
        av[base + filled] = slots[base + filled];
        filled++;
        saw_word = 1;
    }
    if (filled > 0) {
        av[base + filled] = 0;
        {
            int st = run_batch(av, base + filled, have_cmd, pathbuf);
            if (st != 0) rc = st;
        }
    } else if (!no_run_empty && !saw_word) {
        av[base] = 0;
        {
            int st = run_batch(av, base, have_cmd, pathbuf);
            if (st != 0) rc = st;
        }
    }
    return rc;
}
