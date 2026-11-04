/* fakeos userland - unexpand: spaces to tabs (thin POSIX subset).
 * Forms: unexpand [-a] [-t N|-tN] [FILE...]
 * Default: only leading blanks; -a converts all blank runs that hit a stop.
 * -t implies -a (POSIX). Default tab stops every 8 columns. stdin if no FILE.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);

enum { TAB_DEFAULT = 8, LINE_MAX = 512 };

static int tabstop = TAB_DEFAULT;
static int all_blanks; /* -a or -t */

static void usage(void) {
    u_write(2, "usage: unexpand [-a] [-t N] [FILE...]\n", 38);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static void emit_ch(char c) {
    u_write(1, &c, 1);
}

static void emit_spaces(int n) {
    while (n-- > 0) emit_ch(' ');
}

static void unexpand_line(const char *line, int len) {
    int i = 0;
    int col = 0;
    int pend = 0; /* pending spaces not yet emitted */
    int in_lead = 1;

    while (i < len) {
        char c = line[i++];
        if (c == '\t') {
            int advance = tabstop - (col % tabstop);
            if (advance <= 0) advance = tabstop;
            if (in_lead || all_blanks) {
                pend = 0;
                emit_ch('\t');
                col += advance;
            } else {
                if (pend > 0) {
                    emit_spaces(pend);
                    col += pend;
                    pend = 0;
                }
                emit_ch('\t');
                col += advance;
            }
            continue;
        }
        if (c == ' ') {
            if (in_lead || all_blanks) {
                pend++;
                if ((col + pend) % tabstop == 0) {
                    emit_ch('\t');
                    col += pend;
                    pend = 0;
                }
            } else {
                emit_ch(' ');
                col++;
            }
            continue;
        }
        if (pend > 0) {
            emit_spaces(pend);
            col += pend;
            pend = 0;
        }
        in_lead = 0;
        emit_ch(c);
        if (c == '\b') {
            if (col > 0) col--;
        } else {
            col++;
        }
    }
    if (pend > 0) emit_spaces(pend);
    emit_ch('\n');
}

static int unexpand_fd(long fd) {
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
                unexpand_line(line, li);
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        unexpand_line(line, li);
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-a") == 0) {
            all_blanks = 1;
            i++;
        } else if (strcmp(a, "-t") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            tabstop = (int)strtol_dec(argv[i + 1]);
            if (tabstop < 1) tabstop = 1;
            if (tabstop > LINE_MAX) tabstop = LINE_MAX;
            all_blanks = 1; /* POSIX: -t implies convert all */
            i += 2;
        } else if (a[1] == 't' && a[2] != 0 && is_digits(a + 2)) {
            tabstop = (int)strtol_dec(a + 2);
            if (tabstop < 1) tabstop = 1;
            if (tabstop > LINE_MAX) tabstop = LINE_MAX;
            all_blanks = 1;
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        return unexpand_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (unexpand_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "unexpand: open failed\n", 22);
            rc = 1;
            continue;
        }
        if (unexpand_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
