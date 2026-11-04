/* fakeos userland - sort: lexicographic line sort (thin POSIX subset).
 * Forms: sort [-r] [-n] [FILE...]
 * -r reverse; -n numeric (leading optional sign/digits). Loads into RAM.
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

enum {
    LINE_MAX = 512,
    MAX_LINES = 4096
};

static int opt_r;
static int opt_n;
static char *lines[MAX_LINES];
static int nlines;

static void usage(void) {
    u_write(2, "usage: sort [-r] [-n] [FILE...]\n", 32);
}

static long parse_num(const char *s) {
    long v = 0;
    int i = 0;
    int neg = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '-') {
        neg = 1;
        i++;
    } else if (s[i] == '+') {
        i++;
    }
    if (s[i] < '0' || s[i] > '9') {
        /* Non-numeric sorts as 0 like a thin subset. */
        return 0;
    }
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    return neg ? -v : v;
}

static int cmp_lines(const char *a, const char *b) {
    int c;
    if (opt_n) {
        long na = parse_num(a);
        long nb = parse_num(b);
        if (na < nb) c = -1;
        else if (na > nb) c = 1;
        else c = strcmp(a, b);
    } else {
        c = strcmp(a, b);
    }
    if (opt_r) return -c;
    return c;
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
        for (j = 1; argv[i][j]; j++) {
            char o = argv[i][j];
            if (o == 'r') opt_r = 1;
            else if (o == 'n') opt_n = 1;
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
