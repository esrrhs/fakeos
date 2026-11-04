/* fakeos userland - mkdir: create directories (thin; mirrors shell builtin).
 *   mkdir [-p] DIR...
 * -p  create parents; existing directories are OK (re-stat after mkdir).
 * External form for xargs/scripts that exec mkdir without the shell builtin.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_mkdir(const char *path);
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

int main(int argc, char **argv) {
    int parents = 0;
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
            } else {
                u_write(2, "usage: mkdir [-p] DIR...\n", 24);
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
        }
        ai++;
    }
    if (!saw) {
        u_write(2, "mkdir: missing operand\n", 23);
        return 1;
    }
    return rc;
}
