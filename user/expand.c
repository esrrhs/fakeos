/* fakeos userland - expand: tabs to spaces (thin POSIX subset).
 * Forms: expand [-i] [-t N|-tN] [FILE...]
 * Default tab stops every 8 columns. -i converts only initial tabs (stops
 * after the first non-blank on a line). stdin if no FILE.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);

enum { TAB_DEFAULT = 8, OUT_MAX = 512 };

static int tabstop = TAB_DEFAULT;
static int opt_i; /* -i: initial tabs only */

static void usage(void) {
    u_write(2, "usage: expand [-i] [-t N] [FILE...]\n", 36);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static void emit_spaces(int n) {
    while (n-- > 0) u_write(1, " ", 1);
}

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

static int expand_fd(long fd) {
    char buf[512];
    long n;
    int i;
    int col = 0;
    int in_lead = 1; /* still in leading blanks (for -i) */

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                u_write(1, "\n", 1);
                col = 0;
                in_lead = 1;
                continue;
            }
            if (c == '\r') {
                u_write(1, "\r", 1);
                col = 0;
                in_lead = 1;
                continue;
            }
            if (c == '\b') {
                u_write(1, "\b", 1);
                if (col > 0) col--;
                continue;
            }
            if (c == '\t') {
                int spaces;
                if (opt_i && !in_lead) {
                    /* leave tab literal after non-blank */
                    u_write(1, "\t", 1);
                    spaces = tabstop - (col % tabstop);
                    if (spaces <= 0) spaces = tabstop;
                    col += spaces;
                    continue;
                }
                spaces = tabstop - (col % tabstop);
                if (spaces <= 0) spaces = tabstop;
                emit_spaces(spaces);
                col += spaces;
                continue;
            }
            if (!is_blank(c)) in_lead = 0;
            u_write(1, &c, 1);
            col++;
        }
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    tabstop = TAB_DEFAULT;
    opt_i = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-i") == 0) {
            opt_i = 1;
            i++;
            continue;
        }
        if (strcmp(a, "-t") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            tabstop = (int)strtol_dec(argv[i + 1]);
            if (tabstop < 1) tabstop = 1;
            if (tabstop > OUT_MAX) tabstop = OUT_MAX;
            i += 2;
        } else if (a[1] == 't' && a[2] != 0 && is_digits(a + 2)) {
            /* -tN glued (keeps argv under EXEC_MAX_ARGC) */
            tabstop = (int)strtol_dec(a + 2);
            if (tabstop < 1) tabstop = 1;
            if (tabstop > OUT_MAX) tabstop = OUT_MAX;
            i++;
        } else if (a[1] == 'i' && a[2] != 0) {
            /* glued -itN or -i then other */
            int j = 1;
            while (a[j]) {
                if (a[j] == 'i') {
                    opt_i = 1;
                    j++;
                } else if (a[j] == 't') {
                    if (a[j + 1] != 0 && is_digits(a + j + 1)) {
                        tabstop = (int)strtol_dec(a + j + 1);
                        if (tabstop < 1) tabstop = 1;
                        if (tabstop > OUT_MAX) tabstop = OUT_MAX;
                        break;
                    }
                    usage();
                    return 1;
                } else {
                    usage();
                    return 1;
                }
            }
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        return expand_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (expand_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "expand: open failed\n", 20);
            rc = 1;
            continue;
        }
        if (expand_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
