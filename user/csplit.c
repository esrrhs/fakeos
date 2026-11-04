/* fakeos userland - csplit: split a file by line numbers (thin POSIX subset).
 * Forms: csplit [-s] [-f PREFIX|-fPREFIX] [-n DIGITS|-nDIGITS] FILE LINE...
 * LINE is 1-based; each LINE starts a new piece before that line. Remainder
 * is the last piece. PREFIX default "xx", DIGITS default 2 (xx00, xx01, ...).
 * Without -s, each created piece path is printed (one per line). -s is silent
 * (GNU --quiet). FILE "-" reads stdin. Glued -f/-n keep EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern long strtol_dec(const char *s);

enum {
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200,
    MODE_0644 = 0x1A4,
    PATH_MAX = 128,
    DIGIT_MAX = 8,
    LINE_ARGS_MAX = 8,
    BUF_MAX = 512
};

static const char *prefix = "xx";
static int digits = 2;
static int opt_s; /* -s: suppress piece-path prints */
static long breaks[LINE_ARGS_MAX];
static int nbreaks;

static void usage(void) {
    u_write(2, "usage: csplit [-s] [-f PREFIX] [-n DIGITS] FILE LINE...\n", 56);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static int build_path(char *path, int idx) {
    unsigned long pl = strlen(prefix);
    int i;
    int v = idx;
    int pow10 = 1;
    if (pl + (unsigned long)digits + 1 > PATH_MAX) return 0;
    for (i = 0; i < digits; i++) pow10 *= 10;
    if (idx < 0 || idx >= pow10) return 0;
    for (i = 0; i < (int)pl; i++) path[i] = prefix[i];
    for (i = digits - 1; i >= 0; i--) {
        path[pl + i] = (char)('0' + (v % 10));
        v /= 10;
    }
    path[pl + digits] = 0;
    return 1;
}

static long open_piece(int idx) {
    char path[PATH_MAX];
    long fd;
    if (!build_path(path, idx)) {
        u_write(2, "csplit: suffix exhausted\n", 25);
        return -1;
    }
    fd = u_open(path, O_WRONLY | O_CREAT | O_TRUNC, MODE_0644);
    if (fd < 0) {
        u_write(2, "csplit: open failed\n", 20);
        return -1;
    }
    /* Print created path (one per line) unless -s / --quiet. */
    if (!opt_s) {
        unsigned long n = 0;
        while (path[n]) n++;
        u_write(1, path, n);
        u_write(1, "\n", 1);
    }
    return fd;
}

static int next_break_line(int piece) {
    /* piece 0 runs until breaks[0]; piece k until breaks[k]; last has none. */
    if (piece < nbreaks) return (int)breaks[piece];
    return -1; /* no further break */
}

static int do_csplit(long in_fd) {
    char buf[BUF_MAX];
    long n;
    int i;
    int cur_line = 1; /* 1-based line of next byte */
    int piece = 0;
    long out;
    int target;

    out = open_piece(piece);
    if (out < 0) return 1;
    target = next_break_line(piece);

    while ((n = u_read(in_fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            while (target > 0 && cur_line == target) {
                u_close(out);
                piece++;
                out = open_piece(piece);
                if (out < 0) return 1;
                target = next_break_line(piece);
            }
            if (u_write(out, &buf[i], 1) != 1) {
                u_write(2, "csplit: write failed\n", 21);
                u_close(out);
                return 1;
            }
            if (buf[i] == '\n') cur_line++;
        }
    }
    if (out >= 0) u_close(out);
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    const char *file = 0;
    long fd;

    opt_s = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-") == 0) break;
        if (strcmp(a, "-s") == 0) {
            opt_s = 1;
            i++;
            continue;
        }
        if (strcmp(a, "-f") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            prefix = argv[i + 1];
            i += 2;
        } else if (a[1] == 'f' && a[2] != 0) {
            prefix = a + 2;
            i++;
        } else if (strcmp(a, "-n") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            digits = (int)strtol_dec(argv[i + 1]);
            if (digits < 1) digits = 1;
            if (digits > DIGIT_MAX) digits = DIGIT_MAX;
            i += 2;
        } else if (a[1] == 'n' && a[2] != 0 && is_digits(a + 2)) {
            digits = (int)strtol_dec(a + 2);
            if (digits < 1) digits = 1;
            if (digits > DIGIT_MAX) digits = DIGIT_MAX;
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) { usage(); return 1; }
    file = argv[i++];
    if (i >= argc) { usage(); return 1; } /* need at least one LINE */

    nbreaks = 0;
    for (; i < argc; i++) {
        long v;
        if (!is_digits(argv[i])) { usage(); return 1; }
        if (nbreaks >= LINE_ARGS_MAX) {
            u_write(2, "csplit: too many LINE args\n", 27);
            return 1;
        }
        v = strtol_dec(argv[i]);
        if (v < 1) { usage(); return 1; }
        /* breaks must be strictly increasing */
        if (nbreaks > 0 && v <= breaks[nbreaks - 1]) {
            u_write(2, "csplit: LINE not ascending\n", 27);
            return 1;
        }
        breaks[nbreaks++] = v;
    }

    if (file == 0 || strcmp(file, "-") == 0) {
        return do_csplit(0);
    }
    fd = u_open(file, 0, 0);
    if (fd < 0) {
        u_write(2, "csplit: open failed\n", 20);
        return 1;
    }
    {
        int rc = do_csplit(fd);
        u_close(fd);
        return rc;
    }
}
