/* fakeos userland - rm: unlink files; -r/-R removes directories recursively. */
package user;

extern long u_unlink(const char *path);
extern long u_rmdir(const char *path);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern long u_stat(const char *path, void *statbuf);
extern long u_write(long fd, const void *buf, unsigned long n);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    ST_MODE = 24,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    PATH_CAP = 128,
    DENT_BUF = 1920,
    DENT_REC = 48,
    NAME_OFF = 19,
    TYPE_OFF = 18,
    DT_DIR = 4,
    MAX_NAMES = 64,
    NAME_LEN = 30
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

static void join_path(char *out, unsigned long cap, const char *dir,
                      const char *name) {
    unsigned long i = 0;
    unsigned long j = 0;
    if (cap < 2) {
        out[0] = 0;
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

static int rm_path(const char *path, int recursive);

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
        printf("rm: %s: cannot open directory\n", path);
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
                printf("rm: %s: too many entries\n", path);
                u_close(fd);
                return 1;
            }
            strcpy(names[nnames], name);
            nnames++;
        }
    }
    u_close(fd);

    for (i = 0; i < nnames; i++) {
        join_path(child, PATH_CAP, path, names[i]);
        if (rm_path(child, 1) != 0) rc = 1;
    }
    if (u_rmdir(path) != 0) {
        printf("rm: %s: cannot remove directory\n", path);
        rc = 1;
    }
    return rc;
}

static int rm_path(const char *path, int recursive) {
    if (is_dir(path)) {
        if (!recursive) {
            printf("rm: %s: is a directory\n", path);
            return 1;
        }
        return rm_dir_rec(path);
    }
    if (u_unlink(path) != 0) {
        printf("rm: %s: cannot remove\n", path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int i;
    int ai = 1;
    int recursive = 0;
    int rc = 0;
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
            } else {
                u_write(2, "usage: rm [-r|-R] FILE...\n", 26);
                return 1;
            }
        }
        ai++;
    }
    if (ai >= argc) {
        u_write(2, "usage: rm [-r|-R] FILE...\n", 26);
        return 1;
    }
    for (i = ai; i < argc; i++) {
        if (rm_path(argv[i], recursive) != 0) rc = 1;
    }
    return rc;
}
