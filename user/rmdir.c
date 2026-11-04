/* fakeos userland - rmdir: remove empty directories (thin shell/POSIX mirror).
 * Forms: rmdir [-p] DIR...
 * -p also removes empty parent components (mkdir -p inverse).
 * External form for xargs/scripts that exec rmdir without the shell builtin.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_rmdir(const char *path);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);

enum { PATH_CAP = 128 };

/* Strip one trailing path component in-place. Returns 0 if a parent remains
 * to try, -1 when nothing left (empty, "/", or bare name with no slash). */
static int strip_parent(char *path) {
    unsigned long n;
    unsigned long i;
    if (path == 0 || path[0] == 0) return -1;
    n = strlen(path);
    while (n > 1 && path[n - 1] == '/') {
        path[n - 1] = 0;
        n--;
    }
    if (n == 0 || (n == 1 && path[0] == '/')) return -1;
    i = n;
    while (i > 0 && path[i - 1] != '/') i--;
    if (i == 0) {
        /* bare component: no parent */
        path[0] = 0;
        return -1;
    }
    if (i == 1 && path[0] == '/') {
        path[1] = 0; /* parent is "/" — stop before removing root */
        return -1;
    }
    path[i - 1] = 0;
    return 0;
}

static int rmdir_parents(const char *path) {
    char buf[PATH_CAP];
    unsigned long n;
    if (path == 0 || path[0] == 0) return -1;
    n = strlen(path);
    if (n >= PATH_CAP) return -1;
    strcpy(buf, path);
    for (;;) {
        if (u_rmdir(buf) < 0) {
            return -1;
        }
        if (strip_parent(buf) < 0) {
            return 0;
        }
        if (buf[0] == 0) return 0;
    }
}

static void usage(void) {
    u_write(2, "usage: rmdir [-p] DIR...\n", 24);
}

int main(int argc, char **argv) {
    int i;
    int rc = 0;
    int parents = 0;
    int ai = 1;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) {
            ai++;
            break;
        }
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'p') {
                parents = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    if (ai >= argc) {
        usage();
        return 1;
    }
    for (i = ai; i < argc; i++) {
        if (parents) {
            if (rmdir_parents(argv[i]) < 0) {
                printf("rmdir: %s: cannot remove directory\n", argv[i]);
                rc = 1;
            }
        } else if (u_rmdir(argv[i]) < 0) {
            printf("rmdir: %s: cannot remove directory\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
