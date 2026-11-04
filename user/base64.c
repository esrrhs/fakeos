/* fakeos userland - base64: encode/decode (thin POSIX/GNU subset).
 * Forms: base64 [-w COLS|-wCOLS] [FILE...]
 *        base64 -d [FILE...]
 * Encode wraps at COLS columns (default 76) with a trailing newline when the
 * last line is open; -w 0 disables wrapping entirely (no trailing newline).
 * Decode ignores whitespace; rejects other non-alphabet. stdin if no FILE.
 * Glued -wCOLS keeps argv under EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);

enum { WRAP_DEFAULT = 76, INBUF = 512, OUTBUF = 1024 };

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int decode_mode;
static int wrap_cols = WRAP_DEFAULT; /* 0 = no wrap */
static signed char decmap[128];
static int map_ready;

static void usage(void) {
    u_write(2, "usage: base64 [-d] [-w COLS] [FILE...]\n", 37);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static void init_map(void) {
    int i;
    if (map_ready) return;
    for (i = 0; i < 128; i++) decmap[i] = -1;
    for (i = 0; i < 64; i++) decmap[(unsigned char)b64tab[i]] = (signed char)i;
    map_ready = 1;
}

static int is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int encode_fd(long fd) {
    unsigned char in[INBUF];
    char out[OUTBUF];
    long n;
    int hold_n = 0;
    unsigned char hold[3];
    int col = 0;
    int any = 0;
    long i;
    int oi;

    while ((n = u_read(fd, in, INBUF)) > 0) {
        i = 0;
        while (hold_n < 3 && i < n) {
            hold[hold_n++] = in[i++];
        }
        oi = 0;
        while (hold_n == 3) {
            unsigned long v = ((unsigned long)hold[0] << 16) |
                             ((unsigned long)hold[1] << 8) |
                             (unsigned long)hold[2];
            out[oi++] = b64tab[(v >> 18) & 63];
            out[oi++] = b64tab[(v >> 12) & 63];
            out[oi++] = b64tab[(v >> 6) & 63];
            out[oi++] = b64tab[v & 63];
            any = 1;
            col += 4;
            if (wrap_cols > 0 && col >= wrap_cols) {
                out[oi++] = '\n';
                col = 0;
            }
            hold_n = 0;
            while (hold_n < 3 && i < n) {
                hold[hold_n++] = in[i++];
            }
            if (oi + 8 >= OUTBUF) {
                if (u_write(1, out, (unsigned long)oi) < 0) return 1;
                oi = 0;
            }
        }
        if (oi > 0) {
            if (u_write(1, out, (unsigned long)oi) < 0) return 1;
        }
    }
    if (n < 0) return 1;

    oi = 0;
    if (hold_n == 1) {
        unsigned long v = (unsigned long)hold[0] << 16;
        out[oi++] = b64tab[(v >> 18) & 63];
        out[oi++] = b64tab[(v >> 12) & 63];
        out[oi++] = '=';
        out[oi++] = '=';
        any = 1;
        col += 4;
    } else if (hold_n == 2) {
        unsigned long v = ((unsigned long)hold[0] << 16) |
                         ((unsigned long)hold[1] << 8);
        out[oi++] = b64tab[(v >> 18) & 63];
        out[oi++] = b64tab[(v >> 12) & 63];
        out[oi++] = b64tab[(v >> 6) & 63];
        out[oi++] = '=';
        any = 1;
        col += 4;
    }
    /* Trailing newline only when wrapping is on and the last line is open.
     * Exact WRAP already emitted a newline during streaming; -w0 never wraps. */
    if (wrap_cols > 0 && any && col != 0) {
        out[oi++] = '\n';
    }
    if (oi > 0) {
        if (u_write(1, out, (unsigned long)oi) < 0) return 1;
    }
    return 0;
}

static int decode_fd(long fd) {
    unsigned char in[INBUF];
    unsigned char out[OUTBUF];
    long n;
    int qi = 0;
    int q[4];
    int done = 0;
    long i;
    int oi;
    int v;

    init_map();
    oi = 0;
    while ((n = u_read(fd, in, INBUF)) > 0) {
        for (i = 0; i < n; i++) {
            unsigned char c = in[i];
            if (is_ws(c)) continue;
            if (done) return 1;
            if (c == '=') {
                q[qi++] = -2; /* pad marker */
            } else {
                if (c >= 128) return 1;
                v = decmap[c];
                if (v < 0) return 1;
                q[qi++] = v;
            }
            if (qi < 4) continue;
            /* Emit from quartet. */
            if (q[0] < 0 || q[1] < 0) return 1;
            if (q[2] == -2 && q[3] != -2) return 1;
            out[oi++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
            if (q[2] >= 0) {
                out[oi++] = (unsigned char)(((q[1] & 15) << 4) | (q[2] >> 2));
                if (q[3] >= 0) {
                    out[oi++] = (unsigned char)(((q[2] & 3) << 6) | q[3]);
                } else {
                    /* one pad: two data bytes */
                    done = 1;
                }
            } else {
                /* two pads: one data byte; q[3] must be pad */
                if (q[3] != -2) return 1;
                done = 1;
            }
            qi = 0;
            if (oi + 4 >= OUTBUF) {
                if (u_write(1, out, (unsigned long)oi) < 0) return 1;
                oi = 0;
            }
        }
    }
    if (n < 0) return 1;
    if (qi != 0) return 1;
    if (oi > 0) {
        if (u_write(1, out, (unsigned long)oi) < 0) return 1;
    }
    return 0;
}

static int process_fd(long fd) {
    if (decode_mode) return decode_fd(fd);
    return encode_fd(fd);
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
        if (strcmp(a, "-") == 0) {
            break;
        }
        if (strcmp(a, "-d") == 0) {
            decode_mode = 1;
            i++;
            continue;
        }
        if (strcmp(a, "-w") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            wrap_cols = (int)strtol_dec(argv[i + 1]);
            if (wrap_cols < 0) wrap_cols = 0;
            i += 2;
            continue;
        }
        if (a[1] == 'w' && a[2] != 0 && is_digits(a + 2)) {
            wrap_cols = (int)strtol_dec(a + 2);
            if (wrap_cols < 0) wrap_cols = 0;
            i++;
            continue;
        }
        usage();
        return 1;
    }

    if (i >= argc) {
        return process_fd(0) != 0 ? 1 : 0;
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (process_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "base64: open failed\n", 20);
            rc = 1;
            continue;
        }
        if (process_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
