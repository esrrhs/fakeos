/* fakeos userland - tac: concatenate and print files in reverse (thin).
 * Forms: tac [-z] [FILE...]
 * stdin if no FILE. Loads records into RAM (like sort); prints last first.
 * Default records end at newline; -z uses NUL-terminated records
 * (input+output; GNU-ish). Records longer than LINE_MAX-1 are truncated.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void *malloc(unsigned long size);
extern void free(void *ptr);

enum {
    LINE_MAX = 512,
    MAX_LINES = 4096
};

static int opt_z; /* NUL records */

static char *lines[MAX_LINES];
static int nlines;

static void clear_lines(void) {
    int i;
    for (i = 0; i < nlines; i++) {
        free(lines[i]);
        lines[i] = 0;
    }
    nlines = 0;
}

static int add_line(const char *s) {
    unsigned long n;
    char *p;
    if (nlines >= MAX_LINES) {
        u_write(2, "tac: too many lines\n", 20);
        return 1;
    }
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (p == 0) {
        u_write(2, "tac: out of memory\n", 19);
        return 1;
    }
    strcpy(p, s);
    lines[nlines++] = p;
    return 0;
}

static void emit_rev(void) {
    int i;
    char nul = 0;
    for (i = nlines - 1; i >= 0; i--) {
        unsigned long n = strlen(lines[i]);
        if (n > 0) u_write(1, lines[i], n);
        if (opt_z) u_write(1, &nul, 1);
        else u_write(1, "\n", 1);
    }
}

static int tac_fd(long fd) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;
    int rc = 0;

    clear_lines();
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (opt_z ? (c == 0) : (c == '\n')) {
                line[li] = 0;
                if (add_line(line) != 0) {
                    rc = 1;
                    clear_lines();
                    return rc;
                }
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        if (add_line(line) != 0) {
            clear_lines();
            return 1;
        }
    }
    if (n < 0) rc = 1;
    emit_rev();
    clear_lines();
    return rc;
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
        u_write(2, "usage: tac [-z] [FILE...]\n", 26);
        return 1;
    }

    if (i >= argc) {
        return tac_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (tac_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "tac: open failed\n", 17);
            rc = 1;
            continue;
        }
        if (tac_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
