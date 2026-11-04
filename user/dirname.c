/* fakeos userland - dirname: strip last path component (thin POSIX/GNU).
 *   dirname PATH...
 * One or more operands; each dirname printed on its own line (GNU multi-arg).
 * Trailing slashes stripped; "/" stays "/"; no slash -> ".". Buffer 256.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);

static void strip_trailing_slashes(char *s) {
    unsigned long n = strlen(s);
    while (n > 1 && s[n - 1] == '/') {
        s[n - 1] = 0;
        n--;
    }
}

static int dirname_one(const char *path) {
    char buf[256];
    unsigned long i;
    unsigned long n;
    long last_slash = -1;

    n = strlen(path);
    if (n >= sizeof(buf)) {
        u_write(2, "dirname: path too long\n", 23);
        return 1;
    }
    for (i = 0; i <= n; i++) buf[i] = path[i];

    if (buf[0] == 0) {
        u_write(1, ".\n", 2);
        return 0;
    }
    strip_trailing_slashes(buf);
    if (buf[0] == '/' && buf[1] == 0) {
        u_write(1, "/\n", 2);
        return 0;
    }

    for (i = 0; buf[i]; i++) {
        if (buf[i] == '/') last_slash = (long)i;
    }
    if (last_slash < 0) {
        u_write(1, ".\n", 2);
        return 0;
    }
    if (last_slash == 0) {
        u_write(1, "/\n", 2);
        return 0;
    }
    buf[last_slash] = 0;
    strip_trailing_slashes(buf);
    printf("%s\n", buf);
    return 0;
}

int main(int argc, char **argv) {
    int i;
    int rc = 0;

    if (argc < 2) {
        u_write(2, "usage: dirname PATH...\n", 23);
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (dirname_one(argv[i]) != 0) rc = 1;
    }
    return rc;
}
