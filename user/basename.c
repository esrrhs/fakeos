/* fakeos userland - basename: strip directory and optional suffix (thin GNU).
 *   basename [-z] PATH [SUFFIX]
 *   basename [-z] -a PATH...
 *   basename [-z] -s SUFFIX PATH...
 * Traditional form keeps optional SUFFIX as the second operand. -a prints
 * one basename per PATH (no suffix). -s applies SUFFIX to every PATH.
 * -z ends each result with NUL (pairs with find -print0 / xargs -0).
 * Compact argv for EXEC_MAX_ARGC=8. Buffer 256.
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

static int basename_one(const char *path, const char *suf) {
    char buf[256];
    char *base;
    unsigned long i;
    unsigned long n;

    n = strlen(path);
    if (n >= sizeof(buf)) {
        u_write(2, "basename: path too long\n", 24);
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
    base = last_component(buf);
    strip_suffix(base, suf);
    emit_result(base);
    return 0;
}

static void usage(void) {
    u_write(2, "usage: basename [-z] PATH [SUFFIX]\n", 35);
    u_write(2, "       basename [-z] -a PATH...\n", 32);
    u_write(2, "       basename [-z] -s SUFFIX PATH...\n", 39);
}

int main(int argc, char **argv) {
    int i;
    int rc = 0;
    int multi = 0;
    const char *suf = 0;

    if (argc < 2) {
        usage();
        return 1;
    }

    opt_z = 0;
    i = 1;
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
        if (strcmp(argv[i], "-a") == 0) {
            multi = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-az") == 0 || strcmp(argv[i], "-za") == 0) {
            multi = 1;
            opt_z = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            suf = argv[i + 1];
            multi = 1;
            i += 2;
            continue;
        }
        /* unknown dash option — treat as a path operand */
        break;
    }

    if (i >= argc) {
        usage();
        return 1;
    }

    if (!multi) {
        /* Traditional: basename PATH [SUFFIX] */
        if (argc - i > 2) {
            usage();
            return 1;
        }
        if (argc - i == 2) suf = argv[i + 1];
        return basename_one(argv[i], suf);
    }

    for (; i < argc; i++) {
        if (basename_one(argv[i], suf) != 0) rc = 1;
    }
    return rc;
}
