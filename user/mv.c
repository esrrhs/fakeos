/* fakeos userland - mv: rename(2); falls back to copy-tree+rm across FS.
 * Forms:
 *   mv [-nv] SRC DST
 *   mv [-nv] SRC... DIRECTORY
 * If DST/DIRECTORY exists as a directory, place each SRC under it as
 * DIRECTORY/basename(SRC). Directories move via rename when possible;
 * otherwise copy-tree then recursive remove. Refuse cycles (DST under
 * SRC). -n noclobber: skip existing destinations (quiet, exit 0). -v prints
 * 'SRC' -> 'DST' once per successful top-level move (GNU). Sticky -nv/-vn.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_unlink(const char *path);
extern long u_rename(const char *oldpath, const char *newpath);
extern long u_getcwd(char *buf, unsigned long size);
extern long u_chdir(const char *path);
extern void u_exit(long code);
extern long u_lstat(const char *path, void *statbuf);
extern long u_readlink(const char *path, char *buf, unsigned long size);
extern long u_symlink(const char *target, const char *path);
extern long u_stat(const char *path, void *statbuf);
extern long u_mkdir(const char *path);
extern long u_rmdir(const char *path);
extern long u_fstat(long fd, void *statbuf);
extern long u_ftruncate(long fd, unsigned long size);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern long u_dirent_reclen(const char *p, unsigned long avail);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200,
    ST_MODE = 24,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    S_IFREG = 0x8000,
    S_IFLNK = 0xA000,
    PATH_CAP = 512,
    DENT_BUF = 1920,
    NAME_OFF = 19,
    NAME_LEN = 256,
    MAX_NAMES = 64
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
    if (ancestor[al - 1] == '/') return 1;
    return child[al] == '/';
}

/* Compare OPEN descriptor identities before any destination truncation. */
static int same_identity(const char *a, const char *b) {
    int i;
    for (i = 0; i < 16; i++) if (a[i] != b[i]) return 0; /* st_dev + st_ino */
    return 1;
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
        printf("mv: cannot restore working directory\n");
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


static int copy_file(const char *src, const char *dst) {
    char buf[1024];
    char src_st[ST_SIZE_BYTES];
    char dst_st[ST_SIZE_BYTES];
    long in;
    long out;
    long n;
    int rc = 0;

    in = u_open(src, O_RDONLY, 0);
    if (in < 0) {
        printf("mv: %s: cannot open\n", src);
        return 1;
    }
    if (u_fstat(in, src_st) < 0
        || (load_u32(src_st + ST_MODE) & S_IFMT) != S_IFREG) {
        printf("mv: %s: cannot copy regular file\n", src);
        u_close(in);
        return 1;
    }
    /* Rename may fail without EXDEV. Never truncate a same-inode alias, and
     * never unlink a source after copying to an unsupported special device. */
    out = u_open(dst, O_WRONLY | O_CREAT, 0x1A4);
    if (out < 0) {
        printf("mv: %s: cannot create\n", dst);
        u_close(in);
        return 1;
    }
    if (u_fstat(out, dst_st) < 0
        || (load_u32(dst_st + ST_MODE) & S_IFMT) != S_IFREG) {
        printf("mv: %s: cannot copy regular file\n", dst);
        rc = 1;
    } else if (same_identity(src_st, dst_st)) {
        printf("mv: %s and %s are the same file\n", src, dst);
        rc = 1;
    } else if (u_ftruncate(out, 0) < 0) {
        printf("mv: %s: cannot truncate\n", dst);
        rc = 1;
    }
    while (rc == 0) {
        long off = 0;
        n = u_read(in, buf, sizeof(buf));
        if (n == -4) continue; /* EINTR is not EOF. */
        if (n < 0) { printf("mv: %s: read failed\n", src); rc = 1; break; }
        if (n == 0) break;
        while (off < n) {
            long written = u_write(out, buf + off, (unsigned long)(n - off));
            if (written == -4) continue;
            if (written <= 0 || written > n - off) {
                printf("mv: %s: write failed\n", dst);
                rc = 1;
                break;
            }
            off += written;
        }
    }
    /* copy_unlink / move_one may remove the source ONLY after every read,
     * write and both closes succeeded. Partial destinations can remain. */
    if (u_close(in) < 0) rc = 1;
    if (u_close(out) < 0) rc = 1;
    return rc;
}

/* Cross-FS traversal copies links themselves, never their outside targets.
 * Existing entries are refused conservatively; no-clobber skips them earlier. */
static int copy_link(const char *src, const char *dst) {
    char target[PATH_CAP];
    char st[ST_SIZE_BYTES];
    long n = u_readlink(src, target, PATH_CAP - 1);
    if (n <= 0 || n >= PATH_CAP - 1) return 1;
    target[n] = 0;
    if (u_lstat(dst, st) == 0) {
        printf("mv: %s: refusing to replace existing entry with symbolic link\n", dst);
        return 1;
    }
    if (u_symlink(target, dst) < 0) return 1;
    return 0;
}

static int copy_path(const char *src, const char *dst);
static int rm_path(const char *path);

static int copy_dir_rec(const char *src, const char *dst) {
    char dent[DENT_BUF];
    char child_src[PATH_CAP];
    char child_dst[PATH_CAP];
    char names[MAX_NAMES][NAME_LEN];
    int nnames;
    long fd;
    long n;
    int i;
    int rc = 0;

    if (!path_exists(dst)) {
        if (u_mkdir(dst) < 0) {
            printf("mv: %s: cannot create directory\n", dst);
            return 1;
        }
    } else if (!is_dir(dst)) {
        printf("mv: %s: not a directory\n", dst);
        return 1;
    }

    fd = u_open(src, O_RDONLY, 0);
    if (fd < 0) {
        printf("mv: %s: cannot open directory\n", src);
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
                printf("mv: %s: too many entries\n", src);
                u_close(fd);
                return 1;
            }
            strcpy(names[nnames], name);
            nnames++;
        }
    }
    if (u_close(fd) < 0) return 1;

    for (i = 0; i < nnames; i++) {
        if (join_dir_base(child_src, PATH_CAP, src, names[i]) < 0
            || join_dir_base(child_dst, PATH_CAP, dst, names[i]) < 0) {
            u_write(2, "mv: path too long\n", 18);
            rc = 1;
            continue;
        }
        if (copy_path(child_src, child_dst) != 0) rc = 1;
    }
    return rc;
}

static int copy_path(const char *src, const char *dst) {
    char st[ST_SIZE_BYTES];
    unsigned int mode;
    if (u_lstat(src, st) < 0) return 1;
    mode = load_u32(st + ST_MODE) & S_IFMT;
    if (mode == S_IFLNK) return copy_link(src, dst);
    if (mode == S_IFDIR) return copy_dir_rec(src, dst);
    return copy_file(src, dst);
}

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
        printf("mv: %s: cannot open directory\n", path);
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
                printf("mv: %s: too many entries\n", path);
                u_close(fd);
                return 1;
            }
            strcpy(names[nnames], name);
            nnames++;
        }
    }
    if (u_close(fd) < 0) return 1;

    for (i = 0; i < nnames; i++) {
        if (join_dir_base(child, PATH_CAP, path, names[i]) < 0) {
            u_write(2, "mv: path too long\n", 18);
            rc = 1;
            continue;
        }
        if (rm_path(child) != 0) rc = 1;
    }
    if (u_rmdir(path) != 0) {
        printf("mv: %s: cannot remove directory\n", path);
        rc = 1;
    }
    return rc;
}

static int rm_path(const char *path) {
    char st[ST_SIZE_BYTES];
    if (u_lstat(path, st) < 0) return 1;
    if ((load_u32(st + ST_MODE) & S_IFMT) == S_IFDIR) {
        return rm_dir_rec(path);
    }
    if (u_unlink(path) != 0) {
        printf("mv: %s: cannot remove\n", path);
        return 1;
    }
    return 0;
}

static int copy_unlink(const char *src, const char *dst) {
    if (copy_file(src, dst) != 0) return 1;
    if (u_unlink(src) < 0) {
        printf("mv: %s: copied but unlink failed\n", src);
        return 1;
    }
    return 0;
}

static void announce(const char *src, const char *dst) {
    printf("'%s' -> '%s'\n", src, dst);
}

static int move_one(const char *src, const char *dst, int noclobber, int verbose) {
    int rc;
    char source_st[ST_SIZE_BYTES];
    unsigned int source_type;
    if (u_lstat(src, source_st) < 0) return 1;
    source_type = load_u32(source_st + ST_MODE) & S_IFMT;
    if (is_under(src, dst)) {
        printf("mv: %s: cannot move a directory into itself\n", src);
        return 1;
    }
    if (noclobber && path_exists(dst)) {
        return 0;
    }
    /* Guard even the rename path: aliases hide descendants from lexical
     * checks, and a failed rename must not become a recursive self-copy. */
    if (source_type == S_IFDIR && !directory_target_safe(src, dst)) {
        printf("mv: %s: cannot move a directory into itself\n", src);
        return 1;
    }
    if (u_rename(src, dst) == 0) {
        if (verbose) announce(src, dst);
        return 0;
    }
    /* EXDEV / unsupported: copy-tree then remove source. */
    if (source_type == S_IFLNK) {
        if (copy_link(src, dst) != 0) return 1;
        rc = rm_path(src); /* lstat: unlink only this link, never its target */
        if (rc == 0 && verbose) announce(src, dst);
        return rc;
    }
    if (source_type == S_IFDIR) {
        char dest_st[ST_SIZE_BYTES];
        /* A failed rename must not silently merge/overwrite an existing tree.
         * Cross-FS empty targets are also refused conservatively. */
        if (u_lstat(dst, dest_st) == 0) {
            printf("mv: %s: refusing directory merge after failed rename\n", dst);
            return 1;
        }
        if (copy_path(src, dst) != 0) return 1;
        rc = rm_path(src);
        if (rc == 0 && verbose) announce(src, dst);
        return rc;
    }
    rc = copy_unlink(src, dst);
    if (rc == 0 && verbose) announce(src, dst);
    return rc;
}

static void usage(void) {
    u_write(2, "usage: mv [-nv] SRC DST|DIR...\n", 32);
}

int main(int argc, char **argv) {
    int nops;
    int i;
    int ai = 1;
    int noclobber = 0;
    int verbose = 0;
    int rc = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) {
            break;
        }
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'n') {
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
                u_write(2, "mv: path too long\n", 18);
                return 1;
            }
            dst = joined;
        }
        return move_one(src, dst, noclobber, verbose);
    }

    /* Multiple sources into directory. */
    {
        const char *dir = argv[argc - 1];
        if (!is_dir(dir)) {
            printf("mv: %s: not a directory\n", dir);
            return 1;
        }
        for (i = ai; i < argc - 1; i++) {
            char joined[PATH_CAP];
            if (join_dir_base(joined, PATH_CAP, dir, basename_of(argv[i])) < 0) {
                u_write(2, "mv: path too long\n", 18);
                rc = 1;
                continue;
            }
            if (move_one(argv[i], joined, noclobber, verbose) != 0) rc = 1;
        }
    }
    return rc;
}
