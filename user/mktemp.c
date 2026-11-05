/* fakeos userland - mktemp: create unique temp file/dir (thin POSIX subset).
 * Forms: mktemp [-du] [-p DIR|-pDIR] [TEMPLATE]
 * TEMPLATE must end in XXXXXX (replaced). Default /tmp/tmp.XXXXXX.
 * -d make directory; -u dry-run (print name, do not keep created object).
 * -p DIR: use DIR as the temp directory (GNU --tmpdir): default template
 * becomes DIR/tmp.XXXXXX; a relative TEMPLATE is joined under DIR; absolute
 * TEMPLATE is an error with -p. Glued -dpu/-pDIR keep argv under
 * EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_unlink(const char *path);
extern long u_mkdir(const char *path);
extern long u_rmdir(const char *path);
extern long u_getpid(void);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);

enum {
    O_RDWR  = 2,
    O_CREAT = 0x40,
    O_EXCL  = 0x080,
    MODE_0600 = 0x180,
    PATH_MAX = 128,
    MAX_TRIES = 64
};

static const char alphabet[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static void usage(void) {
    u_write(2, "usage: mktemp [-du] [-p DIR] [TEMPLATE]\n", 40);
}

static int ends_with_xxxxxx(const char *s, unsigned long n) {
    if (n < 6) return 0;
    return s[n - 6] == 'X' && s[n - 5] == 'X' && s[n - 4] == 'X'
        && s[n - 3] == 'X' && s[n - 2] == 'X' && s[n - 1] == 'X';
}

static void fill_xxxxxx(char *path, unsigned long n, unsigned long seed) {
    int i;
    unsigned long x = seed;
    for (i = 0; i < 6; i++) {
        x = x * 1103515245u + 12345u;
        path[n - 6 + i] = alphabet[(x >> 16) % 62];
    }
}

/* Join DIR + relative REL into out. Returns 0 on success. */
static int join_tmpdir(char *out, unsigned long cap,
                       const char *dir, const char *rel) {
    unsigned long dl = strlen(dir);
    unsigned long rl = strlen(rel);
    unsigned long i;
    unsigned long o = 0;
    int need_slash;
    if (dl + rl + 2 > cap) return -1;
    for (i = 0; i < dl; i++) out[o++] = dir[i];
    need_slash = (dl == 0 || dir[dl - 1] != '/');
    if (need_slash) out[o++] = '/';
    for (i = 0; i < rl; i++) out[o++] = rel[i];
    out[o] = 0;
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int do_dir = 0;
    int dry = 0;
    const char *tmpdir = 0;
    const char *tmpl_arg = 0;
    char tmpl_buf[PATH_MAX];
    const char *tmpl;
    char path[PATH_MAX];
    unsigned long n;
    unsigned long seed;
    int try;
    long pid;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        int j;
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[1] == '-' && a[2] == 0) {
            i++;
            break;
        }
        /* allow glued -du / -udp / -pDIR */
        for (j = 1; a[j]; j++) {
            if (a[j] == 'd') {
                do_dir = 1;
            } else if (a[j] == 'u') {
                dry = 1;
            } else if (a[j] == 'p') {
                if (a[j + 1] != 0) {
                    tmpdir = a + j + 1;
                    j = (int)strlen(a) - 1;
                } else {
                    i++;
                    if (i >= argc) {
                        usage();
                        return 1;
                    }
                    tmpdir = argv[i];
                }
            } else {
                usage();
                return 1;
            }
        }
        i++;
    }
    if (i < argc) {
        tmpl_arg = argv[i];
        i++;
    }
    if (i != argc) {
        usage();
        return 1;
    }

    if (tmpdir != 0) {
        if (tmpl_arg == 0) {
            if (join_tmpdir(tmpl_buf, PATH_MAX, tmpdir, "tmp.XXXXXX") < 0) {
                u_write(2, "mktemp: path too long\n", 22);
                return 1;
            }
            tmpl = tmpl_buf;
        } else if (tmpl_arg[0] == '/') {
            u_write(2, "mktemp: template must be relative with -p\n", 42);
            return 1;
        } else {
            if (join_tmpdir(tmpl_buf, PATH_MAX, tmpdir, tmpl_arg) < 0) {
                u_write(2, "mktemp: path too long\n", 22);
                return 1;
            }
            tmpl = tmpl_buf;
        }
    } else if (tmpl_arg != 0) {
        tmpl = tmpl_arg;
    } else {
        tmpl = "/tmp/tmp.XXXXXX";
    }

    n = strlen(tmpl);
    if (n >= PATH_MAX || !ends_with_xxxxxx(tmpl, n)) {
        u_write(2, "mktemp: invalid template\n", 25);
        return 1;
    }
    strcpy(path, tmpl);

    pid = u_getpid();
    seed = (unsigned long)pid * 2654435761u + 7u;

    for (try = 0; try < MAX_TRIES; try++) {
        fill_xxxxxx(path, n, seed + (unsigned long)try * 97u);
        if (do_dir) {
            if (u_mkdir(path) == 0) {
                if (dry) {
                    u_rmdir(path);
                }
                u_write(1, path, strlen(path));
                u_write(1, "\n", 1);
                return 0;
            }
        } else {
            long fd = u_open(path, O_RDWR | O_CREAT | O_EXCL, MODE_0600);
            if (fd >= 0) {
                u_close(fd);
                if (dry) {
                    u_unlink(path);
                }
                u_write(1, path, strlen(path));
                u_write(1, "\n", 1);
                return 0;
            }
        }
    }
    u_write(2, "mktemp: failed\n", 15);
    return 1;
}
