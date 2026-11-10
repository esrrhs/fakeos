/* fakeos userland - chmod: change file mode bits (thin; mirrors shell builtin).
 *   chmod [-Rv] MODE FILE...
 * MODE is an octal integer (e.g. 755, 0644). -R walks directories depth-first
 * (post-order: children then self) so a restrictive MODE still reaches leaves.
 * -v prints mode of 'PATH' changed from OLD to NEW (or retained as MODE)
 * using 4-digit octal (mode & 07777), GNU-ish thin. Uses lstat(2); does not
 * descend through symlink directories. Depth capped. Glued -Rv/-vR.
 * External form for xargs/scripts. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern unsigned long strlen(const char *s);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern long u_dirent_reclen(const char *p, unsigned long avail);
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
    MODE_BITS = 0xFFF, /* 07777 */
    PATH_CAP = 512,
    DENT_BUF = 1920,
    NAME_OFF = 19,
    NAME_LEN = 256,
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
    unsigned long dl = strlen(dir);
    unsigned long nl = strlen(name);
    unsigned long need = dl + nl + 1 + (dl && dir[dl - 1] != '/' ? 1 : 0);
    if (need > cap) { if (cap) out[0] = 0; return; }
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

static void mode_oct4(char *out, unsigned int mode) {
    unsigned int m = mode & MODE_BITS;
    out[0] = (char)('0' + ((m >> 9) & 7));
    out[1] = (char)('0' + ((m >> 6) & 7));
    out[2] = (char)('0' + ((m >> 3) & 7));
    out[3] = (char)('0' + (m & 7));
    out[4] = 0;
}

static void announce(const char *path, unsigned int oldm, unsigned int newm) {
    char a[8];
    char b[8];
    mode_oct4(a, oldm);
    mode_oct4(b, newm);
    if ((oldm & MODE_BITS) == (newm & MODE_BITS)) {
        printf("mode of '%s' retained as %s\n", path, a);
    } else {
        printf("mode of '%s' changed from %s to %s\n", path, a, b);
    }
}

static int chmod_path(const char *path, unsigned int mode, int recursive,
                      int verbose, int depth);

static int chmod_dir_rec(const char *path, unsigned int mode, int verbose,
                         int depth) {
    char dent[DENT_BUF];
    char child[PATH_CAP];
    char names[MAX_NAMES][NAME_LEN];
    char st[ST_SIZE_BYTES];
    unsigned int oldm = 0;
    int nnames;
    long fd;
    long n;
    int i;
    int rc = 0;

    if (u_lstat(path, st) == 0) {
        oldm = load_u32(st + ST_MODE);
    }

    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("chmod: %s: cannot open directory\n", path);
        return 1;
    }
    nnames = 0;
    for (;;) {
        long off;
        n = u_getdents(fd, dent, sizeof(dent));
        if (n == -4) continue;
        if (n < 0) { u_close(fd); return 1; }
        if (n == 0) break;
        long reclen;
        for (off = 0; off < n; off += reclen) {
            reclen = u_dirent_reclen(dent + off, n - off);
            if (reclen < 0) { u_close(fd); return 1; }
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
        if (child[0] == 0) { rc = 1; continue; }
        if (chmod_path(child, mode, 1, verbose, depth + 1) != 0) rc = 1;
    }
    if (u_chmod(path, (long)mode) != 0) {
        printf("chmod: %s: failed\n", path);
        rc = 1;
    } else if (verbose) {
        announce(path, oldm, mode);
    }
    return rc;
}

static int chmod_path(const char *path, unsigned int mode, int recursive,
                      int verbose, int depth) {
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
        return chmod_dir_rec(path, mode, verbose, depth);
    }
    if (u_chmod(path, (long)mode) != 0) {
        printf("chmod: %s: failed\n", path);
        return 1;
    }
    if (verbose) announce(path, st_mode, mode);
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
    int verbose = 0;
    int rc = 0;
    int i;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) break; /* lone "-" operand */
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'R' || o[j] == 'r') {
                recursive = 1;
            } else if (o[j] == 'v') {
                verbose = 1;
            } else {
                u_write(2, "usage: chmod [-Rv] MODE FILE...\n", 33);
                return 1;
            }
        }
        ai++;
    }
    if (ai + 1 >= argc) {
        u_write(2, "usage: chmod [-Rv] MODE FILE...\n", 33);
        return 1;
    }
    if (!parse_mode(argv[ai], &mode)) {
        u_write(2, "chmod: bad mode\n", 16);
        return 1;
    }
    ai++;
    for (i = ai; i < argc; i++) {
        if (chmod_path(argv[i], mode, recursive, verbose, 0) != 0) rc = 1;
    }
    return rc;
}
