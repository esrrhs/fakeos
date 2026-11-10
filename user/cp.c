/* fakeos userland - cp: copy files; -r/-R trees; -n noclobber; -v verbose.
 * Forms:
 *   cp [-r|-R] [-nv] SRC DST
 *   cp [-r|-R] [-nv] SRC... DIRECTORY
 * If DST/DIRECTORY exists as a directory, place each SRC under it as
 * DIRECTORY/basename(SRC). Refuse directory SRC without -r. Refuse cycles
 * (DST under SRC, including physical aliases). -r preserves symbolic links
 * without following cycles; existing different link destinations are refused.
 * -n skips existing destinations (quiet, exit 0). -v prints
 * each successful copy as: 'SRC' -> 'DST' (GNU). Sticky clusters (-rnv/-nrv).
 * Compact argv for EXEC_MAX_ARGC=8. Not -p (needs utimensat — deferred).
 */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_stat(const char *path, void *statbuf);
extern long u_lstat(const char *path, void *statbuf);
extern long u_fstat(long fd, void *statbuf);
extern long u_ftruncate(long fd, unsigned long length);
extern long u_mkdir(const char *path);
extern long u_chdir(const char *path);
extern long u_getcwd(char *buf, unsigned long n);
extern long u_readlink(const char *path, char *buf, unsigned long n);
extern long u_symlink(const char *target, const char *path);
extern long u_exit(long code);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_EXCL   = 0x80,
    ST_DEV = 0,
    ST_INO = 8,
    ST_MODE = 24,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    S_IFREG = 0x8000,
    S_IFLNK = 0xA000,
    PATH_CAP = 128,
    DENT_BUF = 1920,
    DENT_REC = 48,
    NAME_OFF = 19,
    NAME_LEN = 30,
    MAX_NAMES = 64
};

static unsigned int load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
}

static u64 load_u64(const char *p) {
    return (u64)load_u32(p) | ((u64)load_u32(p + 4) << 32);
}

static int same_identity(const char *a, const char *b) {
    return load_u64(a + ST_DEV) == load_u64(b + ST_DEV)
        && load_u64(a + ST_INO) == load_u64(b + ST_INO);
}

static int is_dir(const char *path) {
    char st[ST_SIZE_BYTES];
    unsigned int mode;
    if (u_stat(path, st) != 0) return 0;
    mode = load_u32(st + ST_MODE);
    return (mode & S_IFMT) == S_IFDIR;
}

static int path_exists(const char *path) {
    char st[ST_SIZE_BYTES];
    return u_lstat(path, st) == 0;
}

static const char *basename_of(const char *path) {
    const char *base = path;
    unsigned long i = 0;
    while (path[i] != 0) {
        if (path[i] == '/' && path[i + 1] != 0) {
            base = path + i + 1;
        }
        i++;
    }
    return base;
}

static int join_dir_base(char *out, unsigned long cap,
                         const char *dir, const char *base) {
    unsigned long dl = strlen(dir);
    unsigned long bl = strlen(base);
    unsigned long i;
    unsigned long o = 0;
    int need_slash;
    if (dl + bl + 2 > cap) return -1;
    for (i = 0; i < dl; i++) out[o++] = dir[i];
    need_slash = (dl == 0 || dir[dl - 1] != '/');
    if (need_slash) out[o++] = '/';
    for (i = 0; i < bl; i++) out[o++] = base[i];
    out[o] = 0;
    return 0;
}

/* True if path is ancestor or equal to child (plain string prefix + '/'). */
static int is_under(const char *ancestor, const char *child) {
    unsigned long al = strlen(ancestor);
    unsigned long i;
    if (al == 0) return 0;
    if (strcmp(ancestor, child) == 0) return 1;
    for (i = 0; i < al; i++) {
        if (child[i] != ancestor[i]) return 0;
    }
    /* ancestor without trailing slash: child must continue with '/' */
    if (ancestor[al - 1] == '/') return 1;
    return child[al] == '/';
}

static void announce(const char *src, const char *dst) {
    printf("'%s' -> '%s'\n", src, dst);
}

/* Resolve the existing destination (or its parent) through physical cwd,
 * then compare every ancestor's identity, including /mnt versus /home aliases.
 * This is a single-threaded utility; always restore cwd before returning.
 * Refuse unresolved/oversize paths instead of making a guessed-safe copy. */
static int directory_target_safe(const char *src, const char *dst) {
    char source[ST_SIZE_BYTES];
    char st[ST_SIZE_BYTES];
    char parent[PATH_CAP];
    char oldcwd[PATH_CAP];
    char physical[PATH_CAP];
    unsigned long n = strlen(dst);
    unsigned long i;
    long got;
    if (n == 0 || n >= PATH_CAP || u_stat(src, source) < 0) return 0;
    strcpy(parent, dst);
    if (!is_dir(dst)) {
        while (n > 1 && parent[n - 1] == '/') parent[--n] = 0;
        while (n > 0 && parent[n - 1] != '/') n--;
        if (n == 0) strcpy(parent, ".");
        else if (n == 1) parent[1] = 0;
        else parent[n - 1] = 0;
    }
    if (u_getcwd(oldcwd, sizeof(oldcwd)) < 0 || u_chdir(parent) < 0) return 0;
    got = u_getcwd(physical, sizeof(physical));
    if (u_chdir(oldcwd) < 0) {
        /* Continuing a multi-source copy with the wrong cwd risks data loss. */
        printf("cp: cannot restore working directory\n");
        u_exit(1);
        return 0;
    }
    if (got < 0 || physical[0] != '/') return 0;
    /* getcwd is physical; a symlink here indicates failed kernel resolution.
     * Fail closed rather than following a lexical fallback into our source. */
    for (i = 1; physical[i] != 0; i++) {
        if (physical[i] == '/') {
            physical[i] = 0;
            got = u_lstat(physical, st);
            physical[i] = '/';
            if (got < 0 || (load_u32(st + ST_MODE) & S_IFMT) == S_IFLNK) return 0;
        }
    }
    if (u_lstat(physical, st) < 0
        || (load_u32(st + ST_MODE) & S_IFMT) == S_IFLNK) return 0;
    for (;;) {
        if (u_stat(physical, st) < 0 || same_identity(source, st)) return 0;
        n = strlen(physical);
        if (n == 1 && physical[0] == '/') break;
        while (n > 1 && physical[n - 1] != '/') n--;
        if (n <= 1) physical[1] = 0;
        else physical[n - 1] = 0;
    }
    return 1;
}

/* Recursive copies preserve links; never follow directory/symlink cycles.
 * Existing identical links are idempotent. Other existing entries are refused
 * rather than unlinking user data before a potentially failing symlink call. */
static int copy_link(const char *src, const char *dst, int noclobber, int verbose) {
    char target[256];
    char prior[256];
    char st[ST_SIZE_BYTES];
    long n = u_readlink(src, target, sizeof(target) - 1);
    if (n < 0 || n >= (long)sizeof(target) - 1) {
        printf("cp: %s: cannot read symbolic link\n", src);
        return 1;
    }
    target[n] = 0;
    if (u_lstat(dst, st) == 0) {
        if (noclobber) return 0;
        if ((load_u32(st + ST_MODE) & S_IFMT) == S_IFLNK) {
            long k = u_readlink(dst, prior, sizeof(prior) - 1);
            if (k >= 0 && k < (long)sizeof(prior) - 1) {
                prior[k] = 0;
                if (strcmp(target, prior) == 0) return 0;
            }
        }
        printf("cp: %s: refusing to replace existing entry with symbolic link\n", dst);
        return 1;
    }
    if (u_symlink(target, dst) < 0) {
        printf("cp: %s: cannot create symbolic link\n", dst);
        return 1;
    }
    if (verbose) announce(src, dst);
    return 0;
}

static int copy_file(const char *src, const char *dst, int noclobber, int verbose) {
    char buf[1024];
    char src_st[ST_SIZE_BYTES];
    char dst_st[ST_SIZE_BYTES];
    long in;
    long out;
    long n;
    int rc = 0;

    if (noclobber && path_exists(dst)) return 0;
    in = u_open(src, O_RDONLY, 0);
    if (in < 0) {
        printf("cp: %s: cannot open\n", src);
        return 1;
    }
    if (u_fstat(in, src_st) < 0) {
        printf("cp: %s: cannot stat\n", src);
        u_close(in);
        return 1;
    }
    /* Never truncate through a pathname. Compare the OPEN file identities
     * first: strings cannot detect hard links, symlinks or ./../ aliases.
     * O_EXCL asks the backend to reject an existing -n destination too.
     * This is not a transaction against concurrent rename/unlink. */
    out = u_open(dst, O_WRONLY | O_CREAT | (noclobber ? O_EXCL : 0), 0x1A4);
    if (out < 0) {
        u_close(in);
        if (noclobber && path_exists(dst)) return 0;
        printf("cp: %s: cannot create\n", dst);
        return 1;
    }
    if (u_fstat(out, dst_st) < 0) {
        printf("cp: %s: cannot stat\n", dst);
        rc = 1;
    } else if (same_identity(src_st, dst_st)) {
        printf("cp: %s and %s are the same file\n", src, dst);
        rc = 1;
    } else if ((load_u32(dst_st + ST_MODE) & S_IFMT) == S_IFREG
               && u_ftruncate(out, 0) < 0) {
        printf("cp: %s: cannot truncate\n", dst);
        rc = 1;
    }
    while (rc == 0) {
        long off = 0;
        n = u_read(in, buf, sizeof(buf));
        if (n == -4) continue; /* EINTR */
        if (n < 0) {
            printf("cp: %s: read failed\n", src);
            rc = 1;
            break;
        }
        if (n == 0) break;
        while (off < n) {
            long written = u_write(out, buf + off, (u64)(n - off));
            if (written == -4) continue;
            if (written <= 0 || written > n - off) {
                printf("cp: %s: write failed\n", dst);
                rc = 1;
                break;
            }
            off += written;
        }
    }
    if (u_close(in) < 0) rc = 1;
    if (u_close(out) < 0) rc = 1;
    if (verbose && rc == 0) announce(src, dst);
    return rc;
}

static int copy_path(const char *src, const char *dst, int recursive, int noclobber,
                     int verbose);

static int copy_dir_rec(const char *src, const char *dst, int noclobber, int verbose) {
    char dent[DENT_BUF];
    char child_src[PATH_CAP];
    char child_dst[PATH_CAP];
    char names[MAX_NAMES][NAME_LEN];
    int nnames;
    long fd;
    long n;
    int i;
    int rc = 0;

    char src_st[ST_SIZE_BYTES];
    char dst_st[ST_SIZE_BYTES];
    if (is_under(src, dst) || !directory_target_safe(src, dst)
        || (u_stat(src, src_st) == 0 && u_stat(dst, dst_st) == 0
            && same_identity(src_st, dst_st))) {
        printf("cp: %s: cannot copy a directory into itself\n", src);
        return 1;
    }

    if (!path_exists(dst)) {
        if (u_mkdir(dst) < 0) {
            printf("cp: %s: cannot create directory\n", dst);
            return 1;
        }
        if (verbose) announce(src, dst);
    } else if (!is_dir(dst)) {
        printf("cp: %s: not a directory\n", dst);
        return 1;
    }

    fd = u_open(src, O_RDONLY, 0);
    if (fd < 0) {
        printf("cp: %s: cannot open directory\n", src);
        return 1;
    }
    nnames = 0;
    for (;;) {
        long off;
        n = u_getdents(fd, dent, sizeof(dent));
        if (n == -4) continue;
        if (n < 0) {
            printf("cp: %s: directory read failed\n", src);
            u_close(fd);
            return 1;
        }
        if (n == 0) break;
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
                printf("cp: %s: too many entries\n", src);
                u_close(fd);
                return 1;
            }
            strcpy(names[nnames], name);
            nnames++;
        }
    }
    u_close(fd);

    for (i = 0; i < nnames; i++) {
        if (join_dir_base(child_src, PATH_CAP, src, names[i]) < 0
            || join_dir_base(child_dst, PATH_CAP, dst, names[i]) < 0) {
            u_write(2, "cp: path too long\n", 18);
            rc = 1;
            continue;
        }
        if (copy_path(child_src, child_dst, 1, noclobber, verbose) != 0) rc = 1;
    }
    return rc;
}

static int copy_path(const char *src, const char *dst, int recursive, int noclobber,
                     int verbose) {
    char st[ST_SIZE_BYTES];
    if (recursive && u_lstat(src, st) == 0
        && (load_u32(st + ST_MODE) & S_IFMT) == S_IFLNK) {
        return copy_link(src, dst, noclobber, verbose);
    }
    if (is_dir(src)) {
        if (!recursive) {
            printf("cp: %s: is a directory (not copied)\n", src);
            return 1;
        }
        return copy_dir_rec(src, dst, noclobber, verbose);
    }
    return copy_file(src, dst, noclobber, verbose);
}

static void usage(void) {
    u_write(2, "usage: cp [-rnvR] SRC DST|DIR...\n", 34);
}

int main(int argc, char **argv) {
    int i;
    int ai = 1;
    int recursive = 0;
    int noclobber = 0;
    int verbose = 0;
    int rc = 0;
    int nops;

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
            } else if (o[j] == 'n') {
                noclobber = 1;
            } else if (o[j] == 'v') {
                verbose = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    nops = argc - ai;
    if (nops < 2) {
        usage();
        return 1;
    }

    if (nops == 2) {
        const char *src = argv[ai];
        const char *dst = argv[ai + 1];
        char joined[PATH_CAP];
        if (is_dir(dst)) {
            if (join_dir_base(joined, PATH_CAP, dst, basename_of(src)) < 0) {
                u_write(2, "cp: path too long\n", 18);
                return 1;
            }
            dst = joined;
        }
        return copy_path(src, dst, recursive, noclobber, verbose);
    }

    /* Multiple sources into directory. */
    {
        const char *dir = argv[argc - 1];
        if (!is_dir(dir)) {
            printf("cp: %s: not a directory\n", dir);
            return 1;
        }
        for (i = ai; i < argc - 1; i++) {
            char joined[PATH_CAP];
            if (join_dir_base(joined, PATH_CAP, dir, basename_of(argv[i])) < 0) {
                u_write(2, "cp: path too long\n", 18);
                rc = 1;
                continue;
            }
            if (copy_path(argv[i], joined, recursive, noclobber, verbose) != 0) rc = 1;
        }
    }
    return rc;
}
