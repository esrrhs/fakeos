/* fakeos userland - cmp: compare two files byte-by-byte (POSIX-ish subset).
 * Forms: cmp [-l|-s] [-n N|-nN] [-i SKIP|-iSKIP|-i SKIP1:SKIP2] FILE1 FILE2
 * -n limits comparison to at most N bytes (GNU-ish). -i skips SKIP initial
 * bytes of both files (or SKIP1 of FILE1 and SKIP2 of FILE2). Byte/line
 * numbers restart after the skip. Exit 0 identical (within limit), 1 differ,
 * 2 error. "-" means stdin (at most one). Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_lseek(long fd, long off, long whence);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);
extern long strtol_dec(const char *s);

enum {
    O_RDONLY = 0,
    MODE_NORMAL = 0,
    MODE_SILENT = 1,
    MODE_VERBOSE = 2,
    BUF = 512
};

static unsigned long opt_limit; /* 0 = unlimited */
static int have_limit;
static unsigned long skip1;
static unsigned long skip2;

static void usage(void) {
    u_write(2, "usage: cmp [-l|-s] [-n N] [-i SKIP] FILE1 FILE2\n", 49);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

/* Parse SKIP or SKIP1:SKIP2 into skip1/skip2. Returns 1 on success. */
static int parse_skip(const char *s) {
    int i = 0;
    unsigned long a = 0;
    unsigned long b = 0;
    if (s == 0 || s[0] == 0) return 0;
    while (s[i] && s[i] >= '0' && s[i] <= '9') {
        a = a * 10 + (unsigned long)(s[i] - '0');
        i++;
    }
    if (i == 0) return 0;
    if (s[i] == 0) {
        skip1 = a;
        skip2 = a;
        return 1;
    }
    if (s[i] != ':') return 0;
    i++;
    if (s[i] < '0' || s[i] > '9') return 0;
    while (s[i] && s[i] >= '0' && s[i] <= '9') {
        b = b * 10 + (unsigned long)(s[i] - '0');
        i++;
    }
    if (s[i] != 0) return 0;
    skip1 = a;
    skip2 = b;
    return 1;
}

static int apply_skip(long fd, unsigned long n) {
    unsigned char buf[BUF];
    long left;
    long got;
    long off;
    if (n == 0) return 0;
    left = (long)n;
    off = u_lseek(fd, left, 1 /* SEEK_CUR */);
    if (off >= 0) return 0;
    while (left > 0) {
        unsigned long want = BUF;
        if ((unsigned long)left < want) want = (unsigned long)left;
        got = u_read(fd, buf, want);
        if (got < 0) return 1;
        if (got == 0) return 0; /* short file: compare will see EOF */
        left -= got;
    }
    return 0;
}

static void emit_dec(unsigned long v) {
    char tmp[24];
    int i = 24;
    if (v == 0) {
        u_write(1, "0", 1);
        return;
    }
    while (v > 0 && i > 0) {
        i--;
        tmp[i] = (char)('0' + (v % 10));
        v /= 10;
    }
    u_write(1, tmp + i, (unsigned long)(24 - i));
}

static void emit_oct(unsigned long v) {
    char tmp[16];
    int i = 16;
    if (v == 0) {
        u_write(1, "0", 1);
        return;
    }
    while (v > 0 && i > 0) {
        i--;
        tmp[i] = (char)('0' + (v & 7));
        v >>= 3;
    }
    u_write(1, tmp + i, (unsigned long)(16 - i));
}

static int open_in(const char *path, long *fd_out) {
    long fd;
    if (strcmp(path, "-") == 0) {
        *fd_out = 0;
        return 0;
    }
    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("cmp: %s: no such file\n", path);
        return 2;
    }
    *fd_out = fd;
    return 0;
}

static int cmp_fds(long fd1, long fd2, const char *n1, const char *n2, int mode) {
    unsigned char a[BUF];
    unsigned char b[BUF];
    unsigned long byte = 1;
    unsigned long line = 1;
    unsigned long left = have_limit ? opt_limit : 0; /* remaining when limited */
    int differ = 0;
    long na;
    long nb;
    int i;
    unsigned long want;

    for (;;) {
        if (have_limit && left == 0) return differ ? 1 : 0;
        want = BUF;
        if (have_limit && left < want) want = left;
        na = u_read(fd1, a, want);
        nb = u_read(fd2, b, want);
        if (na < 0 || nb < 0) return 2;
        if (na == 0 && nb == 0) return differ ? 1 : 0;
        if (na == 0 || nb == 0) {
            if (mode == MODE_SILENT) return 1;
            printf("cmp: EOF on %s after byte ", na == 0 ? n1 : n2);
            emit_dec(byte - 1);
            u_write(1, "\n", 1);
            return 1;
        }
        i = 0;
        while (i < (int)na && i < (int)nb) {
            if (a[i] != b[i]) {
                differ = 1;
                if (mode == MODE_SILENT) return 1;
                if (mode == MODE_VERBOSE) {
                    emit_dec(byte);
                    u_write(1, " ", 1);
                    emit_oct((unsigned long)a[i]);
                    u_write(1, " ", 1);
                    emit_oct((unsigned long)b[i]);
                    u_write(1, "\n", 1);
                } else {
                    printf("%s %s differ: byte ", n1, n2);
                    emit_dec(byte);
                    u_write(1, ", line ", 7);
                    emit_dec(line);
                    u_write(1, "\n", 1);
                    return 1;
                }
            }
            if (a[i] == '\n') line++;
            byte++;
            if (have_limit) {
                left--;
                if (left == 0) return differ ? 1 : 0;
            }
            i++;
        }
        if (na != nb) {
            if (mode == MODE_SILENT) return 1;
            printf("cmp: EOF on %s after byte ", na < nb ? n1 : n2);
            emit_dec(byte - 1);
            u_write(1, "\n", 1);
            return 1;
        }
    }
}

int main(int argc, char **argv) {
    int i = 1;
    int mode = MODE_NORMAL;
    const char *f1;
    const char *f2;
    long fd1;
    long fd2;
    int rc;
    int close1 = 0;
    int close2 = 0;

    opt_limit = 0;
    have_limit = 0;
    skip1 = 0;
    skip2 = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-") == 0) break;
        if (strcmp(argv[i], "-s") == 0) {
            mode = MODE_SILENT;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-l") == 0) {
            mode = MODE_VERBOSE;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 2;
            }
            opt_limit = (unsigned long)strtol_dec(argv[i + 1]);
            have_limit = 1;
            i += 2;
            continue;
        }
        if (argv[i][1] == 'n' && argv[i][2] != 0 && is_digits(argv[i] + 2)) {
            opt_limit = (unsigned long)strtol_dec(argv[i] + 2);
            have_limit = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc || !parse_skip(argv[i + 1])) {
                usage();
                return 2;
            }
            i += 2;
            continue;
        }
        if (argv[i][1] == 'i' && argv[i][2] != 0 && parse_skip(argv[i] + 2)) {
            i++;
            continue;
        }
        /* glued -sn / -ls / -siN etc */
        {
            int j;
            int handled = 0;
            for (j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 's') {
                    mode = MODE_SILENT;
                    handled = 1;
                } else if (argv[i][j] == 'l') {
                    mode = MODE_VERBOSE;
                    handled = 1;
                } else if (argv[i][j] == 'n') {
                    if (argv[i][j + 1] != 0 && is_digits(argv[i] + j + 1)) {
                        opt_limit = (unsigned long)strtol_dec(argv[i] + j + 1);
                        have_limit = 1;
                        handled = 1;
                        i++;
                        goto next_arg;
                    }
                    if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                        usage();
                        return 2;
                    }
                    opt_limit = (unsigned long)strtol_dec(argv[i + 1]);
                    have_limit = 1;
                    i += 2;
                    goto next_arg;
                } else if (argv[i][j] == 'i') {
                    if (argv[i][j + 1] != 0 && parse_skip(argv[i] + j + 1)) {
                        handled = 1;
                        i++;
                        goto next_arg;
                    }
                    if (i + 1 >= argc || !parse_skip(argv[i + 1])) {
                        usage();
                        return 2;
                    }
                    i += 2;
                    goto next_arg;
                } else {
                    usage();
                    return 2;
                }
            }
            if (!handled) {
                usage();
                return 2;
            }
            i++;
        }
    next_arg:
        ;
    }
    if (i + 2 != argc) {
        usage();
        return 2;
    }
    f1 = argv[i];
    f2 = argv[i + 1];
    if (strcmp(f1, "-") == 0 && strcmp(f2, "-") == 0) {
        u_write(2, "cmp: dual stdin not supported\n", 30);
        return 2;
    }
    rc = open_in(f1, &fd1);
    if (rc != 0) return rc;
    if (strcmp(f1, "-") != 0) close1 = 1;
    rc = open_in(f2, &fd2);
    if (rc != 0) {
        if (close1) u_close(fd1);
        return rc;
    }
    if (strcmp(f2, "-") != 0) close2 = 1;
    if (apply_skip(fd1, skip1) != 0 || apply_skip(fd2, skip2) != 0) {
        if (close1) u_close(fd1);
        if (close2) u_close(fd2);
        return 2;
    }
    rc = cmp_fds(fd1, fd2, f1, f2, mode);
    if (close1) u_close(fd1);
    if (close2) u_close(fd2);
    return rc;
}
