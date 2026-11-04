/* fakeos userland - hexdump: thin canonical hex+ASCII dump.
 * Forms: hexdump [-Cv] [-n LEN|-nLEN] [-s OFF|-sOFF] [FILE...]
 * Default is -C style (16 bytes/line, hex + ASCII). Duplicate full-width
 * lines collapse to "*" unless -v. stdin if no FILE. Glued -n/-s keep argv
 * under EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_lseek(long fd, long off, long whence);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    WIDTH = 16,
    SEEK_CUR = 1
};

static long max_bytes = -1; /* -1 = unlimited */
static long skip_bytes = 0;
static int show_all = 0;

static void usage(void) {
    u_write(2, "usage: hexdump [-Cv] [-n LEN] [-s OFF] [FILE...]\n", 49);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static void putc1(char c) {
    u_write(1, &c, 1);
}

static void emit_hex8(unsigned long v) {
    const char *d = "0123456789abcdef";
    char tmp[8];
    int i;
    for (i = 7; i >= 0; i--) {
        tmp[i] = d[v & 0xF];
        v >>= 4;
    }
    u_write(1, tmp, 8);
}

static void emit_hex2(unsigned char b) {
    const char *d = "0123456789abcdef";
    char tmp[2];
    tmp[0] = d[(b >> 4) & 0xF];
    tmp[1] = d[b & 0xF];
    u_write(1, tmp, 2);
}

static int same_bytes(const unsigned char *a, const unsigned char *b, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int apply_skip(long fd) {
    long left = skip_bytes;
    unsigned char buf[WIDTH];
    long n;
    long off;
    if (left <= 0) return 0;
    off = u_lseek(fd, left, SEEK_CUR);
    if (off >= 0) return 0;
    while (left > 0) {
        long want = WIDTH;
        if (left < want) want = left;
        n = u_read(fd, buf, (unsigned long)want);
        if (n <= 0) return n < 0 ? 1 : 0;
        left -= n;
    }
    return 0;
}

static void emit_line(unsigned long off, const unsigned char *buf, int n) {
    int i;
    emit_hex8(off);
    putc1(' ');
    putc1(' ');
    for (i = 0; i < WIDTH; i++) {
        if (i == 8) putc1(' ');
        if (i < n) {
            emit_hex2(buf[i]);
            putc1(' ');
        } else {
            u_write(1, "   ", 3);
        }
    }
    putc1(' ');
    putc1('|');
    for (i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c >= 32 && c < 127) putc1((char)c);
        else putc1('.');
    }
    putc1('|');
    putc1('\n');
}

static int hexdump_fd(long fd) {
    unsigned char buf[WIDTH];
    unsigned char prev[WIDTH];
    unsigned long off = 0;
    long left = max_bytes;
    long n;
    int have_prev = 0;
    int suppressing = 0;

    if (apply_skip(fd) != 0) return 1;
    if (skip_bytes > 0) off = (unsigned long)skip_bytes;

    for (;;) {
        long want = WIDTH;
        int i;
        if (left == 0) break;
        if (left > 0 && left < want) want = left;
        n = u_read(fd, buf, (unsigned long)want);
        if (n < 0) return 1;
        if (n == 0) break;

        if (!show_all && have_prev && n == WIDTH &&
            same_bytes(buf, prev, WIDTH)) {
            if (!suppressing) {
                putc1('*');
                putc1('\n');
                suppressing = 1;
            }
            off += (unsigned long)n;
            if (left > 0) left -= n;
            if (n < want) break;
            continue;
        }

        suppressing = 0;
        emit_line(off, buf, (int)n);

        if (n == WIDTH) {
            for (i = 0; i < WIDTH; i++) prev[i] = buf[i];
            have_prev = 1;
        } else {
            have_prev = 0;
        }

        off += (unsigned long)n;
        if (left > 0) left -= n;
        if (n < want) break;
    }
    emit_hex8(off);
    putc1('\n');
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-C") == 0) {
            /* canonical is the only format */
            i++;
        } else if (strcmp(a, "-v") == 0) {
            show_all = 1;
            i++;
        } else if (strcmp(a, "-Cv") == 0 || strcmp(a, "-vC") == 0) {
            show_all = 1;
            i++;
        } else if (strcmp(a, "-n") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            max_bytes = strtol_dec(argv[i + 1]);
            i += 2;
        } else if (a[0] == '-' && a[1] == 'n' && is_digits(a + 2)) {
            max_bytes = strtol_dec(a + 2);
            i++;
        } else if (strcmp(a, "-s") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            skip_bytes = strtol_dec(argv[i + 1]);
            i += 2;
        } else if (a[0] == '-' && a[1] == 's' && is_digits(a + 2)) {
            skip_bytes = strtol_dec(a + 2);
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        return hexdump_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (hexdump_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            printf("hexdump: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (hexdump_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
