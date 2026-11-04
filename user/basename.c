/* fakeos userland - basename: strip directory and optional suffix. */
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

static char *last_component(char *s) {
    unsigned long i;
    char *last = s;
    for (i = 0; s[i]; i++) {
        if (s[i] == '/' && s[i + 1] != 0) {
            last = &s[i + 1];
        }
    }
    return last;
}

static void strip_suffix(char *name, const char *suf) {
    unsigned long nl;
    unsigned long sl;
    unsigned long i;
    if (suf == 0 || suf[0] == 0) return;
    nl = strlen(name);
    sl = strlen(suf);
    if (sl >= nl) return;
    for (i = 0; i < sl; i++) {
        if (name[nl - sl + i] != suf[i]) return;
    }
    name[nl - sl] = 0;
}

int main(int argc, char **argv) {
    char buf[256];
    char *base;
    const char *suf = 0;
    unsigned long i;
    unsigned long n;

    if (argc < 2 || argc > 3) {
        u_write(2, "usage: basename PATH [SUFFIX]\n", 30);
        return 1;
    }
    if (argc == 3) suf = argv[2];

    n = strlen(argv[1]);
    if (n >= sizeof(buf)) {
        u_write(2, "basename: path too long\n", 24);
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
    base = last_component(buf);
    strip_suffix(base, suf);
    printf("%s\n", base);
    return 0;
}
