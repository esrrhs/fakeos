/* fakeos userland - readlink: print symlink target (thin; mirrors shell).
 *   readlink [-n] [-f] [-q] [-z] PATH
 * Default: readlink(2) + trailing newline. -n omits the newline. -z ends
 * with NUL instead of newline (wins over -n). -f canonicalizes via readlink
 * walks (same algorithm as /bin/realpath -f style; last component may be
 * missing); prints the physical path even when PATH is not a symlink. -q
 * quiets error messages (GNU --quiet/--silent; exit status still non-zero).
 * Glued short options (-nfqz/...) accepted. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_readlink(const char *path, char *buf, unsigned long n);
extern long u_getcwd(char *buf, unsigned long len);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);

enum { PATH_CAP = 128 };

/* Lexical join of absolute base + relative rel into out (cap bytes). */
static int path_join(char *out, unsigned long cap, const char *base,
                     const char *rel) {
    unsigned long i = 0;
    unsigned long j = 0;
    if (cap < 2) {
        return 0;
    }
    if (rel[0] == '/') {
        while (rel[j] != 0 && i + 1 < cap) {
            out[i++] = rel[j++];
        }
        out[i] = 0;
        return 1;
    }
    while (base[i] != 0 && i + 1 < cap) {
        out[i] = base[i];
        i++;
    }
    if (i == 1 && out[0] == '/') {
        /* root */
    } else if (i > 0 && out[i - 1] != '/' && i + 1 < cap) {
        out[i++] = '/';
    }
    while (rel[j] != 0 && i + 1 < cap) {
        out[i++] = rel[j++];
    }
    out[i] = 0;
    return 1;
}

/* Userland physical resolve via readlink (mirrors /bin/realpath). */
static int urealpath(const char *in, char *out, unsigned long cap) {
    char work[PATH_CAP];
    char prefix[PATH_CAP];
    char tgt[PATH_CAP];
    char rebuilt[PATH_CAP];
    unsigned long loop;
    unsigned long i;
    unsigned long nlen;

    if (in == 0 || out == 0 || cap < 2) {
        return 0;
    }
    if (in[0] == '/') {
        nlen = strlen(in);
        if (nlen + 1 > sizeof(work)) {
            return 0;
        }
        for (i = 0; i <= nlen; i++) {
            work[i] = in[i];
        }
    } else {
        char cwd[PATH_CAP];
        if (u_getcwd(cwd, sizeof(cwd)) == -1) {
            return 0;
        }
        if (!path_join(work, sizeof(work), cwd, in)) {
            return 0;
        }
    }

    for (loop = 0; loop < 8; loop++) {
        int found = 0;
        unsigned long wi = 0;
        prefix[0] = '/';
        prefix[1] = 0;
        while (work[wi] != 0) {
            unsigned long start;
            unsigned long n;
            long rn;
            while (work[wi] == '/') {
                wi++;
            }
            if (work[wi] == 0) {
                break;
            }
            start = wi;
            n = 0;
            while (work[wi + n] != 0 && work[wi + n] != '/') {
                n++;
            }
            if (n == 1 && work[start] == '.') {
                wi += n;
                continue;
            }
            if (n == 2 && work[start] == '.' && work[start + 1] == '.') {
                unsigned long L = strlen(prefix);
                if (L > 1) {
                    L--;
                    while (L > 0 && prefix[L] != '/') {
                        L--;
                    }
                    if (L == 0) {
                        prefix[0] = '/';
                        prefix[1] = 0;
                    } else {
                        prefix[L] = 0;
                    }
                }
                wi += n;
                continue;
            }
            {
                char comp[64];
                char joined[PATH_CAP];
                unsigned long c;
                if (n >= sizeof(comp)) {
                    return 0;
                }
                for (c = 0; c < n; c++) {
                    comp[c] = work[start + c];
                }
                comp[n] = 0;
                if (!path_join(joined, sizeof(joined), prefix, comp)) {
                    return 0;
                }
                for (c = 0; joined[c] != 0; c++) {
                    prefix[c] = joined[c];
                }
                prefix[c] = 0;
            }
            wi += n;
            rn = u_readlink(prefix, tgt, sizeof(tgt) - 1);
            if (rn >= 0) {
                const char *rest;
                tgt[rn] = 0;
                if (tgt[0] == '/') {
                    nlen = strlen(tgt);
                    if (nlen + 1 > sizeof(rebuilt)) {
                        return 0;
                    }
                    for (i = 0; i <= nlen; i++) {
                        rebuilt[i] = tgt[i];
                    }
                } else {
                    unsigned long L = strlen(prefix);
                    char parent[PATH_CAP];
                    if (L > 1) {
                        L--;
                        while (L > 0 && prefix[L] != '/') {
                            L--;
                        }
                        if (L == 0) {
                            parent[0] = '/';
                            parent[1] = 0;
                        } else {
                            unsigned long p;
                            for (p = 0; p < L; p++) {
                                parent[p] = prefix[p];
                            }
                            parent[L] = 0;
                        }
                    } else {
                        parent[0] = '/';
                        parent[1] = 0;
                    }
                    if (!path_join(rebuilt, sizeof(rebuilt), parent, tgt)) {
                        return 0;
                    }
                }
                rest = &work[wi];
                while (rest[0] == '/') {
                    rest++;
                }
                if (rest[0] != 0) {
                    char combined[PATH_CAP];
                    if (!path_join(combined, sizeof(combined), rebuilt, rest)) {
                        return 0;
                    }
                    nlen = strlen(combined);
                    for (i = 0; i <= nlen; i++) {
                        rebuilt[i] = combined[i];
                    }
                }
                nlen = strlen(rebuilt);
                if (nlen + 1 > sizeof(work)) {
                    return 0;
                }
                for (i = 0; i <= nlen; i++) {
                    work[i] = rebuilt[i];
                }
                found = 1;
                break;
            }
        }
        if (!found) {
            nlen = strlen(work);
            if (nlen + 1 > cap) {
                return 0;
            }
            for (i = 0; i <= nlen; i++) {
                out[i] = work[i];
            }
            return 1;
        }
    }
    return 0;
}

static void usage(void) {
    u_write(2, "usage: readlink [-nfqz] PATH\n", 30);
}

int main(int argc, char **argv) {
    char buf[PATH_CAP];
    long n;
    int ai = 1;
    int no_nl = 0;
    int canon = 0;
    int opt_z = 0;
    int opt_q = 0;
    const char *path;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        int j;
        if (strcmp(argv[ai], "--") == 0) {
            ai++;
            break;
        }
        for (j = 1; argv[ai][j]; j++) {
            char o = argv[ai][j];
            if (o == 'n') no_nl = 1;
            else if (o == 'f') canon = 1;
            else if (o == 'z') opt_z = 1;
            else if (o == 'q') opt_q = 1;
            else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    if (ai + 1 != argc) {
        usage();
        return 1;
    }
    path = argv[ai];

    if (canon) {
        if (!urealpath(path, buf, sizeof(buf))) {
            if (!opt_q) u_write(2, "readlink: failed\n", 17);
            return 1;
        }
        n = (long)strlen(buf);
    } else {
        n = u_readlink(path, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (!opt_q) u_write(2, "readlink: failed\n", 17);
            return 1;
        }
        buf[n] = 0;
    }

    if (n > 0) u_write(1, buf, (unsigned long)n);
    if (opt_z) {
        char z = 0;
        u_write(1, &z, 1);
    } else if (!no_nl) {
        u_write(1, "\n", 1);
    }
    return 0;
}
