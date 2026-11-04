/* fakeos userland - cmp: compare two files byte-by-byte (POSIX-ish subset).
 * Forms: cmp [-l|-s] FILE1 FILE2
 * Exit 0 identical, 1 differ, 2 error. "-" means stdin (at most one).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    MODE_NORMAL = 0,
    MODE_SILENT = 1,
    MODE_VERBOSE = 2,
    BUF = 512
};

static void usage(void) {
    u_write(2, "usage: cmp [-l|-s] FILE1 FILE2\n", 31);
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
    int differ = 0;
    long na;
    long nb;
    int i;

    for (;;) {
        na = u_read(fd1, a, BUF);
        nb = u_read(fd2, b, BUF);
        if (na < 0 || nb < 0) return 2;
        if (na == 0 && nb == 0) return differ ? 1 : 0;
        if (na == 0 || nb == 0) {
            /* One file ended first — treat as differ at first missing byte. */
            if (mode == MODE_SILENT) return 1;
            if (mode == MODE_VERBOSE) {
                /* No more paired bytes to list; fall through to summary. */
            }
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
            i++;
        }
        if (na != nb) {
            /* Unequal short reads with remaining data on one side. */
            if (mode == MODE_SILENT) return 1;
            if (mode != MODE_VERBOSE) {
                printf("cmp: EOF on %s after byte ", na < nb ? n1 : n2);
                emit_dec(byte - 1);
                u_write(1, "\n", 1);
            } else {
                /* Drain remaining bytes of the longer side as EOF. */
                printf("cmp: EOF on %s after byte ", na < nb ? n1 : n2);
                emit_dec(byte - 1);
                u_write(1, "\n", 1);
            }
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

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-") == 0) break;
        if (strcmp(argv[i], "-s") == 0) {
            mode = MODE_SILENT;
            i++;
        } else if (strcmp(argv[i], "-l") == 0) {
            mode = MODE_VERBOSE;
            i++;
        } else {
            usage();
            return 2;
        }
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
    rc = cmp_fds(fd1, fd2, f1, f2, mode);
    if (close1) u_close(fd1);
    if (close2) u_close(fd2);
    return rc;
}
