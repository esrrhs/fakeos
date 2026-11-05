/* fakeos userland - mktemp: create unique temp file/dir (thin POSIX subset).
 * Forms: mktemp [-dut] [-p DIR|-pDIR] [TEMPLATE|PREFIX]
 * TEMPLATE must end in XXXXXX (replaced). Default /tmp/tmp.XXXXXX.
 * -d make directory; -u dry-run (print name, do not keep created object).
 * -p DIR: use DIR as the temp directory (GNU --tmpdir): default template
 * becomes DIR/tmp.XXXXXX; a relative TEMPLATE is joined under DIR; absolute
 * TEMPLATE is an error with -p.
 * -t: treat operand as PREFIX; create DIR/PREFIX.XXXXXX where DIR is -p DIR
 * if set, else $TMPDIR from /proc/self/environ, else /tmp. No operand →
 * DIR/tmp.XXXXXX. PREFIX must not contain '/'. Glued -dtu/-tpDIR keep argv
 * under EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
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
    MAX_TRIES = 64,
    ENV_CAP = 512,
    TMPDIR_CAP = 96
};

static const char alphabet[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static char tmpdir_env[TMPDIR_CAP];

static void usage(void) {
    u_write(2, "usage: mktemp [-dut] [-p DIR] [TEMPLATE|PREFIX]\n", 48);
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

static int has_slash(const char *s) {
    int i;
    for (i = 0; s[i]; i++) {
        if (s[i] == '/') return 1;
    }
    return 0;
}

/* Load TMPDIR from /proc/self/environ into tmpdir_env. Returns 1 if set. */
static int load_tmpdir_env(void) {
    char env[ENV_CAP];
    long fd;
    long n;
    long i;
    long start;
    tmpdir_env[0] = 0;
    fd = u_open("/proc/self/environ", 0, 0);
    if (fd < 0) return 0;
    n = u_read(fd, env, sizeof(env) - 1);
    u_close(fd);
    if (n < 0) n = 0;
    env[n] = 0;
    start = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || env[i] == 0) {
            if (i - start >= 7 &&
                env[start] == 'T' && env[start + 1] == 'M' &&
                env[start + 2] == 'P' && env[start + 3] == 'D' &&
                env[start + 4] == 'I' && env[start + 5] == 'R' &&
                env[start + 6] == '=') {
                long j;
                long k = 0;
                for (j = start + 7; j < i && k < TMPDIR_CAP - 1; j++) {
                    tmpdir_env[k++] = env[j];
                }
                tmpdir_env[k] = 0;
                return k > 0;
            }
            start = i + 1;
        }
    }
    return 0;
}

/* Build DIR/PREFIX.XXXXXX into out. */
static int join_prefix_xxxxxx(char *out, unsigned long cap,
                              const char *dir, const char *prefix) {
    char rel[PATH_MAX];
    unsigned long pl = strlen(prefix);
    unsigned long i;
    if (pl + 8 >= PATH_MAX) return -1; /* PREFIX + ".XXXXXX" + NUL */
    for (i = 0; i < pl; i++) rel[i] = prefix[i];
    rel[pl] = '.';
    rel[pl + 1] = 'X';
    rel[pl + 2] = 'X';
    rel[pl + 3] = 'X';
    rel[pl + 4] = 'X';
    rel[pl + 5] = 'X';
    rel[pl + 6] = 'X';
    rel[pl + 7] = 0;
    return join_tmpdir(out, cap, dir, rel);
}

int main(int argc, char **argv) {
    int i = 1;
    int do_dir = 0;
    int dry = 0;
    int opt_t = 0;
    const char *tmpdir = 0;
    const char *tmpl_arg = 0;
    char tmpl_buf[PATH_MAX];
    const char *tmpl;
    char path[PATH_MAX];
    unsigned long n;
    unsigned long seed;
    int try;
    long pid;
    const char *base_dir;

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
        /* allow glued -du / -dut / -tpDIR / -pDIR */
        for (j = 1; a[j]; j++) {
            if (a[j] == 'd') {
                do_dir = 1;
            } else if (a[j] == 'u') {
                dry = 1;
            } else if (a[j] == 't') {
                opt_t = 1;
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

    if (opt_t) {
        /* -t: DIR/PREFIX.XXXXXX; DIR from -p, else $TMPDIR, else /tmp. */
        if (tmpdir != 0) {
            base_dir = tmpdir;
        } else if (load_tmpdir_env()) {
            base_dir = tmpdir_env;
        } else {
            base_dir = "/tmp";
        }
        if (tmpl_arg == 0) {
            if (join_tmpdir(tmpl_buf, PATH_MAX, base_dir, "tmp.XXXXXX") < 0) {
                u_write(2, "mktemp: path too long\n", 22);
                return 1;
            }
        } else {
            if (has_slash(tmpl_arg)) {
                u_write(2, "mktemp: prefix must not contain '/'\n", 36);
                return 1;
            }
            if (join_prefix_xxxxxx(tmpl_buf, PATH_MAX, base_dir, tmpl_arg) < 0) {
                u_write(2, "mktemp: path too long\n", 22);
                return 1;
            }
        }
        tmpl = tmpl_buf;
    } else if (tmpdir != 0) {
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
