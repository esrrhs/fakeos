/* fakeos userland - seq: print integer sequences (thin POSIX subset).
 * Forms: seq LAST | seq FIRST LAST | seq FIRST INCREMENT LAST
 * Option: -s SEPARATOR (default newline). Always ends with a newline.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

static int parse_long(const char *s, long *out) {
    long v = 0;
    int i = 0;
    int neg = 0;
    if (s == 0 || s[0] == 0) return 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (s[i] == 0) return 0;
    } else if (s[0] == '+') {
        i = 1;
        if (s[i] == 0) return 0;
    }
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    *out = neg ? -v : v;
    return 1;
}

static void usage(void) {
    u_write(2, "usage: seq [-s SEP] LAST | FIRST LAST | FIRST INCREMENT LAST\n", 61);
}

static void write_sep(const char *sep) {
    unsigned long n = strlen(sep);
    if (n > 0) u_write(1, sep, n);
}

int main(int argc, char **argv) {
    const char *sep = "\n";
    int i = 1;
    long first = 1;
    long step = 1;
    long last = 0;
    long nnums;
    long cur;
    int first_out = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        /* Negative numbers are operands, not options. */
        if (argv[i][1] >= '0' && argv[i][1] <= '9') break;
        if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            sep = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            usage();
            return 1;
        }
    }

    nnums = argc - i;
    if (nnums == 1) {
        if (!parse_long(argv[i], &last)) {
            usage();
            return 1;
        }
        first = 1;
        step = 1;
    } else if (nnums == 2) {
        if (!parse_long(argv[i], &first) || !parse_long(argv[i + 1], &last)) {
            usage();
            return 1;
        }
        step = 1;
    } else if (nnums == 3) {
        if (!parse_long(argv[i], &first) || !parse_long(argv[i + 1], &step) ||
            !parse_long(argv[i + 2], &last)) {
            usage();
            return 1;
        }
    } else {
        usage();
        return 1;
    }

    if (step == 0) {
        u_write(2, "seq: zero increment\n", 20);
        return 1;
    }
    if ((step > 0 && first > last) || (step < 0 && first < last)) {
        return 0;
    }

    cur = first;
    for (;;) {
        if (!first_out) write_sep(sep);
        first_out = 0;
        printf("%d", (int)cur);
        if (step > 0) {
            if (cur > last - step) break;
            cur = cur + step;
        } else {
            if (cur < last - step) break;
            cur = cur + step;
        }
    }
    u_write(1, "\n", 1);
    return 0;
}
