/* fakeos userland - mv: rename(2); falls back to copy+unlink across FS.
 * Forms:
 *   mv SRC DST
 *   mv SRC... DIRECTORY
 * If DST/DIRECTORY exists as a directory, place each SRC under it as
 * DIRECTORY/basename(SRC). Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_unlink(const char *path);
extern long u_rename(const char *oldpath, const char *newpath);
extern long u_stat(const char *path, void *statbuf);
extern unsigned long strlen(const char *s);
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
    PATH_CAP = 128
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

static int copy_unlink(const char *src, const char *dst) {
    char buf[1024];
    long in;
    long out;
    long n;

    in = u_open(src, O_RDONLY, 0);
    if (in < 0) {
        printf("mv: %s: cannot open\n", src);
        return 1;
    }
    out = u_open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0x1A4);
    if (out < 0) {
        printf("mv: %s: cannot create\n", dst);
        u_close(in);
        return 1;
    }
    while ((n = u_read(in, buf, 1024)) > 0) {
        if (u_write(out, buf, (unsigned long)n) != n) {
            printf("mv: write error\n");
            u_close(in);
            u_close(out);
            return 1;
        }
    }
    u_close(in);
    u_close(out);
    if (u_unlink(src) < 0) {
        printf("mv: %s: copied but unlink failed\n", src);
        return 1;
    }
    return 0;
}

static int move_one(const char *src, const char *dst) {
    if (u_rename(src, dst) == 0) {
        return 0;
    }
    /* EXDEV / unsupported: preserve prior copy+unlink behavior. */
    return copy_unlink(src, dst);
}

static void usage(void) {
    u_write(2, "usage: mv SRC DST|DIR...\n", 24);
}

int main(int argc, char **argv) {
    int nops;
    int i;
    int rc = 0;

    if (argc < 3) {
        usage();
        return 1;
    }
    nops = argc - 1;

    if (nops == 2) {
        const char *src = argv[1];
        const char *dst = argv[2];
        char joined[PATH_CAP];
        if (is_dir(dst)) {
            if (join_dir_base(joined, PATH_CAP, dst, basename_of(src)) < 0) {
                u_write(2, "mv: path too long\n", 18);
                return 1;
            }
            dst = joined;
        }
        return move_one(src, dst);
    }

    /* Multiple sources into directory. */
    {
        const char *dir = argv[argc - 1];
        if (!is_dir(dir)) {
            printf("mv: %s: not a directory\n", dir);
            return 1;
        }
        for (i = 1; i < argc - 1; i++) {
            char joined[PATH_CAP];
            if (join_dir_base(joined, PATH_CAP, dir, basename_of(argv[i])) < 0) {
                u_write(2, "mv: path too long\n", 18);
                rc = 1;
                continue;
            }
            if (move_one(argv[i], joined) != 0) rc = 1;
        }
    }
    return rc;
}
