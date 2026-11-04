/* fakeos userland - cp: copy files; -r/-R trees; -n noclobber.
 * Forms:
 *   cp [-r|-R] [-n] SRC DST
 *   cp [-r|-R] [-n] SRC... DIRECTORY
 * If DST/DIRECTORY exists as a directory, place each SRC under it as
 * DIRECTORY/basename(SRC). Refuse directory SRC without -r. Refuse cycles
 * (DST under SRC). -n skips existing destinations (quiet, exit 0). Sticky
 * clusters (-rn/-nr). Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_stat(const char *path, void *statbuf);
extern long u_mkdir(const char *path);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
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
    DENT_BUF = 1920,
    DENT_REC = 48,
    NAME_OFF = 19,
    NAME_LEN = 30,
    MAX_NAMES = 64
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

static int path_exists(const char *path) {
    char st[ST_SIZE_BYTES];
    return u_stat(path, st) == 0;
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

/* True if path is ancestor or equal to child (plain string prefix + '/'). */
static int is_under(const char *ancestor, const char *child) {
    unsigned long al = strlen(ancestor);
    unsigned long i;
    if (al == 0) return 0;
    if (strcmp(ancestor, child) == 0) return 1;
    for (i = 0; i < al; i++) {
        if (child[i] != ancestor[i]) return 0;
    }
    /* ancestor without trailing slash: child must continue with '/' */
    if (ancestor[al - 1] == '/') return 1;
    return child[al] == '/';
}

static int copy_file(const char *src, const char *dst, int noclobber) {
    char buf[1024];
    long in;
    long out;
    long n;

    if (noclobber && path_exists(dst)) {
        return 0;
    }
    in = u_open(src, O_RDONLY, 0);
    if (in < 0) {
        printf("cp: %s: cannot open\n", src);
        return 1;
    }
    out = u_open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0x1A4);
    if (out < 0) {
        printf("cp: %s: cannot create\n", dst);
        u_close(in);
        return 1;
    }
    while ((n = u_read(in, buf, 1024)) > 0) {
        if (u_write(out, buf, (u64)n) != n) {
            printf("cp: write failed\n");
            u_close(in);
            u_close(out);
            return 1;
        }
    }
    u_close(in);
    u_close(out);
    return 0;
}

static int copy_path(const char *src, const char *dst, int recursive, int noclobber);

static int copy_dir_rec(const char *src, const char *dst, int noclobber) {
    char dent[DENT_BUF];
    char child_src[PATH_CAP];
    char child_dst[PATH_CAP];
    char names[MAX_NAMES][NAME_LEN];
    int nnames;
    long fd;
    long n;
    int i;
    int rc = 0;

    if (is_under(src, dst)) {
        printf("cp: %s: cannot copy a directory into itself\n", src);
        return 1;
    }

    if (!path_exists(dst)) {
        if (u_mkdir(dst) < 0) {
            printf("cp: %s: cannot create directory\n", dst);
            return 1;
        }
    } else if (!is_dir(dst)) {
        printf("cp: %s: not a directory\n", dst);
        return 1;
    }

    fd = u_open(src, O_RDONLY, 0);
    if (fd < 0) {
        printf("cp: %s: cannot open directory\n", src);
        return 1;
    }
    nnames = 0;
    for (;;) {
        long off;
        n = u_getdents(fd, dent, sizeof(dent));
        if (n <= 0) break;
        for (off = 0; off + DENT_REC <= n; off += DENT_REC) {
            char name[NAME_LEN];
            unsigned long k = 0;
            while (k + 1 < NAME_LEN && dent[off + NAME_OFF + k] != 0) {
                name[k] = dent[off + NAME_OFF + k];
                k++;
            }
            name[k] = 0;
            if (name[0] == 0) continue;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            if (nnames >= MAX_NAMES) {
                printf("cp: %s: too many entries\n", src);
                u_close(fd);
                return 1;
            }
            strcpy(names[nnames], name);
            nnames++;
        }
    }
    u_close(fd);

    for (i = 0; i < nnames; i++) {
        if (join_dir_base(child_src, PATH_CAP, src, names[i]) < 0
            || join_dir_base(child_dst, PATH_CAP, dst, names[i]) < 0) {
            u_write(2, "cp: path too long\n", 18);
            rc = 1;
            continue;
        }
        if (copy_path(child_src, child_dst, 1, noclobber) != 0) rc = 1;
    }
    return rc;
}

static int copy_path(const char *src, const char *dst, int recursive, int noclobber) {
    if (is_dir(src)) {
        if (!recursive) {
            printf("cp: %s: is a directory (not copied)\n", src);
            return 1;
        }
        return copy_dir_rec(src, dst, noclobber);
    }
    return copy_file(src, dst, noclobber);
}

static void usage(void) {
    u_write(2, "usage: cp [-rnR] SRC DST|DIR...\n", 32);
}

int main(int argc, char **argv) {
    int i;
    int ai = 1;
    int recursive = 0;
    int noclobber = 0;
    int rc = 0;
    int nops;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) {
            /* lone "-" treat as operand */
            break;
        }
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'r' || o[j] == 'R') {
                recursive = 1;
            } else if (o[j] == 'n') {
                noclobber = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    nops = argc - ai;
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
                u_write(2, "cp: path too long\n", 18);
                return 1;
            }
            dst = joined;
        }
        return copy_path(src, dst, recursive, noclobber);
    }

    /* Multiple sources into directory. */
    {
        const char *dir = argv[argc - 1];
        if (!is_dir(dir)) {
            printf("cp: %s: not a directory\n", dir);
            return 1;
        }
        for (i = ai; i < argc - 1; i++) {
            char joined[PATH_CAP];
            if (join_dir_base(joined, PATH_CAP, dir, basename_of(argv[i])) < 0) {
                u_write(2, "cp: path too long\n", 18);
                rc = 1;
                continue;
            }
            if (copy_path(argv[i], joined, recursive, noclobber) != 0) rc = 1;
        }
    }
    return rc;
}
