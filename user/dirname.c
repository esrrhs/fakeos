/* fakeos userland - dirname: strip last path component (thin POSIX/GNU).
 *   dirname [-z] PATH...
 * One or more operands; each dirname printed on its own line (GNU multi-arg).
 * -z ends each result with NUL (pairs with find -print0 / xargs -0).
 * Trailing slashes stripped; "/" stays "/"; no slash -> ".". Buffer 256.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

static int opt_z; /* -z: NUL-terminated output */

static void emit_result(const char *s) {
    unsigned long n = strlen(s);
    if (n > 0) u_write(1, s, n);
    if (opt_z) {
        char z = 0;
        u_write(1, &z, 1);
    } else {
        u_write(1, "\n", 1);
    }
}

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
        emit_result(".");
        return 0;
    }
    strip_trailing_slashes(buf);
    if (buf[0] == '/' && buf[1] == 0) {
        emit_result("/");
        return 0;
    }

    for (i = 0; buf[i]; i++) {
        if (buf[i] == '/') last_slash = (long)i;
    }
    if (last_slash < 0) {
        emit_result(".");
        return 0;
    }
    if (last_slash == 0) {
        emit_result("/");
        return 0;
    }
    buf[last_slash] = 0;
    strip_trailing_slashes(buf);
    emit_result(buf);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    opt_z = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-z") == 0) {
            opt_z = 1;
            i++;
            continue;
        }
        break;
    }
    if (i >= argc) {
        u_write(2, "usage: dirname [-z] PATH...\n", 28);
        return 1;
    }
    for (; i < argc; i++) {
        if (dirname_one(argv[i]) != 0) rc = 1;
    }
    return rc;
}
