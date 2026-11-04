/* fakeos userland - sort: lexicographic line sort (thin POSIX subset).
 * Forms: sort [-r] [-n] [-u] [-f] [-k N|-kN] [FILE...]
 * -r reverse; -n numeric (leading optional sign/digits); -u unique by key
 * (suppress consecutive equal keys after sort); -f fold case (A-Z→a-z) for
 * string compares / tie-breaks; -k N sort by blank-separated field N
 * (1-based; missing field sorts as empty / 0).
 * Loads into RAM. Compact argv for EXEC_MAX_ARGC=8.
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

enum {
    LINE_MAX = 512,
    MAX_LINES = 4096
};

static int opt_r;
static int opt_n;
static int opt_u;
static int opt_f;
static int opt_k; /* 0 = whole line; else 1-based field */
static char *lines[MAX_LINES];
static int nlines;

static void usage(void) {
    u_write(2, "usage: sort [-rnuf] [-k N] [FILE...]\n", 37);
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

static char fold_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
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
    if (opt_k > 0) {
        int al, bl;
        const char *fa = field_at(a, opt_k, &al);
        const char *fb = field_at(b, opt_k, &bl);
        if (opt_n) {
            long na = parse_num_n(fa, al);
            long nb = parse_num_n(fb, bl);
            if (na < nb) c = -1;
            else if (na > nb) c = 1;
            else c = cmp_key_str(fa, al, fb, bl);
        } else {
            c = cmp_key_str(fa, al, fb, bl);
        }
        if (c == 0) c = cmp_str(a, b);
    } else if (opt_n) {
        long na = parse_num(a);
        long nb = parse_num(b);
        if (na < nb) c = -1;
        else if (na > nb) c = 1;
        else c = cmp_str(a, b);
    } else {
        c = cmp_str(a, b);
    }
    if (opt_r) return -c;
    return c;
}

/* Equality for -u: same key rules as cmp_lines, without -r or line tiebreak. */
static int key_cmp(const char *a, const char *b) {
    if (opt_k > 0) {
        int al, bl;
        const char *fa = field_at(a, opt_k, &al);
        const char *fb = field_at(b, opt_k, &bl);
        if (opt_n) {
            long na = parse_num_n(fa, al);
            long nb = parse_num_n(fb, bl);
            if (na < nb) return -1;
            if (na > nb) return 1;
            return 0;
        }
        return cmp_key_str(fa, al, fb, bl);
    }
    if (opt_n) {
        long na = parse_num(a);
        long nb = parse_num(b);
        if (na < nb) return -1;
        if (na > nb) return 1;
        return 0;
    }
    return cmp_str(a, b);
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
            if (c == '\n') {
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
        u_write(1, "\n", 1);
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
    sort_lines();
    emit_all();
    free_all();
    return rc;
}
