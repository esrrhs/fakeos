/* fakeos userland - rev: reverse characters of each record (thin).
 * Forms: rev [-z] [FILE...]
 * stdin if no FILE. Default records end at newline; -z uses NUL-terminated
 * records (input+output; GNU-ish). Lines/records longer than LINE_MAX-1 are
 * truncated for reverse. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);

enum { LINE_MAX = 512 };

static int opt_z; /* NUL records */

static void emit_rev(const char *line, int len) {
    int i;
    char nul = 0;
    for (i = len - 1; i >= 0; i--) {
        u_write(1, &line[i], 1);
    }
    if (opt_z) u_write(1, &nul, 1);
    else u_write(1, "\n", 1);
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
            if (opt_z ? (c == 0) : (c == '\n')) {
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

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-") == 0) break;
        if (strcmp(argv[i], "-z") == 0) {
            opt_z = 1;
            i++;
            continue;
        }
        u_write(2, "usage: rev [-z] [FILE...]\n", 26);
        return 1;
    }

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
