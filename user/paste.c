/* fakeos userland - paste: merge lines of files (thin POSIX subset).
 * Forms: paste [-sz] [-d LIST] [FILE...]
 * Default delim tab. -z NUL-terminated records (input+output). FILE "-" is
 * stdin (at most one). Compact -dLIST ok. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);

enum {
    LINE_MAX = 512,
    MAX_FILES = 6
};

static const char *delim = "\t";
static int serial; /* -s */
static int opt_z; /* NUL records */

static void usage(void) {
    u_write(2, "usage: paste [-sz] [-d LIST] [FILE...]\n", 39);
}

static char next_delim(unsigned long *di) {
    unsigned long n = strlen(delim);
    char c;
    if (n == 0) return 0;
    c = delim[*di % n];
    (*di)++;
    return c;
}

/* Read one record into out (no delimiter). Returns 1 ok, 0 EOF, -1 error.
 * Default delimiter newline; -z uses NUL. */
static int read_line(long fd, char *out, int *len_out, int *saw_eol) {
    char c;
    long n;
    int li = 0;
    *saw_eol = 0;
    while (1) {
        n = u_read(fd, &c, 1);
        if (n < 0) return -1;
        if (n == 0) {
            *len_out = li;
            return li > 0 ? 1 : 0;
        }
        if (opt_z ? (c == 0) : (c == '\n')) {
            *saw_eol = 1;
            out[li] = 0;
            *len_out = li;
            return 1;
        }
        if (li < LINE_MAX - 1) out[li++] = c;
    }
}

static int paste_parallel(long *fds, int nfds) {
    char lines[MAX_FILES][LINE_MAX];
    int lens[MAX_FILES];
    int alive[MAX_FILES];
    int i;
    int any = 1;
    unsigned long di = 0;

    for (i = 0; i < nfds; i++) alive[i] = 1;

    while (any) {
        int got = 0;
        any = 0;
        for (i = 0; i < nfds; i++) {
            int eol = 0;
            lens[i] = 0;
            lines[i][0] = 0;
            if (!alive[i]) continue;
            {
                int r = read_line(fds[i], lines[i], &lens[i], &eol);
                if (r < 0) return 1;
                if (r == 0) {
                    alive[i] = 0;
                    continue;
                }
            }
            got = 1;
            any = 1;
        }
        if (!got) break;
        /* Re-check: if some died mid-round, still emit empties for them. */
        for (i = 0; i < nfds; i++) {
            if (i > 0) {
                char d = next_delim(&di);
                if (d != 0) u_write(1, &d, 1);
            }
            if (lens[i] > 0) u_write(1, lines[i], (unsigned long)lens[i]);
        }
        if (opt_z) {
            char z = 0;
            u_write(1, &z, 1);
        } else {
            u_write(1, "\n", 1);
        }
        any = 0;
        for (i = 0; i < nfds; i++) if (alive[i]) any = 1;
    }
    return 0;
}

static int paste_serial(long *fds, int nfds) {
    int i;
    unsigned long di = 0;

    for (i = 0; i < nfds; i++) {
        char line[LINE_MAX];
        int len = 0;
        int eol = 0;
        int first = 1;
        int r;
        while ((r = read_line(fds[i], line, &len, &eol)) > 0) {
            if (!first) {
                char d = next_delim(&di);
                if (d != 0) u_write(1, &d, 1);
            }
            first = 0;
            if (len > 0) u_write(1, line, (unsigned long)len);
        }
        if (r < 0) return 1;
        if (opt_z) {
            char z = 0;
            u_write(1, &z, 1);
        } else {
            u_write(1, "\n", 1);
        }
        di = 0; /* POSIX: delim list restarts per file in -s */
    }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    long fds[MAX_FILES];
    int nfds = 0;
    int rc = 0;
    int stdin_used = 0;

    serial = 0;
    opt_z = 0;
    delim = "\t";

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-s") == 0) {
            serial = 1;
            i++;
        } else if (strcmp(a, "-z") == 0) {
            opt_z = 1;
            i++;
        } else if (a[1] != 0 && a[2] != 0 && a[1] != 'd') {
            /* glued short opts: -sz / -zs (and lone letters) */
            int j;
            for (j = 1; a[j]; j++) {
                if (a[j] == 's') serial = 1;
                else if (a[j] == 'z') opt_z = 1;
                else { usage(); return 1; }
            }
            i++;
        } else if (strcmp(a, "-d") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            delim = argv[i + 1];
            i += 2;
        } else if (a[1] == 'd' && a[2] != 0) {
            delim = a + 2;
            i++;
        } else if (strcmp(a, "-") == 0) {
            break; /* file operand */
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        fds[0] = 0;
        nfds = 1;
        stdin_used = 1;
    } else {
        for (; i < argc; i++) {
            long fd;
            if (nfds >= MAX_FILES) {
                u_write(2, "paste: too many files\n", 22);
                return 1;
            }
            if (strcmp(argv[i], "-") == 0) {
                if (stdin_used) {
                    u_write(2, "paste: only one stdin\n", 22);
                    return 1;
                }
                fds[nfds++] = 0;
                stdin_used = 1;
                continue;
            }
            fd = u_open(argv[i], 0, 0);
            if (fd < 0) {
                u_write(2, "paste: open failed\n", 19);
                return 1;
            }
            fds[nfds++] = fd;
        }
    }

    if (serial) rc = paste_serial(fds, nfds);
    else rc = paste_parallel(fds, nfds);

    for (i = 0; i < nfds; i++) {
        if (fds[i] > 2) u_close(fds[i]);
    }
    return rc;
}
