/* fakeos userland - cat: concatenate files, or stdin when none given.
 *   cat [-n|-b] [-s] [-v] [-E] [-T] [-A] [FILE...]
 * -n number all lines; -b number non-blank lines (overrides -n numbering
 * style); -s squeeze consecutive blank lines to one; -v show nonprinting
 * (^ and M- notation); -E mark line ends with '$'; -T show tabs as ^I;
 * -A same as -vET. Glued -nbsvETA ok. Number column is width 6 + tab
 * (GNU-ish). Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum { LINE_MAX = 512 };

static int opt_n;
static int opt_b;
static int opt_s;
static int opt_v;
static int opt_E; /* show $ at EOL */
static int opt_T; /* show TAB as ^I */
static int prev_blank; /* across files for -s */

static void usage(void) {
    u_write(2, "usage: cat [-nbsvETA] [FILE...]\n", 31);
}

/* Emit one byte under -v/-E/-T (GNU-ish). */
static void emit_one(unsigned char c) {
    if (c == '\t') {
        if (opt_T) u_write(1, "^I", 2);
        else {
            char ch = '\t';
            u_write(1, &ch, 1);
        }
        return;
    }
    if (c == '\n') {
        if (opt_E) u_write(1, "$", 1);
        {
            char ch = '\n';
            u_write(1, &ch, 1);
        }
        return;
    }
    if (!opt_v) {
        char ch = (char)c;
        u_write(1, &ch, 1);
        return;
    }
    /* -v: high bit as M-, controls as ^X, DEL as ^?. */
    if (c >= 128) {
        u_write(1, "M-", 2);
        c = (unsigned char)(c - 128);
    }
    if (c < 32) {
        char hat = '^';
        char x = (char)(c + 64);
        u_write(1, &hat, 1);
        u_write(1, &x, 1);
    } else if (c == 127) {
        u_write(1, "^?", 2);
    } else {
        char ch = (char)c;
        u_write(1, &ch, 1);
    }
}

static void emit_bytes(const char *buf, int n) {
    int i;
    if (!opt_v && !opt_E && !opt_T) {
        if (n > 0) u_write(1, buf, (unsigned long)n);
        return;
    }
    for (i = 0; i < n; i++) emit_one((unsigned char)buf[i]);
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
    for (j = 0; j < 6 - i; j++) u_write(1, " ", 1);
    while (i > 0) {
        i--;
        u_write(1, &tmp[i], 1);
    }
    u_write(1, "\t", 1);
}

static int cat_fd_raw(long fd) {
    char buf[1024];
    long n;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        if (opt_v || opt_E || opt_T) {
            int i;
            for (i = 0; i < (int)n; i++) emit_one((unsigned char)buf[i]);
        } else if (u_write(1, buf, (u64)n) < 0) {
            return 1;
        }
    }
    return n < 0 ? 1 : 0;
}

static int cat_fd_lines(long fd, long *num) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                int blank = (li == 0);
                if (opt_s && blank && prev_blank) {
                    li = 0;
                    continue;
                }
                if (opt_n || opt_b) {
                    if (opt_b) {
                        if (!blank) {
                            emit_num(*num);
                            (*num)++;
                        }
                    } else {
                        emit_num(*num);
                        (*num)++;
                    }
                }
                if (li > 0) emit_bytes(line, li);
                emit_bytes("\n", 1);
                prev_blank = blank;
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        /* unterminated final line — never blank for -s purposes */
        if (opt_n || opt_b) {
            emit_num(*num);
            (*num)++;
        }
        emit_bytes(line, li);
        prev_blank = 0;
    }
    return n < 0 ? 1 : 0;
}

static int cat_fd(long fd, long *num) {
    if (opt_n || opt_b || opt_s) return cat_fd_lines(fd, num);
    return cat_fd_raw(fd); /* also handles -v/-E/-T alone */
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    long num = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        int j;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        /* lone "-" is a file operand (stdin) */
        if (argv[i][1] == 0) break;
        for (j = 1; argv[i][j]; j++) {
            char o = argv[i][j];
            if (o == 'n') opt_n = 1;
            else if (o == 'b') {
                opt_b = 1;
                opt_n = 1; /* -b implies numbering */
            } else if (o == 's') {
                opt_s = 1;
            } else if (o == 'v') {
                opt_v = 1;
            } else if (o == 'E') {
                opt_E = 1;
            } else if (o == 'T') {
                opt_T = 1;
            } else if (o == 'A') {
                opt_v = 1;
                opt_E = 1;
                opt_T = 1;
            } else {
                usage();
                return 1;
            }
        }
        i++;
    }

    if (i >= argc) {
        return cat_fd(0, &num);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (cat_fd(0, &num) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            printf("cat: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (cat_fd(fd, &num) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
