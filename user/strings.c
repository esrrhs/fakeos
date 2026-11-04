/* fakeos userland - strings: extract printable runs (thin).
 * Forms: strings [-a] [-n N|-nN] [-t o|d|x|-tRADIX] [FILE...]
 * Default min length 4. Printable is 0x20..0x7e. Always scans the whole
 * stream (-a accepted as no-op). -t prints byte offset before each string
 * (octal/decimal/hex). Glued -nN/-tR keep argv under EXEC_MAX_ARGC=8.
 * stdin if no FILE. Runs longer than STR_MAX-1 are truncated on emit.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);

enum { MIN_DEFAULT = 4, STR_MAX = 512 };

static int min_len = MIN_DEFAULT;
static int opt_t; /* 0 off; else 'o'/'d'/'x' */

static void usage(void) {
    u_write(2, "usage: strings [-a] [-n N] [-t o|d|x] [FILE...]\n", 48);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static int is_print(unsigned char c) {
    return c >= 0x20 && c <= 0x7e;
}

static void emit_u(unsigned long v, int base) {
    char tmp[24];
    int i = 0;
    int k;
    const char *digits = "0123456789abcdef";
    if (base != 8 && base != 10 && base != 16) base = 10;
    if (v == 0) {
        u_write(1, "0", 1);
        return;
    }
    while (v > 0 && i < 24) {
        tmp[i++] = digits[v % (unsigned long)base];
        v /= (unsigned long)base;
    }
    for (k = i - 1; k >= 0; k--) u_write(1, &tmp[k], 1);
}

static void emit_run(const char *str, int len, unsigned long off) {
    int out = len;
    if (opt_t) {
        int base = 10;
        if (opt_t == 'o') base = 8;
        else if (opt_t == 'x') base = 16;
        emit_u(off, base);
        u_write(1, " ", 1);
    }
    if (out > STR_MAX - 1) out = STR_MAX - 1;
    if (out > 0) u_write(1, str, (unsigned long)out);
    u_write(1, "\n", 1);
}

static int strings_fd(long fd) {
    char buf[512];
    char str[STR_MAX];
    long n;
    int i;
    int len = 0;
    unsigned long off = 0;
    unsigned long run_off = 0;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (is_print(c)) {
                if (len == 0) run_off = off;
                if (len < STR_MAX - 1) str[len] = (char)c;
                len++;
            } else {
                if (len >= min_len) emit_run(str, len, run_off);
                len = 0;
            }
            off++;
        }
    }
    if (len >= min_len) emit_run(str, len, run_off);
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    min_len = MIN_DEFAULT;
    opt_t = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-") == 0) {
            break;
        }
        if (strcmp(argv[i], "-a") == 0) {
            /* whole-file scan is the only mode */
            i++;
            continue;
        }
        if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == 0 || argv[i + 1][1] != 0) {
                usage();
                return 1;
            }
            if (argv[i + 1][0] != 'o' && argv[i + 1][0] != 'd' &&
                argv[i + 1][0] != 'x') {
                usage();
                return 1;
            }
            opt_t = (int)(unsigned char)argv[i + 1][0];
            i += 2;
            continue;
        }
        if (argv[i][1] == 't' && argv[i][2] != 0 && argv[i][3] == 0 &&
            (argv[i][2] == 'o' || argv[i][2] == 'd' || argv[i][2] == 'x')) {
            opt_t = (int)(unsigned char)argv[i][2];
            i++;
            continue;
        }
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) {
                usage();
                return 1;
            }
            min_len = (int)strtol_dec(argv[i + 1]);
            if (min_len < 1) min_len = 1;
            i += 2;
            continue;
        }
        if (argv[i][1] == 'n' && is_digits(argv[i] + 2)) {
            min_len = (int)strtol_dec(argv[i] + 2);
            if (min_len < 1) min_len = 1;
            i++;
            continue;
        }
        usage();
        return 1;
    }

    if (i >= argc) {
        return strings_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (strings_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "strings: open failed\n", 21);
            rc = 1;
            continue;
        }
        if (strings_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
