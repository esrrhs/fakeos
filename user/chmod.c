/* fakeos userland - chmod: change file mode bits (thin; mirrors shell builtin).
 *   chmod [-R] MODE FILE...
 * MODE is an octal integer (e.g. 755, 0644). -R walks directories depth-first
 * (post-order: children then self) so a restrictive MODE still reaches leaves.
 * Uses lstat(2); does not descend through symlink directories. Depth capped.
 * External form for xargs/scripts.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern long u_lstat(const char *path, void *statbuf);
extern long u_chmod(const char *path, long mode);
extern int strcmp(const char *a, const char *b);
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
    NAME_LEN = 30,
    MAX_NAMES = 64,
    MAX_DEPTH = 16
};

static unsigned int load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
}

static int is_dir_mode(unsigned int mode) {
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
    if (i > 0 && out[i - 1] != '/' && i + 1 < cap) out[i++] = '/';
    while (name[j] != 0 && i + 1 < cap) out[i++] = name[j++];
    out[i] = 0;
}

static int chmod_path(const char *path, unsigned int mode, int recursive,
                      int depth);

static int chmod_dir_rec(const char *path, unsigned int mode, int depth) {
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
        printf("chmod: %s: cannot open directory\n", path);
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
                printf("chmod: %s: too many entries\n", path);
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
        if (child[0] == 0) continue;
        if (chmod_path(child, mode, 1, depth + 1) != 0) rc = 1;
    }
    if (u_chmod(path, (long)mode) != 0) {
        printf("chmod: %s: failed\n", path);
        rc = 1;
    }
    return rc;
}

static int chmod_path(const char *path, unsigned int mode, int recursive,
                      int depth) {
    char st[ST_SIZE_BYTES];
    unsigned int st_mode;

    if (u_lstat(path, st) != 0) {
        printf("chmod: %s: failed\n", path);
        return 1;
    }
    st_mode = load_u32(st + ST_MODE);
    if (recursive && is_dir_mode(st_mode)) {
        if (depth >= MAX_DEPTH) {
            printf("chmod: %s: max depth\n", path);
            return 1;
        }
        return chmod_dir_rec(path, mode, depth);
    }
    if (u_chmod(path, (long)mode) != 0) {
        printf("chmod: %s: failed\n", path);
        return 1;
    }
    return 0;
}

static int parse_mode(const char *s, unsigned int *out) {
    unsigned int mode = 0;
    int j;
    if (s == 0 || s[0] == 0) return 0;
    for (j = 0; s[j] != 0; j++) {
        if (s[j] < '0' || s[j] > '7') return 0;
        mode = (mode << 3) | (unsigned int)(s[j] - '0');
    }
    *out = mode;
    return 1;
}

int main(int argc, char **argv) {
    unsigned int mode = 0;
    int ai = 1;
    int recursive = 0;
    int rc = 0;
    int i;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) break; /* lone "-" operand */
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'R' || o[j] == 'r') {
                recursive = 1;
            } else {
                u_write(2, "usage: chmod [-R] MODE FILE...\n", 31);
                return 1;
            }
        }
        ai++;
    }
    if (ai + 1 >= argc) {
        u_write(2, "usage: chmod [-R] MODE FILE...\n", 31);
        return 1;
    }
    if (!parse_mode(argv[ai], &mode)) {
        u_write(2, "chmod: bad mode\n", 16);
        return 1;
    }
    ai++;
    for (i = ai; i < argc; i++) {
        if (chmod_path(argv[i], mode, recursive, 0) != 0) rc = 1;
    }
    return rc;
}
