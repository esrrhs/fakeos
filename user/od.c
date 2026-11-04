/* fakeos userland - od: thin octal/hex dump (POSIX-ish subset).
 * Forms: od [-v] [-A n|o|x|d] [-t x1|o1|c|u1|d1] [-N COUNT|-NCOUNT]
 *        [-j SKIP|-jSKIP] [FILE...]
 * Default: -A o -t x1, 16 bytes per line. stdin if no FILE.
 * -j skips SKIP input bytes before dumping (seek when possible, else read-discard).
 * Duplicate full-width lines are collapsed to a single "*" (disable with -v).
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
    ADDR_NONE = 0,
    ADDR_OCT = 1,
    ADDR_HEX = 2,
    ADDR_DEC = 3,
    TYPE_X1 = 0,
    TYPE_O1 = 1,
    TYPE_C = 2,
    TYPE_U1 = 3,
    TYPE_D1 = 4
};

static int addr_mode = ADDR_OCT;
static int type_mode = TYPE_X1;
static long max_bytes = -1; /* -1 = unlimited */
static long skip_bytes = 0;
static int show_all = 0; /* -v: do not collapse duplicate lines */

static void usage(void) {
    u_write(2, "usage: od [-v] [-A n|o|x|d] [-t x1|o1|c|u1|d1] [-N COUNT] [-j SKIP] [FILE...]\n", 78);
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

static void emit_oct7(unsigned long v) {
    char tmp[8];
    int i;
    for (i = 6; i >= 0; i--) {
        tmp[i] = (char)('0' + (v & 7));
        v >>= 3;
    }
    u_write(1, tmp, 7);
}

static void emit_hex7(unsigned long v) {
    const char *d = "0123456789abcdef";
    char tmp[8];
    int i;
    for (i = 6; i >= 0; i--) {
        tmp[i] = d[v & 0xF];
        v >>= 4;
    }
    u_write(1, tmp, 7);
}

static void emit_dec7(unsigned long v) {
    char tmp[8];
    unsigned long x = v;
    int i;
    for (i = 6; i >= 0; i--) {
        tmp[i] = (char)('0' + (x % 10));
        x /= 10;
    }
    u_write(1, tmp, 7);
}

static void emit_addr(unsigned long off) {
    if (addr_mode == ADDR_NONE) return;
    if (addr_mode == ADDR_OCT) emit_oct7(off);
    else if (addr_mode == ADDR_HEX) emit_hex7(off);
    else emit_dec7(off);
    putc1(' ');
}

static void emit_hex2(unsigned char b) {
    const char *d = "0123456789abcdef";
    char tmp[2];
    tmp[0] = d[(b >> 4) & 0xF];
    tmp[1] = d[b & 0xF];
    u_write(1, tmp, 2);
}

static void emit_oct3(unsigned char b) {
    char tmp[3];
    tmp[0] = (char)('0' + ((b >> 6) & 7));
    tmp[1] = (char)('0' + ((b >> 3) & 7));
    tmp[2] = (char)('0' + (b & 7));
    u_write(1, tmp, 3);
}

/* Unsigned decimal byte, width 3, space-padded (GNU-ish). */
static void emit_u1(unsigned char b) {
    char tmp[3];
    unsigned v = b;
    tmp[2] = (char)('0' + (v % 10));
    v /= 10;
    tmp[1] = (char)('0' + (v % 10));
    v /= 10;
    tmp[0] = (char)('0' + (v % 10));
    if (tmp[0] == '0') {
        tmp[0] = ' ';
        if (tmp[1] == '0') tmp[1] = ' ';
    }
    u_write(1, tmp, 3);
}

/* Signed decimal byte, width 4, space-padded (GNU-ish). */
static void emit_d1(unsigned char b) {
    char tmp[4];
    int v = (int)(signed char)b;
    int neg = 0;
    int absv;
    int i;
    if (v < 0) {
        neg = 1;
        absv = -v;
    } else {
        absv = v;
    }
    for (i = 3; i >= 0; i--) tmp[i] = ' ';
    i = 3;
    if (absv == 0) {
        tmp[i] = '0';
    } else {
        while (absv > 0 && i >= 0) {
            tmp[i] = (char)('0' + (absv % 10));
            absv /= 10;
            i--;
        }
        if (neg && i >= 0) tmp[i] = '-';
    }
    u_write(1, tmp, 4);
}

static void emit_c(unsigned char b) {
    if (b == '\\') {
        u_write(1, "\\\\", 2);
    } else if (b == '\n') {
        u_write(1, "\\n", 2);
    } else if (b == '\t') {
        u_write(1, "\\t", 2);
    } else if (b == '\r') {
        u_write(1, "\\r", 2);
    } else if (b == '\0') {
        u_write(1, "\\0", 2);
    } else if (b >= 32 && b < 127) {
        putc1((char)b);
    } else {
        putc1('\\');
        emit_oct3(b);
    }
}

static void emit_byte(unsigned char b) {
    if (type_mode == TYPE_X1) emit_hex2(b);
    else if (type_mode == TYPE_O1) emit_oct3(b);
    else if (type_mode == TYPE_U1) emit_u1(b);
    else if (type_mode == TYPE_D1) emit_d1(b);
    else emit_c(b);
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
    off = u_lseek(fd, left, 1 /* SEEK_CUR */);
    if (off >= 0) return 0;
    /* Non-seekable: read and discard. */
    while (left > 0) {
        long want = WIDTH;
        if (left < want) want = left;
        n = u_read(fd, buf, (unsigned long)want);
        if (n <= 0) return n < 0 ? 1 : 0;
        left -= n;
    }
    return 0;
}

static int od_fd(long fd) {
    unsigned char buf[WIDTH];
    unsigned char prev[WIDTH];
    unsigned long off = 0;
    long left = max_bytes;
    long n;
    int i;
    int have_prev = 0;
    int suppressing = 0;

    if (apply_skip(fd) != 0) return 1;
    if (skip_bytes > 0) off = (unsigned long)skip_bytes;

    for (;;) {
        long want = WIDTH;
        if (left == 0) break;
        if (left > 0 && left < want) want = left;
        n = u_read(fd, buf, (unsigned long)want);
        if (n < 0) return 1;
        if (n == 0) break;

        /* Duplicate full-width line collapse (GNU od default; -v disables). */
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
        emit_addr(off);
        for (i = 0; i < (int)n; i++) {
            if (i > 0) putc1(' ');
            emit_byte(buf[i]);
        }
        putc1('\n');

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
    if (addr_mode != ADDR_NONE) {
        /* Final address like GNU od */
        emit_addr(off);
        putc1('\n');
    }
    return 0;
}

static int parse_type(const char *s) {
    if (strcmp(s, "x1") == 0) { type_mode = TYPE_X1; return 0; }
    if (strcmp(s, "o1") == 0) { type_mode = TYPE_O1; return 0; }
    if (strcmp(s, "c") == 0) { type_mode = TYPE_C; return 0; }
    if (strcmp(s, "u1") == 0) { type_mode = TYPE_U1; return 0; }
    if (strcmp(s, "d1") == 0) { type_mode = TYPE_D1; return 0; }
    return -1;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    int files = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-v") == 0) {
            show_all = 1;
            i++;
        } else if (strcmp(argv[i], "-A") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            if (strcmp(argv[i + 1], "n") == 0) addr_mode = ADDR_NONE;
            else if (strcmp(argv[i + 1], "o") == 0) addr_mode = ADDR_OCT;
            else if (strcmp(argv[i + 1], "x") == 0) addr_mode = ADDR_HEX;
            else if (strcmp(argv[i + 1], "d") == 0) addr_mode = ADDR_DEC;
            else {
                usage();
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc || parse_type(argv[i + 1]) != 0) {
                usage();
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-N") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            max_bytes = strtol_dec(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-An") == 0) {
            addr_mode = ADDR_NONE;
            i++;
        } else if (strcmp(argv[i], "-Ax") == 0) {
            addr_mode = ADDR_HEX;
            i++;
        } else if (strcmp(argv[i], "-Ao") == 0) {
            addr_mode = ADDR_OCT;
            i++;
        } else if (strcmp(argv[i], "-Ad") == 0) {
            addr_mode = ADDR_DEC;
            i++;
        } else if (strcmp(argv[i], "-tx1") == 0) {
            type_mode = TYPE_X1;
            i++;
        } else if (strcmp(argv[i], "-to1") == 0) {
            type_mode = TYPE_O1;
            i++;
        } else if (strcmp(argv[i], "-tc") == 0) {
            type_mode = TYPE_C;
            i++;
        } else if (strcmp(argv[i], "-tu1") == 0) {
            type_mode = TYPE_U1;
            i++;
        } else if (strcmp(argv[i], "-td1") == 0) {
            type_mode = TYPE_D1;
            i++;
        } else if (strcmp(argv[i], "-vtx1") == 0) {
            show_all = 1;
            type_mode = TYPE_X1;
            i++;
        } else if (strcmp(argv[i], "-vto1") == 0) {
            show_all = 1;
            type_mode = TYPE_O1;
            i++;
        } else if (strcmp(argv[i], "-vtc") == 0) {
            show_all = 1;
            type_mode = TYPE_C;
            i++;
        } else if (strcmp(argv[i], "-vtu1") == 0) {
            show_all = 1;
            type_mode = TYPE_U1;
            i++;
        } else if (strcmp(argv[i], "-vtd1") == 0) {
            show_all = 1;
            type_mode = TYPE_D1;
            i++;
        } else if (argv[i][0] == '-' && argv[i][1] == 'N' && argv[i][2] != 0 &&
                   is_digits(argv[i] + 2)) {
            max_bytes = strtol_dec(argv[i] + 2);
            i++;
        } else if (strcmp(argv[i], "-j") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            skip_bytes = strtol_dec(argv[i + 1]);
            i += 2;
        } else if (argv[i][0] == '-' && argv[i][1] == 'j' && argv[i][2] != 0 &&
                   is_digits(argv[i] + 2)) {
            skip_bytes = strtol_dec(argv[i] + 2);
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        return od_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (od_fd(0) != 0) rc = 1;
            files++;
            continue;
        }
        fd = u_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            printf("od: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (od_fd(fd) != 0) rc = 1;
        u_close(fd);
        files++;
    }
    return rc;
}
