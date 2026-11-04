/* fakeos userland - dirname: strip last path component. */
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

int main(int argc, char **argv) {
    char buf[256];
    unsigned long i;
    unsigned long n;
    long last_slash = -1;

    if (argc != 2) {
        u_write(2, "usage: dirname PATH\n", 20);
        return 1;
    }

    n = strlen(argv[1]);
    if (n >= sizeof(buf)) {
        u_write(2, "dirname: path too long\n", 23);
        return 1;
    }
    for (i = 0; i <= n; i++) buf[i] = argv[1][i];

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
