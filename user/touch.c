/* fakeos userland - touch: create empty file or update timestamps.
 * Forms: touch [-c] [-r REF] FILE...
 * -c: do not create missing files (POSIX); missing paths are skipped OK.
 * -r REF: use REF's mtime (lstat) via utimensat(2).
 * Default: utimensat NOW on existing paths; create then NOW if missing.
 */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_lstat(const char *path, void *statbuf);
extern long u_utimensat(long dfd, const char *path, const void *times, long flags);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR   = 2,
    O_CREAT  = 0x40,
    AT_FDCWD  = -100,
    ST_MTIME = 88,
    ST_SIZE_BYTES = 144
};

static void usage(void) {
    u_write(2, "usage: touch [-c] [-r REF] FILE...\n", 34);
}

static unsigned long load_u64(const char *p) {
    unsigned long v = 0;
    unsigned long i;
    for (i = 0; i < 8; i++) {
        v |= ((unsigned long)(unsigned char)p[i]) << (8 * i);
    }
    return v;
}

static void store_u64(char *p, unsigned long v) {
    unsigned long i;
    for (i = 0; i < 8; i++) {
        p[i] = (char)(v & 0xFF);
        v >>= 8;
    }
}

/* times[2] timespec: set both atime+mtime to sec (nsec=0). */
static int set_mtime_sec(const char *path, unsigned long sec) {
    char ts[32];
    unsigned long i;
    for (i = 0; i < 32; i++) ts[i] = 0;
    store_u64(ts + 0, sec);
    store_u64(ts + 16, sec);
    return u_utimensat(AT_FDCWD, path, ts, 0) == 0 ? 1 : 0;
}

static int set_mtime_now(const char *path) {
    return u_utimensat(AT_FDCWD, path, 0, 0) == 0 ? 1 : 0;
}

static int touch_one(const char *path, int no_create, int have_ref,
                     unsigned long ref_sec) {
    char st[ST_SIZE_BYTES];
    long fd;
    int exists;

    exists = (u_lstat(path, st) == 0);
    if (!exists) {
        if (no_create) {
            return 1;
        }
        fd = u_open(path, O_WRONLY | O_CREAT, 0x1A4);
        if (fd < 0) {
            printf("touch: %s: cannot touch\n", path);
            return 0;
        }
        u_close(fd);
    }
    if (have_ref) {
        if (!set_mtime_sec(path, ref_sec)) {
            printf("touch: %s: cannot set times\n", path);
            return 0;
        }
    } else if (!set_mtime_now(path)) {
        printf("touch: %s: cannot set times\n", path);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int i;
    int ai = 1;
    int no_create = 0;
    int have_ref = 0;
    unsigned long ref_sec = 0;
    int rc = 0;
    const char *ref_path = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) {
            ai++;
            break;
        }
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'c') {
                no_create = 1;
            } else if (o[j] == 'r') {
                if (o[j + 1] != 0) {
                    ref_path = o + j + 1;
                    while (o[j + 1] != 0) j++;
                } else {
                    ai++;
                    if (ai >= argc) {
                        usage();
                        return 1;
                    }
                    ref_path = argv[ai];
                }
                have_ref = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    if (have_ref) {
        char st[ST_SIZE_BYTES];
        if (ref_path == 0 || u_lstat(ref_path, st) != 0) {
            printf("touch: %s: cannot stat\n", ref_path ? ref_path : "(null)");
            return 1;
        }
        ref_sec = load_u64(st + ST_MTIME);
    }

    if (ai >= argc) {
        usage();
        return 1;
    }

    for (i = ai; i < argc; i++) {
        if (!touch_one(argv[i], no_create, have_ref, ref_sec)) {
            rc = 1;
        }
    }
    return rc;
}
