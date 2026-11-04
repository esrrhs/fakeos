/* fakeos userland - uniq: collapse consecutive duplicate lines (thin POSIX).
 * Forms: uniq [-c] [-d] [-u] [-i] [FILE...]
 * -c count prefix; -d only duplicated; -u only unique; -i fold case when
 * comparing. stdin if no FILE. Compact argv for EXEC_MAX_ARGC=8.
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

enum { LINE_MAX = 512 };

static int opt_c;
static int opt_d;
static int opt_u;
static int opt_i;

static void usage(void) {
    u_write(2, "usage: uniq [-cdui] [FILE...]\n", 31);
}

static char fold_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* Case-sensitive or -i fold compare. */
static int line_eq(const char *a, const char *b) {
    int i;
    if (!opt_i) return strcmp(a, b) == 0;
    for (i = 0; ; i++) {
        char ca = fold_ch(a[i]);
        char cb = fold_ch(b[i]);
        if (ca != cb) return 0;
        if (a[i] == 0) return 1;
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
            if (o == 'c') opt_c = 1;
            else if (o == 'd') opt_d = 1;
            else if (o == 'u') opt_u = 1;
            else if (o == 'i') opt_i = 1;
            else {
                usage();
                return 1;
            }
        }
        i++;
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
