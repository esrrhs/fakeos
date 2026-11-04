/* fakeos userland - xxd: thin hex dump / reverse.
 * Forms: xxd [-p] [-r] [-l LEN|-lLEN] [-s OFF|-sOFF] [-c COLS|-cCOLS]
 *            [IN [OUT]]
 * Default: 16-col address dump with ASCII. -p plain continuous hex.
 * -r reverses hex→binary (-rp / -r -p for plain). OUT only with -r
 * (else stdout). stdin if no IN. Glued -l/-s/-c keep EXEC_MAX_ARGC=8.
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
    O_WRONLY = 1,
    O_CREAT = 0x40,
    O_TRUNC = 0x200,
    SEEK_CUR = 1,
    WIDTH_DEFAULT = 16,
    INBUF = 512
};

static int plain = 0;
static int reverse = 0;
static long max_bytes = -1;
static long skip_bytes = 0;
static int cols = WIDTH_DEFAULT;

static void usage(void) {
    u_write(2, "usage: xxd [-pr] [-l LEN] [-s OFF] [-c COLS] [IN [OUT]]\n", 55);
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

static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static int apply_skip(long fd) {
    long left = skip_bytes;
    unsigned char buf[64];
    long n;
    long off;
    if (left <= 0) return 0;
    off = u_lseek(fd, left, SEEK_CUR);
    if (off >= 0) return 0;
    while (left > 0) {
        long want = 64;
        if (left < want) want = left;
        n = u_read(fd, buf, (unsigned long)want);
        if (n <= 0) return n < 0 ? 1 : 0;
        left -= n;
    }
    return 0;
}

static int dump_plain(long fd) {
    unsigned char buf[INBUF];
    long left = max_bytes;
    long n;
    long i;
    long col = 0;

    if (apply_skip(fd) != 0) return 1;
    for (;;) {
        long want = INBUF;
        if (left == 0) break;
        if (left > 0 && left < want) want = left;
        n = u_read(fd, buf, (unsigned long)want);
        if (n < 0) return 1;
        if (n == 0) break;
        for (i = 0; i < n; i++) {
            emit_hex2(buf[i]);
            col += 2;
            if (cols > 0 && col >= cols * 2) {
                putc1('\n');
                col = 0;
            }
        }
        if (left > 0) left -= n;
        if (n < want) break;
    }
    if (col > 0) putc1('\n');
    return 0;
}

static int dump_normal(long fd) {
    unsigned char buf[64];
    unsigned long off = 0;
    long left = max_bytes;
    long n;
    int width = cols;
    int i;

    if (width < 1) width = WIDTH_DEFAULT;
    if (width > 64) width = 64;
    if (apply_skip(fd) != 0) return 1;
    if (skip_bytes > 0) off = (unsigned long)skip_bytes;

    for (;;) {
        long want = width;
        if (left == 0) break;
        if (left > 0 && left < want) want = left;
        n = u_read(fd, buf, (unsigned long)want);
        if (n < 0) return 1;
        if (n == 0) break;

        emit_hex8(off);
        putc1(':');
        putc1(' ');
        for (i = 0; i < width; i++) {
            if (i > 0 && (i % 2) == 0) putc1(' ');
            if (i < (int)n) emit_hex2(buf[i]);
            else u_write(1, "  ", 2);
        }
        u_write(1, "  ", 2);
        for (i = 0; i < (int)n; i++) {
            unsigned char c = buf[i];
            if (c >= 32 && c < 127) putc1((char)c);
            else putc1('.');
        }
        putc1('\n');

        off += (unsigned long)n;
        if (left > 0) left -= n;
        if (n < want) break;
    }
    return 0;
}

/* Reverse: consume hex digits (ignore other chars); write binary to out_fd. */
static int reverse_fd(long in_fd, long out_fd) {
    unsigned char buf[INBUF];
    unsigned char out[INBUF];
    long left = max_bytes;
    long n;
    long i;
    int oi = 0;
    int have_hi = 0;
    int hi = 0;
    long written = 0;

    if (apply_skip(in_fd) != 0) return 1;

    for (;;) {
        long want = INBUF;
        if (left == 0 && max_bytes >= 0) break;
        n = u_read(in_fd, buf, (unsigned long)want);
        if (n < 0) return 1;
        if (n == 0) break;
        for (i = 0; i < n; i++) {
            int v = hex_val(buf[i]);
            if (v < 0) continue;
            if (!have_hi) {
                hi = v;
                have_hi = 1;
            } else {
                out[oi++] = (unsigned char)((hi << 4) | v);
                have_hi = 0;
                written++;
                if (max_bytes >= 0 && written >= max_bytes) {
                    if (oi > 0) {
                        if (u_write(out_fd, out, (unsigned long)oi) < 0) return 1;
                    }
                    return 0;
                }
                if (oi >= INBUF) {
                    if (u_write(out_fd, out, (unsigned long)oi) < 0) return 1;
                    oi = 0;
                }
            }
        }
    }
    if (oi > 0) {
        if (u_write(out_fd, out, (unsigned long)oi) < 0) return 1;
    }
    return 0;
}

static int parse_flags(const char *a) {
    int j;
    for (j = 1; a[j]; j++) {
        if (a[j] == 'p') plain = 1;
        else if (a[j] == 'r') reverse = 1;
        else return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    const char *in_path = 0;
    const char *out_path = 0;
    long in_fd;
    long out_fd = 1;
    int rc;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-p") == 0 || strcmp(a, "-r") == 0 ||
            strcmp(a, "-pr") == 0 || strcmp(a, "-rp") == 0) {
            if (parse_flags(a) != 0) {
                usage();
                return 1;
            }
            i++;
        } else if (strcmp(a, "-l") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            max_bytes = strtol_dec(argv[i + 1]);
            i += 2;
        } else if (a[0] == '-' && a[1] == 'l' && is_digits(a + 2)) {
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
        } else if (strcmp(a, "-c") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            cols = (int)strtol_dec(argv[i + 1]);
            i += 2;
        } else if (a[0] == '-' && a[1] == 'c' && is_digits(a + 2)) {
            cols = (int)strtol_dec(a + 2);
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i < argc) {
        in_path = argv[i++];
    }
    if (i < argc) {
        out_path = argv[i++];
    }
    if (i < argc) {
        usage();
        return 1;
    }
    if (out_path != 0 && !reverse) {
        usage();
        return 1;
    }

    if (in_path == 0 || strcmp(in_path, "-") == 0) {
        in_fd = 0;
    } else {
        in_fd = u_open(in_path, O_RDONLY, 0);
        if (in_fd < 0) {
            printf("xxd: %s: no such file\n", in_path);
            return 1;
        }
    }

    if (reverse) {
        if (out_path != 0 && strcmp(out_path, "-") != 0) {
            out_fd = u_open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0x1A4);
            if (out_fd < 0) {
                printf("xxd: %s: cannot create\n", out_path);
                if (in_fd > 2) u_close(in_fd);
                return 1;
            }
        }
        rc = reverse_fd(in_fd, out_fd);
        if (out_fd > 2) u_close(out_fd);
    } else if (plain) {
        rc = dump_plain(in_fd);
    } else {
        rc = dump_normal(in_fd);
    }
    if (in_fd > 2) u_close(in_fd);
    return rc;
}
