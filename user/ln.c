/* fakeos userland - ln: create hard or symbolic links (thin; mirrors shell).
 *   ln [-s] [-f] [-v] TARGET LINKPATH
 * -s  create a symlink via symlink(2); otherwise hard link via link(2).
 * -f  force: unlink LINKPATH first if it exists (not a directory).
 * -v  print 'LINKPATH' -> 'TARGET' (symlink) or 'LINKPATH' => 'TARGET'
 *     (hard link) after success (GNU). Glued -sfv/-svf/… accepted.
 * External form for xargs/scripts. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_link(const char *oldpath, const char *newpath);
extern long u_symlink(const char *target, const char *linkpath);
extern long u_unlink(const char *path);
extern long u_stat(const char *path, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum {
    ST_MODE = 24,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000
};

static void usage(void) {
    u_write(2, "usage: ln [-sfv] TARGET LINKPATH\n", 33);
}

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

static int force_unlink(const char *path) {
    if (is_dir(path)) {
        u_write(2, "ln: cannot overwrite directory\n", 31);
        return -1;
    }
    /* Best-effort: missing is fine; other failures still try link below. */
    u_unlink(path);
    return 0;
}

static void announce(const char *target, const char *linkpath, int symbolic) {
    if (symbolic) {
        printf("'%s' -> '%s'\n", linkpath, target);
    } else {
        printf("'%s' => '%s'\n", linkpath, target);
    }
}

int main(int argc, char **argv) {
    int ai = 1;
    int symbolic = 0;
    int force = 0;
    int verbose = 0;
    const char *target;
    const char *linkpath;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) break;
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 's') {
                symbolic = 1;
            } else if (o[j] == 'f') {
                force = 1;
            } else if (o[j] == 'v') {
                verbose = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }
    if (ai + 2 != argc) {
        usage();
        return 1;
    }
    target = argv[ai];
    linkpath = argv[ai + 1];
    if (force) {
        if (force_unlink(linkpath) != 0) return 1;
    }
    if (symbolic) {
        if (u_symlink(target, linkpath) != 0) {
            u_write(2, "ln: symlink failed\n", 19);
            return 1;
        }
        if (verbose) announce(target, linkpath, 1);
        return 0;
    }
    if (u_link(target, linkpath) != 0) {
        u_write(2, "ln: link failed\n", 16);
        return 1;
    }
    if (verbose) announce(target, linkpath, 0);
    return 0;
}
