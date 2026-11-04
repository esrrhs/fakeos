/* fakeos userland - ls: list one directory (default: current directory).
 * Parses the kernel's fixed 48-byte dirent records (name at offset 19).
 * Optional -l uses fstatat(dirfd, name) for mode/size; symlinks show
 * `name -> target` via readlink. File operands print one entry.
 * chmod/umask: modes come from live inode permission bits. */
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
    ST_MODE = 24,
    ST_SIZE = 48,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    S_IFCHR = 0x2000,
    S_IFIFO = 0x1000,
    S_IFSOCK = 0xC000,
    S_IFLNK = 0xA000,
    AT_SYMLINK_NOFOLLOW = 0x100,
    AT_EMPTY_PATH = 0x1000
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

static void print_name_long(const char *dirpath, const char *name,
                            const char *stbuf) {
    char mstr[12];
    unsigned int mode;
    unsigned long size;
    mode = load_u32(stbuf + ST_MODE);
    size = load_u64(stbuf + ST_SIZE);
    mode_string(mode, mstr);
    if ((mode & S_IFMT) == S_IFLNK) {
        char full[96];
        char tgt[96];
        long rn;
        join_dir_name(full, sizeof(full), dirpath, name);
        rn = u_readlink(full, tgt, sizeof(tgt) - 1);
        if (rn >= 0) {
            tgt[rn] = 0;
            printf("%s %u %s -> %s\n", mstr, (unsigned int)size, name, tgt);
        } else {
            printf("%s %u %s -> ?\n", mstr, (unsigned int)size, name);
        }
    } else if ((mode & S_IFMT) == S_IFDIR) {
        printf("%s %u %s/\n", mstr, (unsigned int)size, name);
    } else {
        printf("%s %u %s\n", mstr, (unsigned int)size, name);
    }
}

int main(int argc, char **argv) {
    const char *path = ".";
    int longfmt = 0;
    int ai;
    char buf[1920];
    char stbuf[ST_SIZE_BYTES];
    long fd;
    long n;

    for (ai = 1; ai < argc; ai++) {
        if (argv[ai][0] == '-' && argv[ai][1] == 'l' && argv[ai][2] == 0) {
            longfmt = 1;
        } else {
            path = argv[ai];
        }
    }

    fd = u_open(path, 0, 0);
    if (fd < 0) {
        printf("ls: %s: no such file or directory\n", path);
        return 1;
    }
    if (u_newfstatat(fd, "", stbuf, AT_EMPTY_PATH) == 0) {
        unsigned int mode = load_u32(stbuf + ST_MODE);
        if ((mode & S_IFMT) != S_IFDIR) {
            const char *base = basename_of(path);
            if (longfmt) {
                print_name_long(".", base, stbuf);
            } else {
                printf("%s\n", base);
            }
            u_close(fd);
            return 0;
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
            unsigned long k = 0;
            while (k < 29 && buf[off + 19 + k] != 0) {
                name[k] = buf[off + 19 + k];
                k++;
            }
            name[k] = 0;
            if (longfmt) {
                if (u_newfstatat(fd, name, stbuf, AT_SYMLINK_NOFOLLOW) != 0) {
                    printf("?????????? 0 %s\n", name);
                    continue;
                }
                print_name_long(path, name, stbuf);
            } else {
                if (buf[off + 18] == 4) {
                    name[k++] = '/';
                    name[k] = 0;
                }
                u_write(1, name, k);
                u_write(1, "\n", 1);
            }
        }
    }
    u_close(fd);
    return 0;
}
