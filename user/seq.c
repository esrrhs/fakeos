/* fakeos userland - seq: print integer sequences (thin POSIX subset).
 * Forms: seq LAST | seq FIRST LAST | seq FIRST INCREMENT LAST
 * Options: -s SEPARATOR (default newline); -w equal-width zero pad
 * (width = max decimal width of endpoints, including '-'); -f FORMAT
 * printf-ish (%% and %d/%i/%u/%g/%G/%f/%e/%E → decimal integer; thin, no
 * float). With -f, -w is ignored. Always ends with a newline. Compact argv
 * for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);

static int opt_w;

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
    u_write(2, "usage: seq [-w] [-s SEP] [-f FMT] LAST | FIRST LAST | FIRST INCREMENT LAST\n", 75);
}

static void write_sep(const char *sep) {
    unsigned long n = strlen(sep);
    if (n > 0) u_write(1, sep, n);
}

/* Decimal character width of v, including leading '-' for negatives. */
static int num_width(long v) {
    int w = 0;
    unsigned long u;
    if (v < 0) {
        w = 1;
        u = (unsigned long)(-(v + 1)) + 1UL;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) return w + 1;
    while (u > 0) {
        w++;
        u /= 10UL;
    }
    return w;
}

static void emit_num(long v, int width) {
    char tmp[24];
    int i = 0;
    int k;
    int neg = 0;
    unsigned long u;
    int digits;
    int pad;

    if (v < 0) {
        neg = 1;
        u = (unsigned long)(-(v + 1)) + 1UL;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) {
        tmp[i++] = '0';
    } else {
        while (u > 0 && i < 24) {
            tmp[i++] = (char)('0' + (u % 10UL));
            u /= 10UL;
        }
    }
    digits = i;
    if (opt_w && width > 0) {
        pad = width - digits - neg;
        if (pad < 0) pad = 0;
        if (neg) u_write(1, "-", 1);
        while (pad > 0) {
            u_write(1, "0", 1);
            pad--;
        }
    } else if (neg) {
        u_write(1, "-", 1);
    }
    for (k = digits - 1; k >= 0; k--) {
        char c = tmp[k];
        u_write(1, &c, 1);
    }
}

/* Thin FORMAT: literals, %%, and %[diugGfFeE] as decimal integer. */
static void emit_fmt(long v, const char *fmt) {
    int i = 0;
    while (fmt[i]) {
        if (fmt[i] != '%') {
            u_write(1, &fmt[i], 1);
            i++;
            continue;
        }
        i++;
        if (fmt[i] == '%') {
            u_write(1, "%", 1);
            i++;
            continue;
        }
        /* Skip thin flags/width/precision: 0-9 and '.' digits. */
        while (fmt[i] == '0' || (fmt[i] >= '1' && fmt[i] <= '9') ||
               fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ') {
            i++;
        }
        if (fmt[i] == '.') {
            i++;
            while (fmt[i] >= '0' && fmt[i] <= '9') i++;
        }
        if (fmt[i] == 'd' || fmt[i] == 'i' || fmt[i] == 'u' ||
            fmt[i] == 'g' || fmt[i] == 'G' || fmt[i] == 'f' ||
            fmt[i] == 'e' || fmt[i] == 'E') {
            int save = opt_w;
            opt_w = 0;
            emit_num(v, 0);
            opt_w = save;
            i++;
        } else if (fmt[i] == 0) {
            u_write(1, "%", 1);
            break;
        } else {
            u_write(1, "%", 1);
            u_write(1, &fmt[i], 1);
            i++;
        }
    }
}

int main(int argc, char **argv) {
    const char *sep = "\n";
    const char *fmt = 0;
    int i = 1;
    long first = 1;
    long step = 1;
    long last = 0;
    long nnums;
    long cur;
    int first_out = 1;
    int width = 0;

    opt_w = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        /* Negative numbers are operands, not options. */
        if (argv[i][1] >= '0' && argv[i][1] <= '9') break;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            sep = argv[i + 1];
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            fmt = argv[i + 1];
            i += 2;
            continue;
        }
        if (argv[i][1] == 'f' && argv[i][2] != 0) {
            fmt = argv[i] + 2;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-w") == 0) {
            opt_w = 1;
            i++;
            continue;
        }
        /* Glued shorts: -w, -wsSEP, -sw with following SEP, -fFMT, etc. */
        {
            const char *o = argv[i] + 1;
            int j = 0;
            while (o[j] != 0) {
                if (o[j] == 'w') {
                    opt_w = 1;
                    j++;
                } else if (o[j] == 's') {
                    if (o[j + 1] != 0) {
                        sep = o + j + 1;
                        i++;
                        goto opts_done;
                    }
                    if (i + 1 >= argc) {
                        usage();
                        return 1;
                    }
                    sep = argv[i + 1];
                    i += 2;
                    goto opts_done;
                } else if (o[j] == 'f') {
                    if (o[j + 1] != 0) {
                        fmt = o + j + 1;
                        i++;
                        goto opts_done;
                    }
                    if (i + 1 >= argc) {
                        usage();
                        return 1;
                    }
                    fmt = argv[i + 1];
                    i += 2;
                    goto opts_done;
                } else {
                    usage();
                    return 1;
                }
            }
            i++;
        }
    }
opts_done:

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

    if (opt_w && !fmt) {
        int w1 = num_width(first);
        int w2 = num_width(last);
        width = w1 > w2 ? w1 : w2;
    }

    cur = first;
    for (;;) {
        if (!first_out) write_sep(sep);
        first_out = 0;
        if (fmt) emit_fmt(cur, fmt);
        else emit_num(cur, width);
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
