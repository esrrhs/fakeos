/* fakeos userland - cut: select bytes/fields from lines (thin POSIX subset).
 * Forms: cut -c LIST [FILE...] | cut -f LIST [-d DELIM] [-s] [FILE...]
 * LIST: N | N- | -M | N-M | comma-separated (1-based, inclusive).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);

enum {
    LINE_MAX = 512,
    MAX_SEL  = 64
};

struct sel {
    int lo; /* 1-based inclusive; 0 unused */
    int hi; /* inclusive; -1 means open end (N-) */
};

static struct sel sels[MAX_SEL];
static int nsels;
static int mode_c;      /* 1 = -c, 0 = -f */
static char delim = '\t';
static int suppress;    /* -s: skip lines without delim when -f */

static void usage(void) {
    u_write(2, "usage: cut -c LIST [FILE...] | cut -f LIST [-d DELIM] [-s] [FILE...]\n", 69);
}

static int parse_uint(const char *s, int *endp, int *out) {
    int i = *endp;
    int v = 0;
    int any = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
        any = 1;
        if (v > 1000000) return 0;
    }
    if (!any) return 0;
    *endp = i;
    *out = v;
    return 1;
}

static int parse_list(const char *list) {
    int i = 0;
    nsels = 0;
    if (list == 0 || list[0] == 0) return 0;
    while (list[i]) {
        int lo = 0;
        int hi = 0;
        int have_lo = 0;
        int have_hi = 0;
        if (nsels >= MAX_SEL) return 0;
        if (list[i] == '-') {
            /* -M */
            i++;
            if (!parse_uint(list, &i, &hi)) return 0;
            lo = 1;
            have_lo = 1;
            have_hi = 1;
        } else {
            if (!parse_uint(list, &i, &lo)) return 0;
            have_lo = 1;
            if (list[i] == '-') {
                i++;
                if (list[i] >= '0' && list[i] <= '9') {
                    if (!parse_uint(list, &i, &hi)) return 0;
                    have_hi = 1;
                } else {
                    hi = -1; /* N- */
                    have_hi = 1;
                }
            } else {
                hi = lo;
                have_hi = 1;
            }
        }
        if (!have_lo || !have_hi) return 0;
        if (hi != -1 && hi < lo) return 0;
        if (lo < 1) return 0;
        sels[nsels].lo = lo;
        sels[nsels].hi = hi;
        nsels++;
        if (list[i] == ',') {
            i++;
            continue;
        }
        if (list[i] == 0) break;
        return 0;
    }
    return nsels > 0;
}

static int selected(int pos) {
    int k;
    for (k = 0; k < nsels; k++) {
        int lo = sels[k].lo;
        int hi = sels[k].hi;
        if (hi < 0) {
            if (pos >= lo) return 1;
        } else if (pos >= lo && pos <= hi) {
            return 1;
        }
    }
    return 0;
}

static void emit_chars(const char *line, int len) {
    int i;
    for (i = 0; i < len; i++) {
        if (selected(i + 1)) u_write(1, &line[i], 1);
    }
    u_write(1, "\n", 1);
}

static void emit_fields(const char *line, int len) {
    int i = 0;
    int field = 0;
    int out_any = 0;
    int has_delim = 0;
    int start;

    for (i = 0; i < len; i++) {
        if (line[i] == delim) {
            has_delim = 1;
            break;
        }
    }
    if (suppress && !has_delim) return;
    if (!has_delim) {
        /* POSIX: whole line when no delim and not -s */
        u_write(1, line, (unsigned long)len);
        u_write(1, "\n", 1);
        return;
    }

    i = 0;
    while (i <= len) {
        start = i;
        while (i < len && line[i] != delim) i++;
        field++;
        if (selected(field)) {
            if (out_any) u_write(1, &delim, 1);
            if (i > start) u_write(1, &line[start], (unsigned long)(i - start));
            out_any = 1;
        }
        if (i >= len) break;
        i++; /* skip delim */
    }
    u_write(1, "\n", 1);
}

static int cut_fd(long fd) {
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
                if (mode_c) emit_chars(line, li);
                else emit_fields(line, li);
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        if (mode_c) emit_chars(line, li);
        else emit_fields(line, li);
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    int have_mode = 0;
    const char *list = 0;
    int files = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc || have_mode) {
                usage();
                return 1;
            }
            mode_c = 1;
            have_mode = 1;
            list = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc || have_mode) {
                usage();
                return 1;
            }
            mode_c = 0;
            have_mode = 1;
            list = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == 0) {
                usage();
                return 1;
            }
            delim = argv[i + 1][0];
            i += 2;
        } else if (strcmp(argv[i], "-s") == 0) {
            suppress = 1;
            i++;
        } else if (argv[i][1] == 'c' && argv[i][2] != 0) {
            /* -cLIST */
            if (have_mode) { usage(); return 1; }
            mode_c = 1;
            have_mode = 1;
            list = argv[i] + 2;
            i++;
        } else if (argv[i][1] == 'f' && argv[i][2] != 0) {
            if (have_mode) { usage(); return 1; }
            mode_c = 0;
            have_mode = 1;
            list = argv[i] + 2;
            i++;
        } else if (argv[i][1] == 'd' && argv[i][2] != 0) {
            delim = argv[i][2];
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (!have_mode || !parse_list(list)) {
        usage();
        return 1;
    }
    if (suppress && mode_c) {
        usage();
        return 1;
    }

    if (i >= argc) {
        return cut_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (cut_fd(0) != 0) rc = 1;
            files++;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            printf("cut: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (cut_fd(fd) != 0) rc = 1;
        u_close(fd);
        files++;
    }
    (void)files;
    return rc;
}
