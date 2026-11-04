/* fakeos userland - stat: print file status (thin).
 *   stat [-L] [-c FORMAT|-cFORMAT] FILE...
 * Default uses lstat(2) (nofollow). -L follows the final symlink via stat(2).
 * Default line: type/mode/ino/nlink/uid/gid/size/mtime (boot-relative secs).
 * -c FORMAT (GNU-ish thin): %n %s %i %a %f %F %h %u %g %Y %% ; glued -cFMT
 * and -Lc accepted. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_stat(const char *path, void *statbuf);
extern long u_lstat(const char *path, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
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

static void emit_ch(char c) {
    u_write(1, &c, 1);
}

static void emit_str(const char *s) {
    unsigned long n;
    if (s == 0) s = "";
    n = strlen(s);
    if (n > 0) u_write(1, s, n);
}

static void emit_udec(unsigned long v) {
    char tmp[24];
    int i = 0;
    int k;
    if (v == 0) {
        emit_ch('0');
        return;
    }
    while (v > 0 && i < 24) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (k = i - 1; k >= 0; k--) emit_ch(tmp[k]);
}

static void emit_oct(unsigned long v) {
    char tmp[16];
    int i = 16;
    if (v == 0) {
        emit_ch('0');
        return;
    }
    while (v > 0 && i > 0) {
        i--;
        tmp[i] = (char)('0' + (v & 7));
        v >>= 3;
    }
    u_write(1, tmp + i, (unsigned long)(16 - i));
}

static void emit_hex(unsigned long v) {
    const char *d = "0123456789abcdef";
    char tmp[20];
    int i = 0;
    int k;
    if (v == 0) {
        emit_ch('0');
        return;
    }
    while (v > 0 && i < 20) {
        tmp[i++] = d[v & 0xF];
        v >>= 4;
    }
    for (k = i - 1; k >= 0; k--) emit_ch(tmp[k]);
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

static const char *type_name(unsigned int mode) {
    unsigned int t = mode & S_IFMT;
    if (t == S_IFDIR) return "directory";
    if (t == S_IFCHR) return "character special file";
    if (t == S_IFIFO) return "fifo";
    if (t == S_IFSOCK) return "socket";
    if (t == S_IFLNK) return "symbolic link";
    if (t == S_IFREG) return "regular file";
    return "unknown";
}

static void print_default(const char *path, const char *st) {
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

static void print_format(const char *fmt, const char *path, const char *st) {
    unsigned int mode = load_u32(st + ST_MODE);
    unsigned int uid = load_u32(st + ST_UID);
    unsigned int gid = load_u32(st + ST_GID);
    unsigned long ino = load_u64(st + ST_INO);
    unsigned long nlink = load_u64(st + ST_NLINK);
    unsigned long size = load_u64(st + ST_SIZE);
    unsigned long mtime = load_u64(st + ST_MTIME);
    const char *p = fmt;

    while (*p) {
        if (*p != '%') {
            emit_ch(*p);
            p++;
            continue;
        }
        p++;
        if (*p == 0) {
            emit_ch('%');
            break;
        }
        if (*p == '%') {
            emit_ch('%');
        } else if (*p == 'n') {
            emit_str(path);
        } else if (*p == 's') {
            emit_udec(size);
        } else if (*p == 'i') {
            emit_udec(ino);
        } else if (*p == 'a') {
            emit_oct((unsigned long)(mode & 0xFFFU));
        } else if (*p == 'f') {
            emit_hex((unsigned long)mode);
        } else if (*p == 'F') {
            emit_str(type_name(mode));
        } else if (*p == 'h') {
            emit_udec(nlink);
        } else if (*p == 'u') {
            emit_udec((unsigned long)uid);
        } else if (*p == 'g') {
            emit_udec((unsigned long)gid);
        } else if (*p == 'Y') {
            emit_udec(mtime);
        } else {
            emit_ch('%');
            emit_ch(*p);
        }
        p++;
    }
    emit_ch('\n');
}

static void usage(void) {
    u_write(2, "usage: stat [-L] [-c FORMAT] FILE...\n", 37);
}

int main(int argc, char **argv) {
    int follow = 0;
    const char *cfmt = 0;
    int ai = 1;
    int rc = 0;
    char st[ST_SIZE_BYTES];

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *a = argv[ai];
        int j;
        if (strcmp(a, "--") == 0) {
            ai++;
            break;
        }
        /* Lone "-" is a FILE operand. */
        if (a[1] == 0) break;
        j = 1;
        while (a[j]) {
            char o = a[j];
            if (o == 'L') {
                follow = 1;
                j++;
            } else if (o == 'c') {
                if (a[j + 1] != 0) {
                    cfmt = a + j + 1;
                    j = (int)strlen(a); /* consume rest */
                } else {
                    if (ai + 1 >= argc) {
                        usage();
                        return 1;
                    }
                    cfmt = argv[ai + 1];
                    ai++;
                    j = (int)strlen(a);
                }
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    if (ai >= argc) {
        usage();
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
        if (cfmt) {
            print_format(cfmt, argv[ai], st);
        } else {
            print_default(argv[ai], st);
        }
    }
    return rc;
}
