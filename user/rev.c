/* fakeos userland - rev: reverse characters of each line (thin).
 * Forms: rev [FILE...]
 * stdin if no FILE. Lines longer than LINE_MAX-1 are truncated for reverse.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);

enum { LINE_MAX = 512 };

static void emit_rev(const char *line, int len) {
    int i;
    for (i = len - 1; i >= 0; i--) {
        u_write(1, &line[i], 1);
    }
    u_write(1, "\n", 1);
}

static int rev_fd(long fd) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[li] = 0;
                emit_rev(line, li);
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        emit_rev(line, li);
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    if (i < argc && strcmp(argv[i], "--") == 0) i++;

    if (i >= argc) {
        return rev_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (rev_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "rev: open failed\n", 17);
            rc = 1;
            continue;
        }
        if (rev_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
