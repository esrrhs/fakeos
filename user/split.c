/* fakeos userland - split: split a file into pieces (thin POSIX subset).
 * Forms: split [-d] [-l LINES|-lLINES] [-b SIZE|-bSIZE] [-a N|-aN]
 *              [FILE [PREFIX]]
 * Default: 1000 lines, PREFIX "x", suffix length 2 (xaa, xab, ...).
 * -d: numeric suffixes (x00, x01, ...) instead of alphabetic; -a sets width.
 * -b SIZE splits by bytes (decimal; optional k/K=1024, m/M=1MiB, b/B=512).
 * -l and -b: last one wins. FILE "-" or omitted reads stdin.
 * Glued -lN/-bN/-aN/-d keep argv under EXEC_MAX_ARGC=8.
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
    SUFF_MAX = 8,
    OUT_MAX = 512
};

static int lines_per = 1000;
static unsigned long bytes_per = 0; /* 0 = line mode */
static int byte_mode = 0;
static int suff_len = 2;
static int numeric_suf; /* -d: decimal suffixes */
static const char *prefix = "x";

static void usage(void) {
    u_write(2, "usage: split [-d] [-l LINES] [-b SIZE] [-a N] [FILE [PREFIX]]\n", 62);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

/* Parse SIZE: decimal digits with optional trailing k/K/m/M/b/B. */
static int parse_size(const char *s, unsigned long *out) {
    unsigned long v = 0;
    int i = 0;
    char c;
    if (s == 0 || s[0] == 0) return 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10UL + (unsigned long)(s[i] - '0');
        i++;
    }
    if (i == 0) return 0;
    c = s[i];
    if (c == 0) {
        *out = v;
        return 1;
    }
    if (s[i + 1] != 0) return 0;
    if (c == 'k' || c == 'K') {
        v = v * 1024UL;
    } else if (c == 'm' || c == 'M') {
        v = v * 1024UL * 1024UL;
    } else if (c == 'b' || c == 'B') {
        v = v * 512UL;
    } else {
        return 0;
    }
    *out = v;
    return 1;
}

/* Encode idx as alphabetic (base-26) or numeric (base-10, -d) suffix. */
static int encode_suffix(char *out, int idx) {
    int i;
    int v = idx;
    int max = 1;
    int base = numeric_suf ? 10 : 26;
    for (i = 0; i < suff_len; i++) max *= base;
    if (idx < 0 || idx >= max) return 0;
    for (i = suff_len - 1; i >= 0; i--) {
        int dig = v % base;
        out[i] = numeric_suf ? (char)('0' + dig) : (char)('a' + dig);
        v /= base;
    }
    out[suff_len] = 0;
    return 1;
}

static int build_path(char *path, int idx) {
    unsigned long pl = strlen(prefix);
    char suf[SUFF_MAX + 1];
    int i;
    if (pl + (unsigned long)suff_len + 1 > PATH_MAX) return 0;
    if (!encode_suffix(suf, idx)) return 0;
    for (i = 0; i < (int)pl; i++) path[i] = prefix[i];
    for (i = 0; i < suff_len; i++) path[pl + i] = suf[i];
    path[pl + suff_len] = 0;
    return 1;
}

static long open_piece(int idx) {
    char path[PATH_MAX];
    long fd;
    if (!build_path(path, idx)) {
        u_write(2, "split: suffix exhausted\n", 24);
        return -1;
    }
    fd = u_open(path, O_WRONLY | O_CREAT | O_TRUNC, MODE_0644);
    if (fd < 0) {
        u_write(2, "split: open failed\n", 19);
        return -1;
    }
    return fd;
}

static int do_split_lines(long in_fd) {
    char buf[OUT_MAX];
    long n;
    int i;
    int line_count = 0;
    int piece = 0;
    long out = -1;
    int need_new = 1;

    while ((n = u_read(in_fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            if (need_new) {
                out = open_piece(piece);
                if (out < 0) return 1;
                need_new = 0;
                line_count = 0;
            }
            if (u_write(out, &buf[i], 1) != 1) {
                u_write(2, "split: write failed\n", 20);
                u_close(out);
                return 1;
            }
            if (buf[i] == '\n') {
                line_count++;
                if (line_count >= lines_per) {
                    u_close(out);
                    out = -1;
                    piece++;
                    need_new = 1;
                }
            }
        }
    }
    if (out >= 0) u_close(out);
    /* Empty input: create one empty piece (xaa), matching common thin split. */
    if (n == 0 && piece == 0 && need_new) {
        out = open_piece(0);
        if (out < 0) return 1;
        u_close(out);
    }
    return n < 0 ? 1 : 0;
}

static int do_split_bytes(long in_fd) {
    char buf[OUT_MAX];
    long n;
    int i;
    unsigned long count = 0;
    int piece = 0;
    long out = -1;
    int need_new = 1;

    if (bytes_per < 1) bytes_per = 1;

    while ((n = u_read(in_fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            if (need_new) {
                out = open_piece(piece);
                if (out < 0) return 1;
                need_new = 0;
                count = 0;
            }
            if (u_write(out, &buf[i], 1) != 1) {
                u_write(2, "split: write failed\n", 20);
                u_close(out);
                return 1;
            }
            count++;
            if (count >= bytes_per) {
                u_close(out);
                out = -1;
                piece++;
                need_new = 1;
            }
        }
    }
    if (out >= 0) u_close(out);
    if (n == 0 && piece == 0 && need_new) {
        out = open_piece(0);
        if (out < 0) return 1;
        u_close(out);
    }
    return n < 0 ? 1 : 0;
}

static int do_split(long in_fd) {
    if (byte_mode) return do_split_bytes(in_fd);
    return do_split_lines(in_fd);
}

int main(int argc, char **argv) {
    int i = 1;
    const char *file = 0;
    long fd;

    numeric_suf = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-") == 0) break; /* stdin file operand */
        if (strcmp(a, "-d") == 0) {
            numeric_suf = 1;
            i++;
        } else if (strcmp(a, "-l") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            lines_per = (int)strtol_dec(argv[i + 1]);
            if (lines_per < 1) lines_per = 1;
            byte_mode = 0;
            i += 2;
        } else if (a[1] == 'l' && a[2] != 0 && is_digits(a + 2)) {
            lines_per = (int)strtol_dec(a + 2);
            if (lines_per < 1) lines_per = 1;
            byte_mode = 0;
            i++;
        } else if (strcmp(a, "-b") == 0) {
            if (i + 1 >= argc || !parse_size(argv[i + 1], &bytes_per)) {
                usage();
                return 1;
            }
            if (bytes_per < 1) bytes_per = 1;
            byte_mode = 1;
            i += 2;
        } else if (a[1] == 'b' && a[2] != 0) {
            if (!parse_size(a + 2, &bytes_per)) { usage(); return 1; }
            if (bytes_per < 1) bytes_per = 1;
            byte_mode = 1;
            i++;
        } else if (strcmp(a, "-a") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            suff_len = (int)strtol_dec(argv[i + 1]);
            if (suff_len < 1) suff_len = 1;
            if (suff_len > SUFF_MAX) suff_len = SUFF_MAX;
            i += 2;
        } else if (a[1] == 'a' && a[2] != 0 && is_digits(a + 2)) {
            suff_len = (int)strtol_dec(a + 2);
            if (suff_len < 1) suff_len = 1;
            if (suff_len > SUFF_MAX) suff_len = SUFF_MAX;
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i < argc) {
        file = argv[i++];
    }
    if (i < argc) {
        prefix = argv[i++];
    }
    if (i != argc) {
        usage();
        return 1;
    }

    if (file == 0 || strcmp(file, "-") == 0) {
        return do_split(0);
    }
    fd = u_open(file, 0, 0);
    if (fd < 0) {
        u_write(2, "split: open failed\n", 19);
        return 1;
    }
    {
        int rc = do_split(fd);
        u_close(fd);
        return rc;
    }
}
