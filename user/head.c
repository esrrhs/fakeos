/* fakeos userland - head: print the first N lines or bytes of files.
 *   head [-n N|-N] [-c N|-cN] [-q|-v|-z] [FILE...]
 * Default: first 10 lines. -c selects byte mode (mutually exclusive with -n
 * in practice: last of -n/-c wins). -z NUL-terminated records (line mode).
 * -q never headers; -v always headers; default headers when more than one
 * FILE. stdin if no FILE; "-" is stdin. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);
extern void printf(const char *fmt, ...);

static int opt_z; /* NUL record delimiter */

static int head_lines(long fd, long limit) {
    char buf[512];
    long n;
    long left = limit;
    int i;

    if (left < 0) left = 0;
    while (left > 0 && (n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            if (u_write(1, &buf[i], 1) < 0) return 1;
            if (opt_z ? (buf[i] == 0) : (buf[i] == '\n')) {
                left--;
                if (left <= 0) return 0;
            }
        }
    }
    return n < 0 ? 1 : 0;
}

static int head_bytes(long fd, long limit) {
    char buf[512];
    long n;
    long left = limit;
    long chunk;

    if (left < 0) left = 0;
    while (left > 0) {
        chunk = left < (long)sizeof(buf) ? left : (long)sizeof(buf);
        n = u_read(fd, buf, (unsigned long)chunk);
        if (n <= 0) return n < 0 ? 1 : 0;
        if (u_write(1, buf, (unsigned long)n) < 0) return 1;
        left -= n;
    }
    return 0;
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static void usage(void) {
    u_write(2, "usage: head [-n N|-N] [-c N|-cN] [-q|-v|-z] [FILE...]\n", 54);
}

int main(int argc, char **argv) {
    long limit = 10;
    int byte_mode = 0;
    int quiet = 0;
    int verbose = 0;
    int i = 1;
    opt_z = 0;
    int rc = 0;
    int files = 0;
    int nfiles;
    int fi;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            limit = strtol_dec(argv[i + 1]);
            byte_mode = 0;
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            limit = strtol_dec(argv[i + 1]);
            byte_mode = 1;
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
            verbose = 0;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
            quiet = 0;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-z") == 0) {
            opt_z = 1;
            i++;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == 'c' && is_digits(argv[i] + 2)) {
            limit = strtol_dec(argv[i] + 2);
            byte_mode = 1;
            i++;
            continue;
        }
        if (argv[i][0] == '-' && is_digits(argv[i] + 1)) {
            limit = strtol_dec(argv[i] + 1);
            byte_mode = 0;
            i++;
            continue;
        }
        /* lone "-" is a file operand */
        break;
    }

    if (i >= argc) {
        return byte_mode ? head_bytes(0, limit) : head_lines(0, limit);
    }

    nfiles = argc - i;
    for (fi = i; fi < argc; fi++) {
        long fd;
        const char *name = argv[fi];
        int show_hdr;

        if (strcmp(name, "-") == 0) {
            fd = 0;
            name = "standard input";
        } else {
            fd = u_open(argv[fi], 0, 0);
            if (fd < 0) {
                printf("head: %s: no such file\n", argv[fi]);
                rc = 1;
                continue;
            }
        }

        show_hdr = 0;
        if (!quiet) {
            if (verbose || nfiles > 1) show_hdr = 1;
        }
        if (show_hdr) {
            if (files > 0) u_write(1, "\n", 1);
            printf("==> %s <==\n", name);
        }

        if (byte_mode) {
            if (head_bytes(fd, limit) != 0) rc = 1;
        } else {
            if (head_lines(fd, limit) != 0) rc = 1;
        }
        if (fd != 0) u_close(fd);
        files++;
    }
    return rc;
}
