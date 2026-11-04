/* fakeos userland - install: copy files / create dirs (thin POSIX/GNU subset).
 * Forms:
 *   install [-m MODE|-mMODE] SOURCE DEST
 *   install [-m MODE|-mMODE] SOURCE... DIRECTORY
 *   install -d [-m MODE|-mMODE] DIR...
 * Default MODE is 0755. -d is mkdir -p style, then chmod. Copy path reuses
 * open/read/write + chmod. Glued -mMODE / -dmMODE keep argv under EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_mkdir(const char *path);
extern long u_chmod(const char *path, long mode);
extern long u_stat(const char *path, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200,
    ST_MODE = 24,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    PATH_CAP = 128,
    DEFAULT_MODE = 0x1ED
};

static unsigned int load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
}

static int is_dir(const char *path) {
    char st[ST_SIZE_BYTES];
    unsigned int mode;
    if (u_stat(path, st) != 0) return 0;
    mode = load_u32(st + ST_MODE);
    return (mode & S_IFMT) == S_IFDIR;
}

static int parse_mode(const char *s, unsigned int *out) {
    unsigned int mode = 0;
    int j;
    if (s == 0 || s[0] == 0) return 0;
    for (j = 0; s[j] != 0; j++) {
        if (s[j] < '0' || s[j] > '7') return 0;
        mode = (mode << 3) | (unsigned int)(s[j] - '0');
    }
    *out = mode;
    return 1;
}

static int mkdir_one(const char *path) {
    char buf[PATH_CAP];
    unsigned long n;
    unsigned long i;
    if (path == 0 || path[0] == 0) return -1;
    n = strlen(path);
    if (n >= PATH_CAP) return -1;
    strcpy(buf, path);
    i = 0;
    if (buf[0] == '/') i = 1;
    while (i <= n) {
        char saved;
        if (i < n && buf[i] != '/' && buf[i] != 0) {
            i++;
            continue;
        }
        saved = buf[i];
        buf[i] = 0;
        if (buf[0] != 0 && !(buf[0] == '/' && buf[1] == 0)) {
            if (!is_dir(buf)) {
                if (u_mkdir(buf) < 0) {
                    if (!is_dir(buf)) {
                        buf[i] = saved;
                        return -1;
                    }
                }
            }
        }
        buf[i] = saved;
        if (i >= n) break;
        while (i < n && buf[i] == '/') i++;
        if (i >= n) break;
    }
    return 0;
}

static const char *basename_of(const char *path) {
    const char *base = path;
    unsigned long i = 0;
    while (path[i] != 0) {
        if (path[i] == '/' && path[i + 1] != 0) {
            base = path + i + 1;
        }
        i++;
    }
    return base;
}

static int join_dir_base(char *out, unsigned long cap,
                         const char *dir, const char *base) {
    unsigned long dl = strlen(dir);
    unsigned long bl = strlen(base);
    unsigned long i;
    unsigned long o = 0;
    int need_slash;
    if (dl + bl + 2 > cap) return -1;
    for (i = 0; i < dl; i++) out[o++] = dir[i];
    need_slash = (dl == 0 || dir[dl - 1] != '/');
    if (need_slash) out[o++] = '/';
    for (i = 0; i < bl; i++) out[o++] = base[i];
    out[o] = 0;
    return 0;
}

static int copy_file(const char *src, const char *dst, unsigned int mode) {
    char buf[1024];
    long in;
    long out;
    long n;
    in = u_open(src, O_RDONLY, 0);
    if (in < 0) {
        printf("install: %s: cannot open\n", src);
        return -1;
    }
    out = u_open(dst, O_WRONLY | O_CREAT | O_TRUNC, (long)mode);
    if (out < 0) {
        printf("install: %s: cannot create\n", dst);
        u_close(in);
        return -1;
    }
    while ((n = u_read(in, buf, 1024)) > 0) {
        if (u_write(out, buf, (unsigned long)n) != n) {
            printf("install: %s: write failed\n", dst);
            u_close(in);
            u_close(out);
            return -1;
        }
    }
    u_close(in);
    u_close(out);
    if (u_chmod(dst, (long)mode) != 0) {
        printf("install: %s: chmod failed\n", dst);
        return -1;
    }
    return 0;
}

static void usage(void) {
    u_write(2, "usage: install [-d] [-m MODE] SRC DST|DIR...\n", 46);
}

int main(int argc, char **argv) {
    int dirs = 0;
    unsigned int mode = DEFAULT_MODE;
    int ai = 1;
    int rc = 0;
    int nops;
    int i;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) {
            ai++;
            break;
        }
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'd') {
                dirs = 1;
            } else if (o[j] == 'm') {
                const char *ms;
                if (o[j + 1] != 0) {
                    ms = o + j + 1;
                    j = (int)strlen(o) - 1;
                } else {
                    ai++;
                    if (ai >= argc) {
                        usage();
                        return 1;
                    }
                    ms = argv[ai];
                }
                if (!parse_mode(ms, &mode)) {
                    u_write(2, "install: bad mode\n", 18);
                    return 1;
                }
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    nops = argc - ai;
    if (nops <= 0) {
        usage();
        return 1;
    }

    if (dirs) {
        for (i = ai; i < argc; i++) {
            if (mkdir_one(argv[i]) < 0) {
                printf("install: %s: cannot create directory\n", argv[i]);
                rc = 1;
                continue;
            }
            if (u_chmod(argv[i], (long)mode) != 0) {
                printf("install: %s: chmod failed\n", argv[i]);
                rc = 1;
            }
        }
        return rc;
    }

    if (nops < 2) {
        usage();
        return 1;
    }

    if (nops == 2) {
        const char *src = argv[ai];
        const char *dst = argv[ai + 1];
        char joined[PATH_CAP];
        if (is_dir(dst)) {
            if (join_dir_base(joined, PATH_CAP, dst, basename_of(src)) < 0) {
                u_write(2, "install: path too long\n", 23);
                return 1;
            }
            dst = joined;
        }
        if (copy_file(src, dst, mode) < 0) rc = 1;
        return rc;
    }

    /* Multiple sources into directory. */
    {
        const char *dir = argv[argc - 1];
        if (!is_dir(dir)) {
            printf("install: %s: not a directory\n", dir);
            return 1;
        }
        for (i = ai; i < argc - 1; i++) {
            char joined[PATH_CAP];
            if (join_dir_base(joined, PATH_CAP, dir, basename_of(argv[i])) < 0) {
                u_write(2, "install: path too long\n", 23);
                rc = 1;
                continue;
            }
            if (copy_file(argv[i], joined, mode) < 0) rc = 1;
        }
    }
    return rc;
}
