/* fakeos userland - ls: list directory entries or file operands.
 * Forms: ls [-a|-A] [-d] [-l] [-1] [-F] [PATH...]
 * Default: hide names starting with '.' (POSIX); one name per line. -a shows
 * all including . and ..; -A almost all (hidden but not ./..). -d lists a
 * directory itself. -l uses fstatat for mode/nlink/uid/gid/size/mtime;
 * symlinks show `name -> target` via readlink. -1 forces one-per-line (default
 * already; ignored with -l). -F classifies short names (slash, star, at, pipe, equals).
 * Glued flags (-laF, -ald, ...) accepted. Multi-PATH: dirs get a `path:`
 * header when more than one PATH is given. Compact argv for EXEC_MAX_ARGC=8.
 * Ownership and mtime come from live inode fields (chown + write stamps). */
package user;

typedef unsigned long u64;
typedef long i64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern long u_newfstatat(long dfd, const char *path, void *statbuf, long flags);
extern long u_readlink(const char *path, char *buf, unsigned long n);
extern void printf(const char *fmt, ...);

/* Linux x86-64 struct stat field offsets (144 bytes). */
enum {
    ST_NLINK = 16,
    ST_MODE = 24,
    ST_UID = 28,
    ST_GID = 32,
    ST_SIZE = 48,
    ST_MTIME = 88,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    S_IFCHR = 0x2000,
    S_IFBLK = 0x6000,
    S_IFIFO = 0x1000,
    S_IFSOCK = 0xC000,
    S_IFLNK = 0xA000,
    S_IFREG = 0x8000,
    S_IXUSR = 0x40,
    S_IXGRP = 0x8,
    S_IXOTH = 0x1,
    AT_SYMLINK_NOFOLLOW = 0x100,
    AT_EMPTY_PATH = 0x1000,
    MAX_PATHS = 8
};

static unsigned int load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
}

static unsigned long load_u64(const char *p) {
    unsigned long v = 0;
    unsigned long i;
    for (i = 0; i < 8; i++) {
        v |= ((unsigned long)(unsigned char)p[i]) << (8 * i);
    }
    return v;
}

static void join_dir_name(char *out, unsigned long cap, const char *dir,
                          const char *name) {
    unsigned long i = 0;
    unsigned long j = 0;
    if (cap < 2) {
        out[0] = 0;
        return;
    }
    if (dir[0] == '.' && dir[1] == 0) {
        while (name[j] != 0 && i + 1 < cap) {
            out[i++] = name[j++];
        }
        out[i] = 0;
        return;
    }
    while (dir[i] != 0 && i + 1 < cap) {
        out[i] = dir[i];
        i++;
    }
    if (i > 0 && out[i - 1] != '/' && i + 1 < cap) {
        out[i++] = '/';
    }
    while (name[j] != 0 && i + 1 < cap) {
        out[i++] = name[j++];
    }
    out[i] = 0;
}

static char mode_type_char(unsigned int mode) {
    unsigned int t = mode & S_IFMT;
    if (t == S_IFDIR) {
        return 'd';
    }
    if (t == S_IFCHR) {
        return 'c';
    }
    if (t == S_IFBLK) {
        return 'b';
    }
    if (t == S_IFIFO) {
        return 'p';
    }
    if (t == S_IFSOCK) {
        return 's';
    }
    if (t == S_IFLNK) {
        return 'l';
    }
    return '-';
}

static void mode_string(unsigned int mode, char *out) {
    unsigned int bit;
    unsigned long i;
    out[0] = mode_type_char(mode);
    bit = 0x100;
    for (i = 0; i < 9; i++) {
        char c = "rwxrwxrwx"[i];
        if (mode & bit) {
            out[1 + i] = c;
        } else {
            out[1 + i] = '-';
        }
        bit >>= 1;
    }
    out[10] = 0;
}

static const char *basename_of(const char *path) {
    const char *base = path;
    unsigned long i = 0;
    while (path[i] != 0) {
        if (path[i] == '/') {
            base = path + i + 1;
        }
        i++;
    }
    if (base[0] == 0) {
        return path;
    }
    return base;
}

static int is_dot_or_dotdot(const char *name) {
    if (name[0] != '.') {
        return 0;
    }
    if (name[1] == 0) {
        return 1;
    }
    return name[1] == '.' && name[2] == 0;
}

static int show_name(const char *name, int show_all, int almost_all) {
    if (show_all) {
        return 1;
    }
    if (almost_all) {
        return !is_dot_or_dotdot(name);
    }
    return name[0] != '.';
}

/* linkpath: path passed to readlink for symlinks; name: display name. */
static void print_name_long(const char *linkpath, const char *name,
                            const char *stbuf) {
    char mstr[12];
    unsigned int mode;
    unsigned int nlink;
    unsigned int uid;
    unsigned int gid;
    unsigned long size;
    unsigned long mtime;
    mode = load_u32(stbuf + ST_MODE);
    nlink = (unsigned int)load_u64(stbuf + ST_NLINK);
    uid = load_u32(stbuf + ST_UID);
    gid = load_u32(stbuf + ST_GID);
    size = load_u64(stbuf + ST_SIZE);
    mtime = load_u64(stbuf + ST_MTIME);
    mode_string(mode, mstr);
    /* mode nlink uid gid size mtime name  (mtime = boot-relative seconds) */
    if ((mode & S_IFMT) == S_IFLNK) {
        char tgt[96];
        long rn;
        rn = u_readlink(linkpath, tgt, sizeof(tgt) - 1);
        if (rn >= 0) {
            tgt[rn] = 0;
            printf("%s %u %u %u %u %u %s -> %s\n", mstr, nlink, uid, gid,
                   (unsigned int)size, (unsigned int)mtime, name, tgt);
        } else {
            printf("%s %u %u %u %u %u %s -> ?\n", mstr, nlink, uid, gid,
                   (unsigned int)size, (unsigned int)mtime, name);
        }
    } else if ((mode & S_IFMT) == S_IFDIR) {
        printf("%s %u %u %u %u %u %s/\n", mstr, nlink, uid, gid,
               (unsigned int)size, (unsigned int)mtime, name);
    } else {
        printf("%s %u %u %u %u %u %s\n", mstr, nlink, uid, gid,
               (unsigned int)size, (unsigned int)mtime, name);
    }
}

/* Classify suffix for -F (0 = none). */
static char classify_char(unsigned int mode) {
    unsigned int t = mode & S_IFMT;
    if (t == S_IFDIR) return '/';
    if (t == S_IFLNK) return '@';
    if (t == S_IFIFO) return '|';
    if (t == S_IFSOCK) return '=';
    if (t == S_IFREG && (mode & (S_IXUSR | S_IXGRP | S_IXOTH))) return '*';
    return 0;
}

static void print_name_short(const char *name, char suffix) {
    unsigned long k = 0;
    while (name[k] != 0) {
        k++;
    }
    u_write(1, name, k);
    if (suffix != 0) {
        u_write(1, &suffix, 1);
    }
    u_write(1, "\n", 1);
}

static int list_one(const char *path, int longfmt, int dir_as_file,
                    int show_all, int almost_all, int classify, int print_header) {
    char buf[1920];
    char stbuf[ST_SIZE_BYTES];
    long fd;
    long n;
    unsigned int mode;

    fd = u_open(path, 0, 0);
    if (fd < 0) {
        printf("ls: %s: no such file or directory\n", path);
        return 1;
    }
    if (u_newfstatat(fd, "", stbuf, AT_EMPTY_PATH) != 0) {
        printf("ls: %s: stat failed\n", path);
        u_close(fd);
        return 1;
    }
    mode = load_u32(stbuf + ST_MODE);
    if (dir_as_file || (mode & S_IFMT) != S_IFDIR) {
        const char *base = basename_of(path);
        if (longfmt) {
            print_name_long(path, base, stbuf);
        } else {
            print_name_short(base, classify ? classify_char(mode) : 0);
        }
        u_close(fd);
        return 0;
    }

    if (print_header) {
        printf("%s:\n", path);
    }

    /* Ramfs/Ext2 getdents omit synthetic ./.. ; emit them for -a. */
    if (show_all) {
        const char *dots[2];
        int di;
        dots[0] = ".";
        dots[1] = "..";
        for (di = 0; di < 2; di++) {
            if (longfmt) {
                if (u_newfstatat(fd, dots[di], stbuf, AT_SYMLINK_NOFOLLOW) != 0) {
                    printf("?????????? 0 0 0 0 0 %s\n", dots[di]);
                } else {
                    char full[96];
                    join_dir_name(full, sizeof(full), path, dots[di]);
                    print_name_long(full, dots[di], stbuf);
                }
            } else {
                print_name_short(dots[di], classify ? '/' : 0);
            }
        }
    }

    for (;;) {
        long off;
        n = u_getdents(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        for (off = 0; off + 48 <= n; off += 48) {
            char name[32];
            char full[96];
            unsigned long k = 0;
            while (k < 29 && buf[off + 19 + k] != 0) {
                name[k] = buf[off + 19 + k];
                k++;
            }
            name[k] = 0;
            /* Skip kernel-provided ./.. if any; -a already printed them. */
            if (is_dot_or_dotdot(name)) {
                continue;
            }
            if (!show_name(name, show_all, almost_all)) {
                continue;
            }
            if (longfmt) {
                if (u_newfstatat(fd, name, stbuf, AT_SYMLINK_NOFOLLOW) != 0) {
                    printf("?????????? 0 0 0 0 0 %s\n", name);
                    continue;
                }
                join_dir_name(full, sizeof(full), path, name);
                print_name_long(full, name, stbuf);
            } else if (classify) {
                char suf = 0;
                if (u_newfstatat(fd, name, stbuf, AT_SYMLINK_NOFOLLOW) == 0) {
                    suf = classify_char(load_u32(stbuf + ST_MODE));
                } else if (buf[off + 18] == 4) {
                    suf = '/';
                }
                print_name_short(name, suf);
            } else {
                print_name_short(name, 0);
            }
        }
    }
    u_close(fd);
    return 0;
}

static void usage(void) {
    u_write(2, "usage: ls [-a|-A] [-d] [-l] [-1F] [PATH...]
", 44);
}

int main(int argc, char **argv) {
    int longfmt = 0;
    int dir_as_file = 0;
    int show_all = 0;
    int almost_all = 0;
    int classify = 0;
    const char *paths[MAX_PATHS];
    int npaths = 0;
    int ai;
    int i;
    int rc = 0;
    int headers;

    for (ai = 1; ai < argc; ai++) {
        const char *a = argv[ai];
        if (a[0] == '-' && a[1] != 0) {
            int j;
            /* bare "-" is a PATH (cwd); glued flags otherwise */
            if (a[1] == '-' && a[2] == 0) {
                ai++;
                break;
            }
            for (j = 1; a[j] != 0; j++) {
                if (a[j] == 'l') {
                    longfmt = 1;
                } else if (a[j] == 'a') {
                    show_all = 1;
                    almost_all = 0;
                } else if (a[j] == 'A') {
                    if (!show_all) {
                        almost_all = 1;
                    }
                } else if (a[j] == 'd') {
                    dir_as_file = 1;
                } else if (a[j] == '1') {
                    /* one-per-line (already default; ignored under -l) */
                } else if (a[j] == 'F') {
                    classify = 1;
                } else {
                    usage();
                    return 1;
                }
            }
        } else {
            if (npaths >= MAX_PATHS) {
                u_write(2, "ls: too many paths\n", 19);
                return 1;
            }
            paths[npaths++] = a;
        }
    }
    for (; ai < argc; ai++) {
        if (npaths >= MAX_PATHS) {
            u_write(2, "ls: too many paths\n", 19);
            return 1;
        }
        paths[npaths++] = argv[ai];
    }

    if (npaths == 0) {
        paths[0] = ".";
        npaths = 1;
    }

    headers = (npaths > 1) && !dir_as_file;
    for (i = 0; i < npaths; i++) {
        if (i > 0 && headers) {
            u_write(1, "\n", 1);
        }
        if (list_one(paths[i], longfmt, dir_as_file, show_all, almost_all,
                     classify, headers)) {
            rc = 1;
        }
    }
    return rc;
}
