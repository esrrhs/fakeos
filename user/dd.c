/* fakeos userland - dd: copy and convert a file (thin POSIX-ish subset).
 * Forms: dd [if=FILE] [of=FILE] [bs=N] [count=N] [skip=N] [seek=N] [status=none]
 * Default: if/of = stdin/stdout, bs=512. N is decimal; optional k/K (=1024) or
 * b (=512) suffix. skip/seek are in input/output blocks. status=none quiets
 * the records summary on stderr.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_lseek(long fd, long off, long whence);
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, unsigned long n);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200,
    SEEK_SET = 0,
    MODE_0644 = 0x1A4,
    BUF_CAP = 8192
};

static void usage(void) {
    u_write(2, "usage: dd [if=FILE] [of=FILE] [bs=N] [count=N] [skip=N] [seek=N] [status=none]\n", 79);
}

static int parse_num(const char *s, unsigned long *out) {
    unsigned long v = 0;
    int i = 0;
    char c;
    if (s == 0 || s[0] == 0) return 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (unsigned long)(s[i] - '0');
        i++;
    }
    if (i == 0) return 0;
    c = s[i];
    if (c == 'k' || c == 'K') {
        v = v * 1024;
        i++;
    } else if (c == 'b' || c == 'B') {
        v = v * 512;
        i++;
    } else if (c == 'M') {
        v = v * 1024 * 1024;
        i++;
    }
    if (s[i] != 0) return 0;
    *out = v;
    return 1;
}

static void emit_dec_fd(long fd, unsigned long v) {
    char tmp[24];
    int i = 24;
    if (v == 0) {
        u_write(fd, "0", 1);
        return;
    }
    while (v > 0 && i > 0) {
        i--;
        tmp[i] = (char)('0' + (v % 10));
        v /= 10;
    }
    u_write(fd, tmp + i, (unsigned long)(24 - i));
}

static void report(unsigned long in_full, unsigned long in_partial,
                   unsigned long out_full, unsigned long out_partial) {
    emit_dec_fd(2, in_full);
    u_write(2, "+", 1);
    emit_dec_fd(2, in_partial);
    u_write(2, " records in\n", 12);
    emit_dec_fd(2, out_full);
    u_write(2, "+", 1);
    emit_dec_fd(2, out_partial);
    u_write(2, " records out\n", 13);
}

static int eq_key(const char *arg, const char *key) {
    unsigned long n = strlen(key);
    return strncmp(arg, key, n) == 0;
}

int main(int argc, char **argv) {
    const char *in_path = 0;
    const char *out_path = 0;
    unsigned long bs = 512;
    unsigned long count = (unsigned long)-1; /* unlimited */
    unsigned long skip = 0;
    unsigned long seek = 0;
    int quiet = 0;
    long in_fd = 0;
    long out_fd = 1;
    int close_in = 0;
    int close_out = 0;
    unsigned char buf[BUF_CAP];
    unsigned long in_full = 0;
    unsigned long in_partial = 0;
    unsigned long out_full = 0;
    unsigned long out_partial = 0;
    unsigned long copied = 0;
    int i;
    long n;
    long w;
    long off;

    for (i = 1; i < argc; i++) {
        if (eq_key(argv[i], "if=")) {
            in_path = argv[i] + 3;
        } else if (eq_key(argv[i], "of=")) {
            out_path = argv[i] + 3;
        } else if (eq_key(argv[i], "bs=")) {
            if (!parse_num(argv[i] + 3, &bs) || bs == 0) {
                usage();
                return 1;
            }
            if (bs > BUF_CAP) {
                u_write(2, "dd: bs too large (max 8192)\n", 28);
                return 1;
            }
        } else if (eq_key(argv[i], "count=")) {
            if (!parse_num(argv[i] + 6, &count)) {
                usage();
                return 1;
            }
        } else if (eq_key(argv[i], "skip=")) {
            if (!parse_num(argv[i] + 5, &skip)) {
                usage();
                return 1;
            }
        } else if (eq_key(argv[i], "seek=")) {
            if (!parse_num(argv[i] + 5, &seek)) {
                usage();
                return 1;
            }
        } else if (eq_key(argv[i], "status=")) {
            if (strcmp(argv[i] + 7, "none") == 0 ||
                strcmp(argv[i] + 7, "noxfer") == 0) {
                quiet = 1;
            } else {
                usage();
                return 1;
            }
        } else {
            usage();
            return 1;
        }
    }

    if (in_path != 0 && strcmp(in_path, "-") != 0) {
        in_fd = u_open(in_path, O_RDONLY, 0);
        if (in_fd < 0) {
            printf("dd: %s: cannot open\n", in_path);
            return 1;
        }
        close_in = 1;
    }
    if (out_path != 0 && strcmp(out_path, "-") != 0) {
        long flags = O_WRONLY | O_CREAT;
        if (seek == 0) flags = flags | O_TRUNC;
        out_fd = u_open(out_path, flags, MODE_0644);
        if (out_fd < 0) {
            printf("dd: %s: cannot create\n", out_path);
            if (close_in) u_close(in_fd);
            return 1;
        }
        close_out = 1;
    }

    if (skip > 0) {
        off = u_lseek(in_fd, (long)(skip * bs), SEEK_SET);
        if (off < 0) {
            /* Non-seekable: read and discard. */
            unsigned long left = skip;
            while (left > 0) {
                n = u_read(in_fd, buf, bs);
                if (n <= 0) break;
                left--;
            }
        }
    }
    if (seek > 0) {
        off = u_lseek(out_fd, (long)(seek * bs), SEEK_SET);
        if (off < 0) {
            u_write(2, "dd: seek failed\n", 16);
            if (close_in) u_close(in_fd);
            if (close_out) u_close(out_fd);
            return 1;
        }
    }

    while (count == (unsigned long)-1 || copied < count) {
        n = u_read(in_fd, buf, bs);
        if (n < 0) {
            u_write(2, "dd: read error\n", 15);
            if (close_in) u_close(in_fd);
            if (close_out) u_close(out_fd);
            if (!quiet) report(in_full, in_partial, out_full, out_partial);
            return 1;
        }
        if (n == 0) break;
        if ((unsigned long)n == bs) in_full++;
        else in_partial++;
        w = u_write(out_fd, buf, (unsigned long)n);
        if (w < 0 || w != n) {
            u_write(2, "dd: write error\n", 16);
            if (close_in) u_close(in_fd);
            if (close_out) u_close(out_fd);
            if (!quiet) report(in_full, in_partial, out_full, out_partial);
            return 1;
        }
        if ((unsigned long)w == bs) out_full++;
        else out_partial++;
        copied++;
    }

    if (close_in) u_close(in_fd);
    if (close_out) u_close(out_fd);
    if (!quiet) report(in_full, in_partial, out_full, out_partial);
    return 0;
}
