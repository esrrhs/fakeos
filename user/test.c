/* fakeos userland - test / [: thin POSIX expression evaluator.
 * Exit 0 true, 1 false, 2 usage/error. Invoked as [ requires closing ].
 * Unary: -b -c -d -e -f -h -L -p -S -s -r -w -x -z -n
 * Binary: = != < > -eq -ne -lt -le -gt -ge -ot -nt
 *   < > are byte-order lexicographic (strcmp), like bash test.
 * Bang: ! EXPR (single). STRING alone is -n STRING.
 * -ot/-nt compare st_mtime (boot-relative secs); missing-file rules match
 * common POSIX/bash: A -nt B true if A exists and B does not (or A newer);
 * A -ot B true if B exists and A does not (or A older). Equal mtime => false.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_stat(const char *path, void *statbuf);
extern long u_lstat(const char *path, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);

enum {
    ST_MODE = 24,
    ST_SIZE = 48,
    ST_MTIME = 88,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFIFO = 0x1000,
    S_IFCHR = 0x2000,
    S_IFDIR = 0x4000,
    S_IFBLK = 0x6000,
    S_IFREG = 0x8000,
    S_IFLNK = 0xA000,
    S_IFSOCK = 0xC000,
    S_IRANY = 0x124, /* 0444 */
    S_IWANY = 0x92,  /* 0222 */
    S_IXANY = 0x49   /* 0111 */
};

static unsigned int load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
}

static unsigned long load_u64(const char *p) {
    unsigned long v = 0;
    unsigned long i;
    for (i = 0; i < 8; i++) {
        v |= ((unsigned long)(unsigned char)p[i]) << (8 * i);
    }
    return v;
}

static int is_bracket(const char *argv0) {
    unsigned long n;
    if (argv0 == 0) return 0;
    n = strlen(argv0);
    if (n == 0) return 0;
    return argv0[n - 1] == '[';
}

static int parse_long(const char *s, long *out) {
    long v = 0;
    int i = 0;
    int neg = 0;
    if (s == 0 || s[0] == 0) return 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (s[i] == 0) return 0;
    } else if (s[0] == '+') {
        i = 1;
        if (s[i] == 0) return 0;
    }
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    *out = neg ? -v : v;
    return 1;
}

static void usage(int bracket) {
    if (bracket) {
        u_write(2, "usage: [ EXPR ]\n", 16);
    } else {
        u_write(2, "usage: test EXPR\n", 17);
    }
}

static int do_stat(const char *path, int nofollow, char *st) {
    if (nofollow) {
        return u_lstat(path, st) == 0;
    }
    return u_stat(path, st) == 0;
}

static int file_type_is(const char *path, unsigned int want, int nofollow) {
    char st[ST_SIZE_BYTES];
    unsigned int mode;
    if (!do_stat(path, nofollow, st)) return 0;
    mode = load_u32(st + ST_MODE);
    return (mode & S_IFMT) == want;
}

static int file_exists(const char *path) {
    char st[ST_SIZE_BYTES];
    return do_stat(path, 0, st);
}

static int file_size_nonzero(const char *path) {
    char st[ST_SIZE_BYTES];
    if (!do_stat(path, 0, st)) return 0;
    return load_u64(st + ST_SIZE) > 0;
}

static int file_mode_any(const char *path, unsigned int bits) {
    char st[ST_SIZE_BYTES];
    unsigned int mode;
    if (!do_stat(path, 0, st)) return 0;
    mode = load_u32(st + ST_MODE);
    return (mode & bits) != 0;
}

static int file_mtime(const char *path, unsigned long *out) {
    char st[ST_SIZE_BYTES];
    if (!do_stat(path, 0, st)) return 0;
    *out = load_u64(st + ST_MTIME);
    return 1;
}

/* Evaluate argc/argv expression (already stripped of program name / closing ]).
 * Returns 0/1 for true/false, or -1 on error (caller maps to exit 2). */
static int eval_expr(int argc, char **argv) {
    int neg = 0;
    int base = 0;
    int n;
    const char *a0;
    const char *a1;
    const char *a2;

    if (argc == 0) return 0; /* false */

    if (strcmp(argv[0], "!") == 0) {
        neg = 1;
        base = 1;
        if (argc == 1) {
            /* test !  -> true (empty after bang is false, negated) */
            return 1;
        }
    }

    n = argc - base;
    a0 = argv[base];
    a1 = (n >= 2) ? argv[base + 1] : 0;
    a2 = (n >= 3) ? argv[base + 2] : 0;

    if (n == 1) {
        int t = (a0[0] != 0);
        return neg ? !t : t;
    }

    if (n == 2) {
        int t = 0;
        if (strcmp(a0, "-z") == 0) {
            t = (a1[0] == 0);
        } else if (strcmp(a0, "-n") == 0) {
            t = (a1[0] != 0);
        } else if (strcmp(a0, "-e") == 0) {
            t = file_exists(a1);
        } else if (strcmp(a0, "-f") == 0) {
            t = file_type_is(a1, S_IFREG, 0);
        } else if (strcmp(a0, "-d") == 0) {
            t = file_type_is(a1, S_IFDIR, 0);
        } else if (strcmp(a0, "-h") == 0 || strcmp(a0, "-L") == 0) {
            t = file_type_is(a1, S_IFLNK, 1);
        } else if (strcmp(a0, "-b") == 0) {
            t = file_type_is(a1, S_IFBLK, 0);
        } else if (strcmp(a0, "-c") == 0) {
            t = file_type_is(a1, S_IFCHR, 0);
        } else if (strcmp(a0, "-p") == 0) {
            t = file_type_is(a1, S_IFIFO, 0);
        } else if (strcmp(a0, "-S") == 0) {
            t = file_type_is(a1, S_IFSOCK, 0);
        } else if (strcmp(a0, "-s") == 0) {
            t = file_size_nonzero(a1);
        } else if (strcmp(a0, "-r") == 0) {
            t = file_mode_any(a1, S_IRANY);
        } else if (strcmp(a0, "-w") == 0) {
            t = file_mode_any(a1, S_IWANY);
        } else if (strcmp(a0, "-x") == 0) {
            t = file_mode_any(a1, S_IXANY);
        } else {
            return -1;
        }
        return neg ? !t : t;
    }

    if (n == 3) {
        int t = 0;
        long n1 = 0;
        long n2 = 0;
        unsigned long m0 = 0;
        unsigned long m2 = 0;
        int e0;
        int e2;
        if (strcmp(a1, "=") == 0) {
            t = (strcmp(a0, a2) == 0);
        } else if (strcmp(a1, "!=") == 0) {
            t = (strcmp(a0, a2) != 0);
        } else if (strcmp(a1, "<") == 0) {
            t = (strcmp(a0, a2) < 0);
        } else if (strcmp(a1, ">") == 0) {
            t = (strcmp(a0, a2) > 0);
        } else if (strcmp(a1, "-eq") == 0 || strcmp(a1, "-ne") == 0 ||
                   strcmp(a1, "-lt") == 0 || strcmp(a1, "-le") == 0 ||
                   strcmp(a1, "-gt") == 0 || strcmp(a1, "-ge") == 0) {
            if (!parse_long(a0, &n1) || !parse_long(a2, &n2)) return -1;
            if (strcmp(a1, "-eq") == 0) t = (n1 == n2);
            else if (strcmp(a1, "-ne") == 0) t = (n1 != n2);
            else if (strcmp(a1, "-lt") == 0) t = (n1 < n2);
            else if (strcmp(a1, "-le") == 0) t = (n1 <= n2);
            else if (strcmp(a1, "-gt") == 0) t = (n1 > n2);
            else t = (n1 >= n2);
        } else if (strcmp(a1, "-nt") == 0 || strcmp(a1, "-ot") == 0) {
            e0 = file_mtime(a0, &m0);
            e2 = file_mtime(a2, &m2);
            if (strcmp(a1, "-nt") == 0) {
                if (e0 && !e2) t = 1;
                else if (e0 && e2) t = (m0 > m2);
                else t = 0;
            } else {
                if (!e0 && e2) t = 1;
                else if (e0 && e2) t = (m0 < m2);
                else t = 0;
            }
        } else {
            return -1;
        }
        return neg ? !t : t;
    }

    return -1;
}

int main(int argc, char **argv) {
    int bracket = is_bracket(argv[0]);
    int expr_argc;
    char **expr_argv;
    int r;

    if (bracket) {
        if (argc < 2) {
            usage(1);
            return 2;
        }
        if (strcmp(argv[argc - 1], "]") != 0) {
            u_write(2, "[: missing ']\n", 14);
            return 2;
        }
        expr_argc = argc - 2; /* drop argv[0] and trailing ] */
        expr_argv = argv + 1;
    } else {
        expr_argc = argc - 1;
        expr_argv = argv + 1;
    }

    r = eval_expr(expr_argc, expr_argv);
    if (r < 0) {
        usage(bracket);
        return 2;
    }
    return r ? 0 : 1;
}
