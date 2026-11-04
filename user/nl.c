/* fakeos userland - nl: number lines (thin POSIX subset).
 * Forms: nl [-b a|t] [-v START] [-i INCR] [-w WIDTH] [-s SEP] [FILE...]
 * Default: -b t (non-empty), -v 1, -i 1, -w 6, -s tab. stdin if no FILE.
 * Glued -ba/-bt/-vN/-iN/-wN/-sSEP keep argv under EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern long strtol_dec(const char *s);

enum { LINE_MAX = 512 };

static int body_all;      /* -b a: number all; else -b t non-empty */
static long start_num = 1;
static long incr = 1;     /* -i: number increment */
static int width = 6;
static const char *sep = "\t";

static void usage(void) {
    u_write(2, "usage: nl [-b a|t] [-v N] [-i N] [-w N] [-s SEP] [FILE...]\n", 59);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static void emit_num(long num) {
    char tmp[16];
    int i = 0;
    int j;
    long v = num;
    if (v < 0) v = 0;
    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v > 0 && i < 15) {
            tmp[i++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    for (j = 0; j < width - i; j++) u_write(1, " ", 1);
    while (i > 0) {
        i--;
        u_write(1, &tmp[i], 1);
    }
    u_write(1, sep, strlen(sep));
}

static void emit_line(const char *line, int len, long *num) {
    int empty = (len == 0);
    if (body_all || !empty) {
        emit_num(*num);
        (*num) += incr;
    } else {
        /* blank body lines: spaces matching number column + sep omitted
         * (POSIX leaves leading whitespace empty for unnumbered). */
        int j;
        for (j = 0; j < width; j++) u_write(1, " ", 1);
        u_write(1, sep, strlen(sep));
    }
    if (len > 0) u_write(1, line, (unsigned long)len);
    u_write(1, "\n", 1);
}

static int nl_fd(long fd, long *num) {
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
                emit_line(line, li, num);
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        emit_line(line, li, num);
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    long num;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-b") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            if (strcmp(argv[i + 1], "a") == 0) body_all = 1;
            else if (strcmp(argv[i + 1], "t") == 0) body_all = 0;
            else { usage(); return 1; }
            i += 2;
        } else if (a[1] == 'b' && a[2] != 0 && a[3] == 0) {
            /* -ba / -bt (compact; keeps argv under EXEC_MAX_ARGC) */
            if (a[2] == 'a') body_all = 1;
            else if (a[2] == 't') body_all = 0;
            else { usage(); return 1; }
            i++;
        } else if (strcmp(a, "-v") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            start_num = strtol_dec(argv[i + 1]);
            i += 2;
        } else if (a[1] == 'v' && a[2] != 0 && is_digits(a + 2)) {
            start_num = strtol_dec(a + 2);
            i++;
        } else if (strcmp(a, "-i") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            incr = strtol_dec(argv[i + 1]);
            if (incr < 1) incr = 1;
            i += 2;
        } else if (a[1] == 'i' && a[2] != 0 && is_digits(a + 2)) {
            incr = strtol_dec(a + 2);
            if (incr < 1) incr = 1;
            i++;
        } else if (strcmp(a, "-w") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            width = (int)strtol_dec(argv[i + 1]);
            if (width < 1) width = 1;
            if (width > 16) width = 16;
            i += 2;
        } else if (a[1] == 'w' && a[2] != 0 && is_digits(a + 2)) {
            width = (int)strtol_dec(a + 2);
            if (width < 1) width = 1;
            if (width > 16) width = 16;
            i++;
        } else if (strcmp(a, "-s") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            sep = argv[i + 1];
            i += 2;
        } else if (a[1] == 's' && a[2] != 0) {
            sep = a + 2;
            i++;
        } else {
            usage();
            return 1;
        }
    }

    num = start_num;
    if (i >= argc) {
        return nl_fd(0, &num);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (nl_fd(0, &num) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "nl: open failed\n", 16);
            rc = 1;
            continue;
        }
        if (nl_fd(fd, &num) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
