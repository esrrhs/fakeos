/* fakeos userland - shuf: shuffle lines or a number range (thin).
 *   shuf [-n N] [-i LO-HI] [-e ARG...] [FILE...]
 * Reads lines into RAM (MAX_LINES), Fisher-Yates shuffle, prints.
 * -i LO-HI: ignore FILE/stdin; emit inclusive decimal range then shuffle.
 * -e: remaining args are input lines (compact; EXEC_MAX_ARGC=8).
 * -n N / -nN: output at most N lines. Seed: clock_gettime + getpid.
 * No -z/-o/--random-source. Default FILE is stdin.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getpid(void);
extern long u_clock_gettime(long clockid, void *tp);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void *malloc(unsigned long size);
extern long strtol_dec(const char *s);
extern void printf(const char *fmt, ...);

enum {
    LINE_MAX = 512,
    MAX_LINES = 4096,
    CLOCK_REALTIME = 0
};

static char *lines[MAX_LINES];
static int nlines;
static long out_limit = -1; /* -1 = all */
static unsigned int rng_state;

static void usage(void) {
    u_write(2, "usage: shuf [-n N] [-i LO-HI] [-e ARG...] [FILE...]\n", 52);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static void rng_seed(void) {
    long ts[2];
    unsigned int s;
    if (u_clock_gettime(CLOCK_REALTIME, ts) != 0) {
        ts[0] = 1;
        ts[1] = 1;
    }
    s = (unsigned int)ts[0] ^ (unsigned int)ts[1]
      ^ ((unsigned int)u_getpid() * 2654435761u);
    if (s == 0) s = 0xA5A5A5A5u;
    rng_state = s;
}

static unsigned int rng_next(void) {
    unsigned int x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* Uniform in [0, bound) for bound > 0. */
static unsigned int rng_below(unsigned int bound) {
    unsigned int x;
    unsigned int lim;
    if (bound <= 1) return 0;
    lim = (0u - bound) % bound;
    do {
        x = rng_next();
    } while (x < lim);
    return x % bound;
}

static int add_line(const char *s) {
    unsigned long n;
    char *p;
    if (nlines >= MAX_LINES) {
        u_write(2, "shuf: too many lines\n", 21);
        return 1;
    }
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (p == 0) {
        u_write(2, "shuf: out of memory\n", 20);
        return 1;
    }
    strcpy(p, s);
    lines[nlines++] = p;
    return 0;
}

static int add_line_n(const char *s, int len) {
    char tmp[LINE_MAX];
    int i;
    if (len >= LINE_MAX) len = LINE_MAX - 1;
    for (i = 0; i < len; i++) tmp[i] = s[i];
    tmp[len] = 0;
    return add_line(tmp);
}

/* Format non-negative decimal into out; returns length. */
static int fmt_ulong(unsigned long v, char *out) {
    char tmp[24];
    int i = 24;
    int n;
    int k;
    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return 1;
    }
    while (v > 0 && i > 0) {
        i--;
        tmp[i] = (char)('0' + (v % 10));
        v /= 10;
    }
    n = 24 - i;
    for (k = 0; k < n; k++) out[k] = tmp[i + k];
    out[n] = 0;
    return n;
}

static int load_fd(long fd) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (add_line_n(line, li) != 0) return 1;
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        if (add_line_n(line, li) != 0) return 1;
    }
    return n < 0 ? 1 : 0;
}

static void shuffle(void) {
    int i;
    for (i = nlines - 1; i > 0; i--) {
        unsigned int j = rng_below((unsigned int)(i + 1));
        char *tmp = lines[i];
        lines[i] = lines[j];
        lines[j] = tmp;
    }
}

static void print_out(void) {
    int i;
    long left = out_limit;
    for (i = 0; i < nlines; i++) {
        if (left == 0) break;
        u_write(1, lines[i], strlen(lines[i]));
        u_write(1, "\n", 1);
        if (left > 0) left--;
    }
}

static int parse_range(const char *s, long *lo, long *hi) {
    int i = 0;
    int dash = -1;
    long a;
    long b;
    char left[32];
    char right[32];
    int L;
    int R;
    int k;
    if (s == 0 || s[0] == 0) return 0;
    while (s[i]) {
        if (s[i] == '-') {
            if (dash >= 0) return 0;
            dash = i;
        } else if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        i++;
    }
    if (dash <= 0 || dash + 1 >= i) return 0;
    L = dash;
    R = i - dash - 1;
    if (L >= 31 || R >= 31 || L == 0 || R == 0) return 0;
    for (k = 0; k < L; k++) left[k] = s[k];
    left[L] = 0;
    for (k = 0; k < R; k++) right[k] = s[dash + 1 + k];
    right[R] = 0;
    if (!is_digits(left) || !is_digits(right)) return 0;
    a = strtol_dec(left);
    b = strtol_dec(right);
    if (a > b) return 0;
    *lo = a;
    *hi = b;
    return 1;
}

static int load_range(long lo, long hi) {
    long v;
    char tmp[24];
    if (hi < lo) return 1;
    if ((unsigned long)(hi - lo) + 1UL > (unsigned long)MAX_LINES) {
        u_write(2, "shuf: range too large\n", 22);
        return 1;
    }
    for (v = lo; v <= hi; v++) {
        fmt_ulong((unsigned long)v, tmp);
        if (add_line(tmp) != 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    int opt_e = 0;
    int opt_i = 0;
    long lo = 0;
    long hi = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-e") == 0) {
            opt_e = 1;
            i++;
            break;
        }
        if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc || !parse_range(argv[i + 1], &lo, &hi)) {
                usage();
                return 1;
            }
            opt_i = 1;
            i += 2;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == 'i' && argv[i][2] != 0) {
            if (!parse_range(argv[i] + 2, &lo, &hi)) {
                usage();
                return 1;
            }
            opt_i = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            out_limit = strtol_dec(argv[i + 1]);
            i += 2;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == 'n' && is_digits(argv[i] + 2)) {
            out_limit = strtol_dec(argv[i] + 2);
            i++;
            continue;
        }
        usage();
        return 1;
    }

    rng_seed();

    if (opt_i) {
        if (opt_e || i < argc) {
            usage();
            return 1;
        }
        if (load_range(lo, hi) != 0) return 1;
    } else if (opt_e) {
        for (; i < argc; i++) {
            if (add_line(argv[i]) != 0) return 1;
        }
    } else if (i >= argc) {
        if (load_fd(0) != 0) return 1;
    } else {
        for (; i < argc; i++) {
            long fd;
            if (strcmp(argv[i], "-") == 0) {
                if (load_fd(0) != 0) rc = 1;
                continue;
            }
            fd = u_open(argv[i], 0, 0);
            if (fd < 0) {
                printf("shuf: %s: no such file\n", argv[i]);
                rc = 1;
                continue;
            }
            if (load_fd(fd) != 0) rc = 1;
            u_close(fd);
        }
    }

    if (rc != 0) return rc;
    shuffle();
    print_out();
    return 0;
}
