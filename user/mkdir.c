/* fakeos userland - mkdir: create directories (thin; mirrors shell builtin).
 *   mkdir [-p] [-m MODE|-mMODE] DIR...
 * -p  create parents; existing directories are OK (re-stat after mkdir).
 * -m  set mode after create (octal; glued -mMODE keeps EXEC_MAX_ARGC=8).
 * External form for xargs/scripts that exec mkdir without the shell builtin.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_mkdir(const char *path);
extern long u_chmod(const char *path, long mode);
extern long u_stat(const char *path, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);

enum {
    ST_MODE = 24,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    PATH_CAP = 128
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

static int mkdir_one(const char *path, int parents) {
    char buf[PATH_CAP];
    unsigned long n;
    unsigned long i;
    if (path == 0 || path[0] == 0) return -1;
    if (!parents) {
        return u_mkdir(path) < 0 ? -1 : 0;
    }
    n = strlen(path);
    if (n >= PATH_CAP) return -1;
    strcpy(buf, path);
    i = 0;
    if (buf[0] == '/') i = 1;
    while (i <= n) {
        char saved;
        if (i < n && buf[i] != '/' && buf[i] != 0) {
            i++;
            continue;
        }
        saved = buf[i];
        buf[i] = 0;
        if (buf[0] != 0 && !(buf[0] == '/' && buf[1] == 0)) {
            if (!is_dir(buf)) {
                if (u_mkdir(buf) < 0) {
                    if (!is_dir(buf)) {
                        buf[i] = saved;
                        return -1;
                    }
                }
            }
        }
        buf[i] = saved;
        if (i >= n) break;
        while (i < n && buf[i] == '/') i++;
        if (i >= n) break;
    }
    return 0;
}

static void usage(void) {
    u_write(2, "usage: mkdir [-p] [-m MODE] DIR...\n", 35);
}

int main(int argc, char **argv) {
    int parents = 0;
    int have_mode = 0;
    unsigned int mode = 0;
    int ai = 1;
    int rc = 0;
    int saw = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) break;
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'p') {
                parents = 1;
            } else if (o[j] == 'm') {
                const char *ms;
                if (o[j + 1] != 0) {
                    ms = o + j + 1;
                    j = (int)strlen(o) - 1;
                } else {
                    ai++;
                    if (ai >= argc) {
                        usage();
                        return 1;
                    }
                    ms = argv[ai];
                }
                if (!parse_mode(ms, &mode)) {
                    u_write(2, "mkdir: bad mode\n", 16);
                    return 1;
                }
                have_mode = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }
    if (ai >= argc) {
        u_write(2, "mkdir: missing operand\n", 23);
        return 1;
    }
    while (ai < argc) {
        saw = 1;
        if (mkdir_one(argv[ai], parents) < 0) {
            printf("mkdir: %s: cannot create directory\n", argv[ai]);
            rc = 1;
        } else if (have_mode) {
            /* chmod final path only (GNU mkdir -m with -p). */
            if (u_chmod(argv[ai], (long)mode) != 0) {
                printf("mkdir: %s: chmod failed\n", argv[ai]);
                rc = 1;
            }
        }
        ai++;
    }
    if (!saw) {
        u_write(2, "mkdir: missing operand\n", 23);
        return 1;
    }
    return rc;
}
