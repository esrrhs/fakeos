/* fakeos userland - comm: compare two sorted files line-by-line (thin POSIX).
 * Forms: comm [-123] FILE1 FILE2
 * Three columns: unique to FILE1, unique to FILE2, common. Leading tabs only
 * between columns that are actually printed. "-" is stdin (at most one).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);

enum { LINE_MAX = 512 };

static int show1 = 1;
static int show2 = 1;
static int show3 = 1;

static void usage(void) {
    u_write(2, "usage: comm [-123] FILE1 FILE2\n", 31);
}

static int read_line(long fd, char *out, int *len_out) {
    char c;
    long n;
    int li = 0;
    while (1) {
        n = u_read(fd, &c, 1);
        if (n < 0) return -1;
        if (n == 0) {
            *len_out = li;
            return li > 0 ? 1 : 0;
        }
        if (c == '\n') {
            out[li] = 0;
            *len_out = li;
            return 1;
        }
        if (li < LINE_MAX - 1) out[li++] = c;
    }
}

static void emit_tabs(int n) {
    while (n-- > 0) u_write(1, "\t", 1);
}

/* col: 1, 2, or 3. Emit leading tabs for earlier *shown* columns. */
static void emit_line(int col, const char *line, int len) {
    int tabs = 0;
    if (col >= 2 && show1) tabs++;
    if (col >= 3 && show2) tabs++;
    emit_tabs(tabs);
    if (len > 0) u_write(1, line, (unsigned long)len);
    u_write(1, "\n", 1);
}

static int open_in(const char *path, long *fd_out) {
    long fd;
    if (strcmp(path, "-") == 0) {
        *fd_out = 0;
        return 0;
    }
    fd = u_open(path, 0, 0);
    if (fd < 0) {
        u_write(2, "comm: open failed\n", 18);
        return 1;
    }
    *fd_out = fd;
    return 0;
}

static int parse_suppress(const char *a) {
    int i;
    if (a[0] != '-' || a[1] == 0) return 0;
    for (i = 1; a[i]; i++) {
        if (a[i] == '1') show1 = 0;
        else if (a[i] == '2') show2 = 0;
        else if (a[i] == '3') show3 = 0;
        else return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int i = 1;
    long fd1;
    long fd2;
    char line1[LINE_MAX];
    char line2[LINE_MAX];
    int len1 = 0;
    int len2 = 0;
    int have1 = 0;
    int have2 = 0;
    int r;
    int stdin_used = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (!parse_suppress(argv[i])) {
            usage();
            return 1;
        }
        i++;
    }
    if (i + 2 != argc) {
        usage();
        return 1;
    }
    if (strcmp(argv[i], "-") == 0) stdin_used = 1;
    if (strcmp(argv[i + 1], "-") == 0) {
        if (stdin_used) {
            u_write(2, "comm: only one stdin\n", 21);
            return 1;
        }
    }
    if (open_in(argv[i], &fd1) != 0) return 1;
    if (open_in(argv[i + 1], &fd2) != 0) {
        if (fd1 != 0) u_close(fd1);
        return 1;
    }

    r = read_line(fd1, line1, &len1);
    if (r < 0) return 1;
    have1 = r;
    r = read_line(fd2, line2, &len2);
    if (r < 0) return 1;
    have2 = r;

    while (have1 || have2) {
        int cmp;
        if (!have1) cmp = 1;
        else if (!have2) cmp = -1;
        else cmp = strcmp(line1, line2);

        if (cmp < 0) {
            if (show1) emit_line(1, line1, len1);
            r = read_line(fd1, line1, &len1);
            if (r < 0) return 1;
            have1 = r;
        } else if (cmp > 0) {
            if (show2) emit_line(2, line2, len2);
            r = read_line(fd2, line2, &len2);
            if (r < 0) return 1;
            have2 = r;
        } else {
            if (show3) emit_line(3, line1, len1);
            r = read_line(fd1, line1, &len1);
            if (r < 0) return 1;
            have1 = r;
            r = read_line(fd2, line2, &len2);
            if (r < 0) return 1;
            have2 = r;
        }
    }

    if (fd1 != 0) u_close(fd1);
    if (fd2 != 0) u_close(fd2);
    return 0;
}
