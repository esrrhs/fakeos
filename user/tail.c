/* fakeos userland - tail: print the last N lines or bytes of files.
 *   tail [-n N|-N] [-c N|-cN] [-q|-v] [FILE...]
 * Default: last 10 lines (ring). -c byte mode: seekable files use
 * lseek+fstat; non-seekable/stdin use a byte ring (capped).
 * -q never headers; -v always headers; default headers when more than
 * one FILE. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_lseek(long fd, long off, long whence);
extern long u_fstat(long fd, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);
extern void printf(const char *fmt, ...);

enum {
    LINE_MAX = 512,
    RING_MAX = 256,
    BYTE_RING_MAX = 8192,
    SEEK_SET = 0,
    SEEK_END = 2,
    ST_SIZE = 48
};

static char ring[RING_MAX][LINE_MAX];
static int ring_len;
static int ring_next;
static int ring_cap;

static char byte_ring[BYTE_RING_MAX];
static unsigned long byte_len;
static unsigned long byte_next;
static unsigned long byte_cap;

static void ring_reset(int cap) {
    ring_len = 0;
    ring_next = 0;
    ring_cap = cap;
    if (ring_cap < 0) ring_cap = 0;
    if (ring_cap > RING_MAX) ring_cap = RING_MAX;
}

static void ring_push_line(const char *line, int n) {
    int i;
    if (ring_cap <= 0) return;
    if (n >= LINE_MAX) n = LINE_MAX - 1;
    for (i = 0; i < n; i++) ring[ring_next][i] = line[i];
    ring[ring_next][n] = 0;
    ring_next = (ring_next + 1) % ring_cap;
    if (ring_len < ring_cap) ring_len++;
}

static void ring_flush(void) {
    int i;
    int start = (ring_len < ring_cap) ? 0 : ring_next;
    for (i = 0; i < ring_len; i++) {
        int idx = (start + i) % ring_cap;
        int n = 0;
        while (ring[idx][n]) n++;
        if (n > 0) u_write(1, ring[idx], (unsigned long)n);
        u_write(1, "\n", 1);
    }
}

static int tail_lines(long fd, long limit) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;

    ring_reset((int)limit);
    if (limit <= 0) {
        while ((n = u_read(fd, buf, sizeof(buf))) > 0) { }
        return n < 0 ? 1 : 0;
    }

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[li] = 0;
                ring_push_line(line, li);
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        ring_push_line(line, li);
    }
    ring_flush();
    return n < 0 ? 1 : 0;
}

static unsigned long load_u64(const char *p) {
    unsigned long v = 0;
    unsigned long i;
    for (i = 0; i < 8; i++) {
        v |= ((unsigned long)(unsigned char)p[i]) << (8 * i);
    }
    return v;
}

static void byte_ring_reset(unsigned long cap) {
    byte_len = 0;
    byte_next = 0;
    byte_cap = cap;
    if (byte_cap > BYTE_RING_MAX) byte_cap = BYTE_RING_MAX;
}

static void byte_ring_push(char c) {
    if (byte_cap == 0) return;
    byte_ring[byte_next] = c;
    byte_next = (byte_next + 1) % byte_cap;
    if (byte_len < byte_cap) byte_len++;
}

static void byte_ring_flush(void) {
    unsigned long i;
    unsigned long start = (byte_len < byte_cap) ? 0 : byte_next;
    for (i = 0; i < byte_len; i++) {
        unsigned long idx = (start + i) % byte_cap;
        u_write(1, &byte_ring[idx], 1);
    }
}

static int tail_bytes_stream(long fd, long limit) {
    char buf[512];
    long n;
    int i;
    unsigned long cap;

    if (limit <= 0) {
        while ((n = u_read(fd, buf, sizeof(buf))) > 0) { }
        return n < 0 ? 1 : 0;
    }
    cap = (unsigned long)limit;
    if (cap > BYTE_RING_MAX) cap = BYTE_RING_MAX;
    byte_ring_reset(cap);
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) byte_ring_push(buf[i]);
    }
    byte_ring_flush();
    return n < 0 ? 1 : 0;
}

static int tail_bytes(long fd, long limit) {
    char st[160];
    long sz;
    long off;
    char buf[512];
    long n;
    long left;

    if (limit <= 0) {
        while ((n = u_read(fd, buf, sizeof(buf))) > 0) { }
        return n < 0 ? 1 : 0;
    }

    /* Prefer seekable path via fstat size + lseek. */
    if (u_fstat(fd, st) == 0) {
        sz = (long)load_u64(st + ST_SIZE);
        if (sz >= 0) {
            off = (sz > limit) ? (sz - limit) : 0;
            if (u_lseek(fd, off, SEEK_SET) == off) {
                left = (sz > limit) ? limit : sz;
                while (left > 0) {
                    long chunk = left < (long)sizeof(buf) ? left : (long)sizeof(buf);
                    n = u_read(fd, buf, (unsigned long)chunk);
                    if (n <= 0) return n < 0 ? 1 : 0;
                    if (u_write(1, buf, (unsigned long)n) < 0) return 1;
                    left -= n;
                }
                return 0;
            }
        }
    }
    /* Fallback: stream ring (stdin / non-seekable). */
    return tail_bytes_stream(fd, limit);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static void usage(void) {
    u_write(2, "usage: tail [-n N|-N] [-c N|-cN] [-q|-v] [FILE...]\n", 51);
}

int main(int argc, char **argv) {
    long limit = 10;
    int byte_mode = 0;
    int quiet = 0;
    int verbose = 0;
    int i = 1;
    int rc = 0;
    int files = 0;
    int nfiles;
    int fi;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            limit = strtol_dec(argv[i + 1]);
            byte_mode = 0;
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            limit = strtol_dec(argv[i + 1]);
            byte_mode = 1;
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
            verbose = 0;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
            quiet = 0;
            i++;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == 'c' && is_digits(argv[i] + 2)) {
            limit = strtol_dec(argv[i] + 2);
            byte_mode = 1;
            i++;
            continue;
        }
        if (argv[i][0] == '-' && is_digits(argv[i] + 1)) {
            limit = strtol_dec(argv[i] + 1);
            byte_mode = 0;
            i++;
            continue;
        }
        break;
    }

    if (i >= argc) {
        return byte_mode ? tail_bytes(0, limit) : tail_lines(0, limit);
    }

    nfiles = argc - i;
    for (fi = i; fi < argc; fi++) {
        long fd;
        const char *name = argv[fi];
        int show_hdr;

        if (strcmp(name, "-") == 0) {
            fd = 0;
            name = "standard input";
        } else {
            fd = u_open(argv[fi], 0, 0);
            if (fd < 0) {
                printf("tail: %s: no such file\n", argv[fi]);
                rc = 1;
                continue;
            }
        }

        show_hdr = 0;
        if (!quiet) {
            if (verbose || nfiles > 1) show_hdr = 1;
        }
        if (show_hdr) {
            if (files > 0) u_write(1, "\n", 1);
            printf("==> %s <==\n", name);
        }

        if (byte_mode) {
            if (tail_bytes(fd, limit) != 0) rc = 1;
        } else {
            if (tail_lines(fd, limit) != 0) rc = 1;
        }
        if (fd != 0) u_close(fd);
        files++;
    }
    return rc;
}
