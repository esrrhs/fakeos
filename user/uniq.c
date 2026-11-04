/* fakeos userland - uniq: collapse consecutive duplicate lines (thin POSIX).
 * Forms: uniq [-cdui] [-f N] [-s N] [FILE...]
 * -c count prefix; -d only duplicated; -u only unique; -i fold case when
 * comparing; -f N skip N fields; -s N skip N chars (after fields) when
 * comparing. Output still emits the full original line. stdin if no FILE.
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
extern void printf(const char *fmt, ...);
extern long strtol_dec(const char *s);

enum { LINE_MAX = 512 };

static int opt_c;
static int opt_d;
static int opt_u;
static int opt_i;
static int opt_f; /* skip N fields (0 = none) */
static int opt_s; /* skip N chars after fields (0 = none) */

static void usage(void) {
    u_write(2, "usage: uniq [-cdui] [-f N] [-s N] [FILE...]\n", 47);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

/* Comparison key: skip opt_f fields then opt_s chars. */
static const char *key_at(const char *line) {
    int f = opt_f;
    int s = opt_s;
    const char *p = line;
    while (f > 0) {
        while (*p && is_blank(*p)) p++;
        if (*p == 0) break;
        while (*p && !is_blank(*p)) p++;
        f--;
    }
    while (s > 0 && *p) {
        p++;
        s--;
    }
    return p;
}

static char fold_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* Case-sensitive or -i fold compare on keys (after -f/-s skips). */
static int line_eq(const char *a, const char *b) {
    const char *ka = key_at(a);
    const char *kb = key_at(b);
    int i;
    if (!opt_i) return strcmp(ka, kb) == 0;
    for (i = 0; ; i++) {
        char ca = fold_ch(ka[i]);
        char cb = fold_ch(kb[i]);
        if (ca != cb) return 0;
        if (ka[i] == 0) return 1;
    }
}

static void emit_count(int count) {
    char tmp[16];
    int i = 0;
    int j;
    int v = count;
    if (v < 0) v = 0;
    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v > 0) {
            tmp[i++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    /* right-pad to width 7 like classic uniq -c */
    for (j = 0; j < 7 - i; j++) u_write(1, " ", 1);
    while (i > 0) {
        i--;
        u_write(1, &tmp[i], 1);
    }
    u_write(1, " ", 1);
}

static void emit(const char *line, int count) {
    if (count <= 0) return;
    if (opt_d && count < 2) return;
    if (opt_u && count != 1) return;
    if (opt_c) emit_count(count);
    u_write(1, line, strlen(line));
    u_write(1, "\n", 1);
}

static int uniq_fd(long fd) {
    char buf[512];
    char prev[LINE_MAX];
    char cur[LINE_MAX];
    long n;
    int li = 0;
    int have_prev = 0;
    int count = 0;
    int i;

    prev[0] = 0;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                cur[li] = 0;
                if (!have_prev) {
                    strcpy(prev, cur);
                    have_prev = 1;
                    count = 1;
                } else if (line_eq(prev, cur)) {
                    count++;
                } else {
                    emit(prev, count);
                    strcpy(prev, cur);
                    count = 1;
                }
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) cur[li++] = c;
        }
    }
    if (li > 0) {
        cur[li] = 0;
        if (!have_prev) {
            strcpy(prev, cur);
            have_prev = 1;
            count = 1;
        } else if (line_eq(prev, cur)) {
            count++;
        } else {
            emit(prev, count);
            strcpy(prev, cur);
            count = 1;
        }
    }
    if (have_prev) emit(prev, count);
    return n < 0 ? 1 : 0;
}

/* Parse -f/-s value: attached digits after flag letter, else next argv. */
static int take_num(int argc, char **argv, int *i, int j, int *out) {
    const char *a = argv[*i];
    if (a[j + 1] != 0) {
        if (!is_digits(a + j + 1)) return 0;
        *out = (int)strtol_dec(a + j + 1);
        (*i)++;
        return 1;
    }
    if (*i + 1 >= argc || !is_digits(argv[*i + 1])) return 0;
    *out = (int)strtol_dec(argv[*i + 1]);
    *i += 2;
    return 1;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    opt_c = 0;
    opt_d = 0;
    opt_u = 0;
    opt_i = 0;
    opt_f = 0;
    opt_s = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        int j;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        /* Dedicated -f / -s forms (and -fN / -sN) first. */
        if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            opt_f = (int)strtol_dec(argv[i + 1]);
            i += 2;
            continue;
        }
        if (argv[i][1] == 'f' && argv[i][2] != 0 && is_digits(argv[i] + 2)) {
            opt_f = (int)strtol_dec(argv[i] + 2);
            i++;
            continue;
        }
        if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            opt_s = (int)strtol_dec(argv[i + 1]);
            i += 2;
            continue;
        }
        if (argv[i][1] == 's' && argv[i][2] != 0 && is_digits(argv[i] + 2)) {
            opt_s = (int)strtol_dec(argv[i] + 2);
            i++;
            continue;
        }
        for (j = 1; argv[i][j]; j++) {
            char o = argv[i][j];
            if (o == 'c') opt_c = 1;
            else if (o == 'd') opt_d = 1;
            else if (o == 'u') opt_u = 1;
            else if (o == 'i') opt_i = 1;
            else if (o == 'f') {
                if (!take_num(argc, argv, &i, j, &opt_f)) {
                    usage();
                    return 1;
                }
                goto next_arg;
            } else if (o == 's') {
                if (!take_num(argc, argv, &i, j, &opt_s)) {
                    usage();
                    return 1;
                }
                goto next_arg;
            } else {
                usage();
                return 1;
            }
        }
        i++;
    next_arg:
        ;
    }
    if (opt_d && opt_u) {
        usage();
        return 1;
    }

    if (i >= argc) {
        return uniq_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (uniq_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            printf("uniq: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (uniq_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
