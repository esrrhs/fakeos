/* fakeos userland - pr: paginate with headers (thin POSIX subset).
 * Forms: pr [-n] [-t] [-w WIDTH|-wWIDTH] [-l LENGTH|-lLENGTH]
 *           [-h HEADER|-hHEADER] [FILE...]
 * Default WIDTH 72, LENGTH 66. Without -t, a 5-line header (blanks + title +
 * "Page N") precedes each page of (LENGTH-5) body lines (min 1), then a form
 * feed. -t omits headers and form feeds. -n prefixes body lines with a
 * 5-digit number and a tab. Glued -wN/-lN/-hTEXT keep argv under
 * EXEC_MAX_ARGC=8. stdin if no FILE.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern long strtol_dec(const char *s);

enum { LINE_MAX = 512, WIDTH_DEFAULT = 72, LENGTH_DEFAULT = 66 };

static int number_lines;
static int omit_header; /* -t */
static int width = WIDTH_DEFAULT;
static int length = LENGTH_DEFAULT;
static const char *header = 0; /* -h; else file name / empty */
static long page_no = 1;

static void usage(void) {
    u_write(2, "usage: pr [-n] [-t] [-w N] [-l N] [-h HDR] [FILE...]\n", 52);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static void emit_num5(long num) {
    char tmp[8];
    int i = 0;
    int j;
    long v = num;
    if (v < 0) v = 0;
    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v > 0 && i < 7) {
            tmp[i++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    for (j = 0; j < 5 - i; j++) u_write(1, " ", 1);
    while (i > 0) {
        i--;
        u_write(1, &tmp[i], 1);
    }
    u_write(1, "\t", 1);
}

static void emit_page_hdr(const char *title) {
    char pbuf[16];
    int pi = 0;
    int j;
    long v = page_no;
    int title_len;
    int right_len;
    int pad;
    const char *t = title ? title : "";

    u_write(1, "\n\n", 2);
    /* "Page N" on the right within WIDTH */
    if (v == 0) {
        pbuf[pi++] = '0';
    } else {
        while (v > 0 && pi < 15) {
            pbuf[pi++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    /* reverse digits into place after "Page " */
    right_len = 5 + pi; /* "Page " + digits */
    title_len = (int)strlen(t);
    if (title_len > width - right_len - 1) title_len = width - right_len - 1;
    if (title_len < 0) title_len = 0;
    if (title_len > 0) u_write(1, t, (unsigned long)title_len);
    pad = width - title_len - right_len;
    if (pad < 1) pad = 1;
    for (j = 0; j < pad; j++) u_write(1, " ", 1);
    u_write(1, "Page ", 5);
    while (pi > 0) {
        pi--;
        u_write(1, &pbuf[pi], 1);
    }
    u_write(1, "\n\n", 2);
}

static int pr_fd(long fd, const char *title) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int i;
    int li = 0;
    int body = length - 5;
    int lines_on_page = 0;
    long line_no = 1;
    int started = 0;
    const char *hdr = header ? header : (title ? title : "");

    if (body < 1) body = 1;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (!omit_header) {
                    if (!started || lines_on_page >= body) {
                        if (started) {
                            u_write(1, "\f", 1);
                            page_no++;
                        }
                        emit_page_hdr(hdr);
                        lines_on_page = 0;
                        started = 1;
                    }
                } else if (!started) {
                    started = 1;
                }
                if (number_lines) emit_num5(line_no);
                if (li > 0) u_write(1, line, (unsigned long)li);
                u_write(1, "\n", 1);
                line_no++;
                lines_on_page++;
                li = 0;
            } else {
                if (li < LINE_MAX - 1) line[li++] = c;
            }
        }
    }
    if (li > 0) {
        if (!omit_header) {
            if (!started || lines_on_page >= body) {
                if (started) {
                    u_write(1, "\f", 1);
                    page_no++;
                }
                emit_page_hdr(hdr);
                started = 1;
            }
        }
        if (number_lines) emit_num5(line_no);
        u_write(1, line, (unsigned long)li);
        u_write(1, "\n", 1);
    }
    if (started && !omit_header) {
        /* traditional trailing form feed after last page */
        u_write(1, "\f", 1);
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-") == 0) break;
        if (strcmp(argv[i], "-n") == 0) {
            number_lines = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-t") == 0) {
            omit_header = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-w") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            width = (int)strtol_dec(argv[i + 1]);
            if (width < 20) width = 20;
            i += 2;
            continue;
        }
        if (argv[i][1] == 'w' && is_digits(argv[i] + 2)) {
            width = (int)strtol_dec(argv[i] + 2);
            if (width < 20) width = 20;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-l") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            length = (int)strtol_dec(argv[i + 1]);
            if (length < 6) length = 6;
            i += 2;
            continue;
        }
        if (argv[i][1] == 'l' && is_digits(argv[i] + 2)) {
            length = (int)strtol_dec(argv[i] + 2);
            if (length < 6) length = 6;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-h") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            header = argv[i + 1];
            i += 2;
            continue;
        }
        if (argv[i][1] == 'h' && argv[i][2] != 0) {
            header = argv[i] + 2;
            i++;
            continue;
        }
        usage();
        return 1;
    }

    if (i >= argc) {
        return pr_fd(0, "");
    }
    for (; i < argc; i++) {
        long fd;
        page_no = 1;
        if (strcmp(argv[i], "-") == 0) {
            if (pr_fd(0, "") != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "pr: open failed\n", 16);
            rc = 1;
            continue;
        }
        if (pr_fd(fd, argv[i]) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
