/* fakeos userland - split: split a file into pieces (thin POSIX subset).
 * Forms: split [-l LINES|-lLINES] [-a N|-aN] [FILE [PREFIX]]
 * Default: 1000 lines, PREFIX "x", suffix length 2 (xaa, xab, ...).
 * FILE "-" or omitted reads stdin. Glued -lN/-aN keep argv under EXEC_MAX_ARGC=8.
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
static int suff_len = 2;
static const char *prefix = "x";

static void usage(void) {
    u_write(2, "usage: split [-l LINES] [-a N] [FILE [PREFIX]]\n", 48);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

/* Encode idx as base-26 alphabetic suffix of length suff_len (aa..zz). */
static int encode_suffix(char *out, int idx) {
    int i;
    int v = idx;
    int max = 1;
    for (i = 0; i < suff_len; i++) max *= 26;
    if (idx < 0 || idx >= max) return 0;
    for (i = suff_len - 1; i >= 0; i--) {
        out[i] = (char)('a' + (v % 26));
        v /= 26;
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

static int do_split(long in_fd) {
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

int main(int argc, char **argv) {
    int i = 1;
    const char *file = 0;
    long fd;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-") == 0) break; /* stdin file operand */
        if (strcmp(a, "-l") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            lines_per = (int)strtol_dec(argv[i + 1]);
            if (lines_per < 1) lines_per = 1;
            i += 2;
        } else if (a[1] == 'l' && a[2] != 0 && is_digits(a + 2)) {
            lines_per = (int)strtol_dec(a + 2);
            if (lines_per < 1) lines_per = 1;
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
