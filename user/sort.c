/* fakeos userland - sort: lexicographic line sort (thin POSIX subset).
 * Forms: sort [-rnufbhVRz] [-k N|-kN] [FILE...]
 * -r reverse; -n numeric (leading optional sign/digits); -u unique by key
 * (suppress consecutive equal keys after sort); -f fold case (A-Z→a-z) for
 * string compares / tie-breaks; -b ignore leading blanks on the sort key;
 * -h human-numeric (GNU; integer + optional SI suffix kKMGTPE as 1024^n;
 * optional trailing i ignored; overrides -n for key compare); -V version
 * sort (GNU; digit runs compared numerically, non-digits as chars; more
 * leading zeros sorts first; overrides -n/-h for key compare); -R random
 * order (GNU; Fisher-Yates shuffle; other compare flags ignored for
 * ordering; -u still unique by key after shuffle); -z NUL-terminated
 * records (input+output; pairs with xargs -0); -k N sort by blank-separated
 * field N (1-based; missing field sorts as empty / 0). Loads into RAM.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void *malloc(unsigned long size);
extern void free(void *ptr);
extern void printf(const char *fmt, ...);
extern long strtol_dec(const char *s);
extern long u_getpid(void);
extern long u_clock_gettime(long clockid, void *tp);

enum {
    LINE_MAX = 512,
    MAX_LINES = 4096,
    CLOCK_REALTIME = 0
};

static int opt_r;
static int opt_n;
static int opt_u;
static int opt_f;
static int opt_b; /* ignore leading blanks on key */
static int opt_h; /* human-numeric (GNU SI suffixes) */
static int opt_V; /* version sort (GNU) */
static int opt_R; /* random order (GNU) */
static int opt_z; /* NUL-terminated records */
static int opt_k; /* 0 = whole line; else 1-based field */
static char *lines[MAX_LINES];
static int nlines;
static unsigned int rng_state;

static void usage(void) {
    u_write(2, "usage: sort [-rnufbhVRz] [-k N] [FILE...]\n", 42);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

/* Point at blank-separated field N (1-based) within s; set *len.
 * Missing field -> empty (len 0). */
static const char *field_at(const char *s, int n, int *len) {
    int f = 0;
    int i = 0;
    int start;
    *len = 0;
    if (n < 1) {
        *len = (int)strlen(s);
        return s;
    }
    while (s[i]) {
        while (s[i] == ' ' || s[i] == '\t') i++;
        if (s[i] == 0) break;
        f++;
        start = i;
        while (s[i] && s[i] != ' ' && s[i] != '\t') i++;
        if (f == n) {
            *len = i - start;
            return s + start;
        }
    }
    return s + i; /* empty */
}

static long parse_num_n(const char *s, int n) {
    long v = 0;
    int i = 0;
    int neg = 0;
    int end = n;
    if (n < 0) end = (int)strlen(s);
    while (i < end && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < end && s[i] == '-') {
        neg = 1;
        i++;
    } else if (i < end && s[i] == '+') {
        i++;
    }
    if (i >= end || s[i] < '0' || s[i] > '9') {
        return 0;
    }
    while (i < end && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    return neg ? -v : v;
}

static long parse_num(const char *s) {
    return parse_num_n(s, -1);
}

/* Human-numeric key: optional sign + digits + optional SI suffix (1024^n).
 * Thin: no fractional part. Trailing 'i' (Ki/Mi) ignored. */
static int human_suffix_exp(char c) {
    if (c == 'k' || c == 'K') return 1;
    if (c == 'm' || c == 'M') return 2;
    if (c == 'g' || c == 'G') return 3;
    if (c == 't' || c == 'T') return 4;
    if (c == 'p' || c == 'P') return 5;
    if (c == 'e' || c == 'E') return 6;
    return -1;
}

static void parse_human_n(const char *s, int n, long *mant_out, int *exp_out, int *neg_out) {
    int i = 0;
    int end = n;
    int neg = 0;
    long v = 0;
    int exp = 0;
    int se;
    if (n < 0) end = (int)strlen(s);
    while (i < end && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < end && s[i] == '-') {
        neg = 1;
        i++;
    } else if (i < end && s[i] == '+') {
        i++;
    }
    if (i >= end || s[i] < '0' || s[i] > '9') {
        *mant_out = 0;
        *exp_out = 0;
        *neg_out = 0;
        return;
    }
    while (i < end && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    if (i < end) {
        se = human_suffix_exp(s[i]);
        if (se >= 0) {
            exp = se;
            i++;
            if (i < end && (s[i] == 'i' || s[i] == 'I')) i++;
        }
    }
    *mant_out = v;
    *exp_out = exp;
    *neg_out = neg;
}

static void parse_human(const char *s, long *mant_out, int *exp_out, int *neg_out) {
    parse_human_n(s, -1, mant_out, exp_out, neg_out);
}

/* Compare non-negative (mant, exp) pairs; exp is power-of-1024.
 * Expand the higher-exp mantissa downward (×1024, exp--) to the lower
 * exponent so both are in the same unit, then compare mantissas. */
static int cmp_human_pos(long m1, int e1, long m2, int e2) {
    while (e1 > e2) {
        if (m1 > 0x7fffffffffffffffL / 1024L) return 1;
        m1 *= 1024L;
        e1--;
    }
    while (e2 > e1) {
        if (m2 > 0x7fffffffffffffffL / 1024L) return -1;
        m2 *= 1024L;
        e2--;
    }
    if (m1 < m2) return -1;
    if (m1 > m2) return 1;
    return 0;
}

static int cmp_human_vals(long m1, int e1, int n1, long m2, int e2, int n2) {
    int c;
    if (n1 && !n2) return -1;
    if (!n1 && n2) return 1;
    c = cmp_human_pos(m1, e1, m2, e2);
    if (n1) return -c;
    return c;
}

static char fold_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* Version compare (thin GNU -V): digit runs as numbers; longer leading-zero
 * prefix sorts first when values equal; other chars by byte (honors -f). */
static int is_ver_digit(char c) {
    return c >= '0' && c <= '9';
}

static int cmp_version_n(const char *a, int al, const char *b, int bl) {
    int i = 0;
    int j = 0;
    if (al < 0) al = (int)strlen(a);
    if (bl < 0) bl = (int)strlen(b);
    while (i < al && j < bl) {
        if (is_ver_digit(a[i]) && is_ver_digit(b[j])) {
            int zi = 0;
            int zj = 0;
            int si;
            int sj;
            int li;
            int lj;
            int k;
            while (i < al && a[i] == '0') { i++; zi++; }
            while (j < bl && b[j] == '0') { j++; zj++; }
            si = i;
            sj = j;
            while (i < al && is_ver_digit(a[i])) i++;
            while (j < bl && is_ver_digit(b[j])) j++;
            li = i - si;
            lj = j - sj;
            if (li != lj) return li < lj ? -1 : 1;
            for (k = 0; k < li; k++) {
                if (a[si + k] != b[sj + k]) {
                    return (unsigned char)a[si + k] < (unsigned char)b[sj + k] ? -1 : 1;
                }
            }
            if (zi != zj) return zi > zj ? -1 : 1;
        } else {
            unsigned char ca = (unsigned char)(opt_f ? fold_ch(a[i]) : a[i]);
            unsigned char cb = (unsigned char)(opt_f ? fold_ch(b[j]) : b[j]);
            if (ca != cb) return (int)ca - (int)cb;
            i++;
            j++;
        }
    }
    if (i < al) return 1;
    if (j < bl) return -1;
    return 0;
}

static int cmp_version(const char *a, const char *b) {
    return cmp_version_n(a, -1, b, -1);
}

static const char *skip_blanks(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Key start (+ optional length for -k). Applies -b blank skip. */
static const char *sort_key(const char *line, int *len) {
    if (opt_k > 0) {
        const char *f = field_at(line, opt_k, len);
        if (opt_b) {
            while (*len > 0 && (*f == ' ' || *f == '\t')) {
                f++;
                (*len)--;
            }
        }
        return f;
    }
    if (opt_b) line = skip_blanks(line);
    if (len) *len = -1;
    return line;
}

static int cmp_key_str(const char *a, int al, const char *b, int bl) {
    int i;
    int n = al < bl ? al : bl;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)(opt_f ? fold_ch(a[i]) : a[i]);
        unsigned char cb = (unsigned char)(opt_f ? fold_ch(b[i]) : b[i]);
        if (ca != cb) return (int)ca - (int)cb;
    }
    return al - bl;
}

static int cmp_str(const char *a, const char *b) {
    int i;
    if (!opt_f) return strcmp(a, b);
    for (i = 0; ; i++) {
        unsigned char ca = (unsigned char)fold_ch(a[i]);
        unsigned char cb = (unsigned char)fold_ch(b[i]);
        if (ca != cb) return (int)ca - (int)cb;
        if (a[i] == 0) return 0;
    }
}

static int cmp_lines(const char *a, const char *b) {
    int c;
    int al, bl;
    const char *ka;
    const char *kb;
    if (opt_k > 0) {
        ka = sort_key(a, &al);
        kb = sort_key(b, &bl);
        if (opt_V) {
            c = cmp_version_n(ka, al, kb, bl);
        } else if (opt_h) {
            long ma, mb;
            int ea, eb, na, nb;
            parse_human_n(ka, al, &ma, &ea, &na);
            parse_human_n(kb, bl, &mb, &eb, &nb);
            c = cmp_human_vals(ma, ea, na, mb, eb, nb);
            if (c == 0) c = cmp_key_str(ka, al, kb, bl);
        } else if (opt_n) {
            long na = parse_num_n(ka, al);
            long nb = parse_num_n(kb, bl);
            if (na < nb) c = -1;
            else if (na > nb) c = 1;
            else c = cmp_key_str(ka, al, kb, bl);
        } else {
            c = cmp_key_str(ka, al, kb, bl);
        }
        if (c == 0) {
            const char *ta = opt_b ? skip_blanks(a) : a;
            const char *tb = opt_b ? skip_blanks(b) : b;
            c = cmp_str(ta, tb);
        }
    } else if (opt_V) {
        ka = sort_key(a, 0);
        kb = sort_key(b, 0);
        c = cmp_version(ka, kb);
    } else if (opt_h) {
        long ma, mb;
        int ea, eb, na, nb;
        ka = sort_key(a, 0);
        kb = sort_key(b, 0);
        parse_human(ka, &ma, &ea, &na);
        parse_human(kb, &mb, &eb, &nb);
        c = cmp_human_vals(ma, ea, na, mb, eb, nb);
        if (c == 0) c = cmp_str(ka, kb);
    } else if (opt_n) {
        ka = sort_key(a, 0);
        kb = sort_key(b, 0);
        {
            long na = parse_num(ka);
            long nb = parse_num(kb);
            if (na < nb) c = -1;
            else if (na > nb) c = 1;
            else c = cmp_str(ka, kb);
        }
    } else {
        ka = sort_key(a, 0);
        kb = sort_key(b, 0);
        c = cmp_str(ka, kb);
    }
    if (opt_r) return -c;
    return c;
}

/* Equality for -u: same key rules as cmp_lines, without -r or line tiebreak. */
static int key_cmp(const char *a, const char *b) {
    int al, bl;
    const char *ka;
    const char *kb;
    if (opt_k > 0) {
        ka = sort_key(a, &al);
        kb = sort_key(b, &bl);
        if (opt_V) {
            return cmp_version_n(ka, al, kb, bl);
        }
        if (opt_h) {
            long ma, mb;
            int ea, eb, na, nb;
            parse_human_n(ka, al, &ma, &ea, &na);
            parse_human_n(kb, bl, &mb, &eb, &nb);
            return cmp_human_vals(ma, ea, na, mb, eb, nb);
        }
        if (opt_n) {
            long na = parse_num_n(ka, al);
            long nb = parse_num_n(kb, bl);
            if (na < nb) return -1;
            if (na > nb) return 1;
            return 0;
        }
        return cmp_key_str(ka, al, kb, bl);
    }
    ka = sort_key(a, 0);
    kb = sort_key(b, 0);
    if (opt_V) {
        return cmp_version(ka, kb);
    }
    if (opt_h) {
        long ma, mb;
        int ea, eb, na, nb;
        parse_human(ka, &ma, &ea, &na);
        parse_human(kb, &mb, &eb, &nb);
        return cmp_human_vals(ma, ea, na, mb, eb, nb);
    }
    if (opt_n) {
        long na = parse_num(ka);
        long nb = parse_num(kb);
        if (na < nb) return -1;
        if (na > nb) return 1;
        return 0;
    }
    return cmp_str(ka, kb);
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

static void shuffle_lines(void) {
    int i;
    rng_seed();
    for (i = nlines - 1; i > 0; i--) {
        unsigned int j = rng_below((unsigned int)(i + 1));
        char *tmp = lines[i];
        lines[i] = lines[j];
        lines[j] = tmp;
    }
}

static void sort_lines(void) {
    /* Insertion sort — fine for thin daily-driver smoke sizes. */
    int i;
    for (i = 1; i < nlines; i++) {
        char *key = lines[i];
        int j = i - 1;
        while (j >= 0 && cmp_lines(lines[j], key) > 0) {
            lines[j + 1] = lines[j];
            j--;
        }
        lines[j + 1] = key;
    }
}

static int add_line(const char *s) {
    unsigned long n;
    char *p;
    if (nlines >= MAX_LINES) {
        u_write(2, "sort: too many lines\n", 21);
        return 1;
    }
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (p == 0) {
        u_write(2, "sort: out of memory\n", 20);
        return 1;
    }
    strcpy(p, s);
    lines[nlines++] = p;
    return 0;
}

static int read_fd(long fd) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            int delim = opt_z ? (c == 0) : (c == '\n');
            if (delim) {
                line[li] = 0;
                if (add_line(line) != 0) return 1;
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        if (add_line(line) != 0) return 1;
    }
    return n < 0 ? 1 : 0;
}

static void emit_all(void) {
    int i;
    for (i = 0; i < nlines; i++) {
        if (opt_u && i > 0 && key_cmp(lines[i - 1], lines[i]) == 0) continue;
        u_write(1, lines[i], strlen(lines[i]));
        if (opt_z) {
            char z = 0;
            u_write(1, &z, 1);
        } else {
            u_write(1, "\n", 1);
        }
    }
}

static void free_all(void) {
    int i;
    for (i = 0; i < nlines; i++) free(lines[i]);
    nlines = 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    opt_r = 0;
    opt_n = 0;
    opt_u = 0;
    opt_f = 0;
    opt_b = 0;
    opt_h = 0;
    opt_V = 0;
    opt_R = 0;
    opt_z = 0;
    opt_k = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        int j;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-k") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            opt_k = (int)strtol_dec(argv[i + 1]);
            if (opt_k < 1) opt_k = 1;
            i += 2;
            continue;
        }
        if (argv[i][1] == 'k' && argv[i][2] != 0 && is_digits(argv[i] + 2)) {
            opt_k = (int)strtol_dec(argv[i] + 2);
            if (opt_k < 1) opt_k = 1;
            i++;
            continue;
        }
        for (j = 1; argv[i][j]; j++) {
            char o = argv[i][j];
            if (o == 'r') opt_r = 1;
            else if (o == 'n') opt_n = 1;
            else if (o == 'u') opt_u = 1;
            else if (o == 'f') opt_f = 1;
            else if (o == 'b') opt_b = 1;
            else if (o == 'h') opt_h = 1;
            else if (o == 'V') opt_V = 1;
            else if (o == 'R') opt_R = 1;
            else if (o == 'z') opt_z = 1;
            else {
                usage();
                return 1;
            }
        }
        i++;
    }

    if (i >= argc) {
        if (read_fd(0) != 0) {
            free_all();
            return 1;
        }
    } else {
        for (; i < argc; i++) {
            long fd;
            if (strcmp(argv[i], "-") == 0) {
                if (read_fd(0) != 0) {
                    free_all();
                    return 1;
                }
                continue;
            }
            fd = u_open(argv[i], 0, 0);
            if (fd < 0) {
                printf("sort: %s: no such file\n", argv[i]);
                rc = 1;
                continue;
            }
            if (read_fd(fd) != 0) {
                u_close(fd);
                free_all();
                return 1;
            }
            u_close(fd);
        }
    }
    if (rc != 0 && nlines == 0) {
        free_all();
        return rc;
    }
    if (opt_R) shuffle_lines();
    else sort_lines();
    emit_all();
    free_all();
    return rc;
}
