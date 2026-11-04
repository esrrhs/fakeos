/* fakeos userland - ls: list one directory (default: current directory).
 * Parses the kernel's fixed 48-byte dirent records (name at offset 19).
 * Optional -l uses fstatat(dirfd, name) for mode/size (daily-driver). */
package user;

typedef unsigned long u64;
typedef long i64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getdents(long fd, void *buf, unsigned long len);
extern long u_newfstatat(long dfd, const char *path, void *statbuf, long flags);
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
    AT_SYMLINK_NOFOLLOW = 0x100
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
        printf("ls: %s: no such directory\n", path);
        return 1;
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
                char mstr[12];
                unsigned int mode;
                unsigned long size;
                if (u_newfstatat(fd, name, stbuf, AT_SYMLINK_NOFOLLOW) != 0) {
                    printf("?????????? 0 %s\n", name);
                    continue;
                }
                mode = load_u32(stbuf + ST_MODE);
                size = load_u64(stbuf + ST_SIZE);
                mode_string(mode, mstr);
                if ((mode & S_IFMT) == S_IFDIR) {
                    printf("%s %u %s/\n", mstr, (unsigned int)size, name);
                } else {
                    printf("%s %u %s\n", mstr, (unsigned int)size, name);
                }
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
