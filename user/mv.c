/* fakeos userland - mv: rename(2); falls back to copy-tree+rm across FS.
 * Forms:
 *   mv SRC DST
 *   mv SRC... DIRECTORY
 * If DST/DIRECTORY exists as a directory, place each SRC under it as
 * DIRECTORY/basename(SRC). Directories move via rename when possible;
 * otherwise copy-tree then recursive remove. Refuse cycles (DST under
 * SRC). Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_unlink(const char *path);
extern long u_rename(const char *oldpath, const char *newpath);
extern long u_stat(const char *path, void *statbuf);
extern long u_mkdir(const char *path);
extern long u_rmdir(const char *path);
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
    if (ancestor[al - 1] == '/') return 1;
    return child[al] == '/';
}

static int copy_file(const char *src, const char *dst) {
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
    return 0;
}

static int copy_path(const char *src, const char *dst);
static int rm_path(const char *path);

static int copy_dir_rec(const char *src, const char *dst) {
    char dent[DENT_BUF];
    char child_src[PATH_CAP];
    char child_dst[PATH_CAP];
    char names[MAX_NAMES][NAME_LEN];
    int nnames;
    long fd;
    long n;
    int i;
    int rc = 0;

    if (!path_exists(dst)) {
        if (u_mkdir(dst) < 0) {
            printf("mv: %s: cannot create directory\n", dst);
            return 1;
        }
    } else if (!is_dir(dst)) {
        printf("mv: %s: not a directory\n", dst);
        return 1;
    }

    fd = u_open(src, O_RDONLY, 0);
    if (fd < 0) {
        printf("mv: %s: cannot open directory\n", src);
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
                printf("mv: %s: too many entries\n", src);
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
            u_write(2, "mv: path too long\n", 18);
            rc = 1;
            continue;
        }
        if (copy_path(child_src, child_dst) != 0) rc = 1;
    }
    return rc;
}

static int copy_path(const char *src, const char *dst) {
    if (is_dir(src)) {
        return copy_dir_rec(src, dst);
    }
    return copy_file(src, dst);
}

static int rm_dir_rec(const char *path) {
    char dent[DENT_BUF];
    char child[PATH_CAP];
    char names[MAX_NAMES][NAME_LEN];
    int nnames;
    long fd;
    long n;
    int i;
    int rc = 0;

    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("mv: %s: cannot open directory\n", path);
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
                printf("mv: %s: too many entries\n", path);
                u_close(fd);
                return 1;
            }
            strcpy(names[nnames], name);
            nnames++;
        }
    }
    u_close(fd);

    for (i = 0; i < nnames; i++) {
        if (join_dir_base(child, PATH_CAP, path, names[i]) < 0) {
            u_write(2, "mv: path too long\n", 18);
            rc = 1;
            continue;
        }
        if (rm_path(child) != 0) rc = 1;
    }
    if (u_rmdir(path) != 0) {
        printf("mv: %s: cannot remove directory\n", path);
        rc = 1;
    }
    return rc;
}

static int rm_path(const char *path) {
    if (is_dir(path)) {
        return rm_dir_rec(path);
    }
    if (u_unlink(path) != 0) {
        printf("mv: %s: cannot remove\n", path);
        return 1;
    }
    return 0;
}

static int copy_unlink(const char *src, const char *dst) {
    if (copy_file(src, dst) != 0) return 1;
    if (u_unlink(src) < 0) {
        printf("mv: %s: copied but unlink failed\n", src);
        return 1;
    }
    return 0;
}

static int move_one(const char *src, const char *dst) {
    if (is_under(src, dst)) {
        printf("mv: %s: cannot move a directory into itself\n", src);
        return 1;
    }
    if (u_rename(src, dst) == 0) {
        return 0;
    }
    /* EXDEV / unsupported: copy-tree then remove source. */
    if (is_dir(src)) {
        if (copy_path(src, dst) != 0) return 1;
        return rm_path(src);
    }
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
