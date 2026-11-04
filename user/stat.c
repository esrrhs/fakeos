/* fakeos userland - stat: print file status (thin).
 *   stat [-L] FILE...
 * Default uses lstat(2) (nofollow). -L follows the final symlink via stat(2).
 * Fields: mode/nlink/uid/gid/size/mtime (boot-relative seconds).
 * External form for xargs/scripts.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_stat(const char *path, void *statbuf);
extern long u_lstat(const char *path, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum {
    ST_INO = 8,
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
    S_IFIFO = 0x1000,
    S_IFSOCK = 0xC000,
    S_IFLNK = 0xA000,
    S_IFREG = 0x8000
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

static char type_char(unsigned int mode) {
    unsigned int t = mode & S_IFMT;
    if (t == S_IFDIR) return 'd';
    if (t == S_IFCHR) return 'c';
    if (t == S_IFIFO) return 'p';
    if (t == S_IFSOCK) return 's';
    if (t == S_IFLNK) return 'l';
    if (t == S_IFREG) return '-';
    return '?';
}

static void print_one(const char *path, const char *st) {
    unsigned int mode = load_u32(st + ST_MODE);
    unsigned int uid = load_u32(st + ST_UID);
    unsigned int gid = load_u32(st + ST_GID);
    unsigned long ino = load_u64(st + ST_INO);
    unsigned long nlink = load_u64(st + ST_NLINK);
    unsigned long size = load_u64(st + ST_SIZE);
    unsigned long mtime = load_u64(st + ST_MTIME);
    printf("%s: type=%c mode=%x ino=%u nlink=%u uid=%u gid=%u size=%u mtime=%u\n",
           path, type_char(mode), mode, (unsigned int)ino, (unsigned int)nlink,
           uid, gid, (unsigned int)size, (unsigned int)mtime);
}

int main(int argc, char **argv) {
    int follow = 0;
    int ai = 1;
    int rc = 0;
    char st[ST_SIZE_BYTES];

    if (argc >= 2 && strcmp(argv[1], "-L") == 0) {
        follow = 1;
        ai = 2;
    }
    if (ai >= argc) {
        u_write(2, "usage: stat [-L] FILE...\n", 25);
        return 1;
    }
    for (; ai < argc; ai++) {
        long r;
        if (follow) {
            r = u_stat(argv[ai], st);
        } else {
            r = u_lstat(argv[ai], st);
        }
        if (r != 0) {
            printf("stat: %s: failed\n", argv[ai]);
            rc = 1;
            continue;
        }
        print_one(argv[ai], st);
    }
    return rc;
}
