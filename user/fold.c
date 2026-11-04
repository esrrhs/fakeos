/* fakeos userland - fold: wrap lines to WIDTH columns (thin POSIX subset).
 * Forms: fold [-w WIDTH] [-s] [FILE...]
 * Default WIDTH 80. -s breaks at the last blank before WIDTH when possible.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);

enum { OUT_MAX = 512 };

static int width = 80;
static int break_spaces; /* -s */

static void usage(void) {
    u_write(2, "usage: fold [-w WIDTH] [-s] [FILE...]\n", 38);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

static void flush_buf(char *out, int *olen) {
    if (*olen > 0) {
        u_write(1, out, (unsigned long)(*olen));
        *olen = 0;
    }
}

static void fold_char(char c, char *out, int *olen, int *col) {
    if (c == '\n') {
        flush_buf(out, olen);
        u_write(1, "\n", 1);
        *col = 0;
        return;
    }
    if (c == '\b') {
        if (*col > 0) (*col)--;
        if (*olen > 0) (*olen)--;
        return;
    }
    if (c == '\r') {
        flush_buf(out, olen);
        u_write(1, "\r", 1);
        *col = 0;
        return;
    }
    if (c == '\t') {
        int spaces = 8 - (*col % 8);
        int k;
        for (k = 0; k < spaces; k++) fold_char(' ', out, olen, col);
        return;
    }

    if (*col >= width) {
        if (break_spaces && *olen > 0) {
            int last = -1;
            int j;
            for (j = 0; j < *olen; j++) {
                if (is_blank(out[j])) last = j;
            }
            if (last >= 0) {
                u_write(1, out, (unsigned long)(last + 1));
                u_write(1, "\n", 1);
                {
                    int rem = *olen - (last + 1);
                    int k;
                    for (k = 0; k < rem; k++) out[k] = out[last + 1 + k];
                    *olen = rem;
                    *col = rem;
                }
            } else {
                flush_buf(out, olen);
                u_write(1, "\n", 1);
                *col = 0;
            }
        } else {
            flush_buf(out, olen);
            u_write(1, "\n", 1);
            *col = 0;
        }
    }

    if (*olen < OUT_MAX - 1) {
        out[(*olen)++] = c;
        (*col)++;
    }
}

static int fold_fd(long fd) {
    char buf[512];
    char out[OUT_MAX];
    long n;
    int i;
    int olen = 0;
    int col = 0;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            fold_char(buf[i], out, &olen, &col);
        }
    }
    flush_buf(out, &olen);
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
        if (strcmp(argv[i], "-w") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            width = (int)strtol_dec(argv[i + 1]);
            if (width < 1) width = 1;
            if (width > OUT_MAX - 1) width = OUT_MAX - 1;
            i += 2;
        } else if (strcmp(argv[i], "-s") == 0) {
            break_spaces = 1;
            i++;
        } else if (argv[i][1] == 'w' && argv[i][2] != 0 && is_digits(argv[i] + 2)) {
            width = (int)strtol_dec(argv[i] + 2);
            if (width < 1) width = 1;
            if (width > OUT_MAX - 1) width = OUT_MAX - 1;
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        return fold_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (fold_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "fold: open failed\n", 18);
            rc = 1;
            continue;
        }
        if (fold_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
