/* fakeos userland - find: walk trees and print matching paths (thin).
 *   find [PATH...] [EXPRESSION]
 * Paths are argv before the first primary; default PATH is ".".
 * Primaries (AND): -name GLOB / -iname GLOB (basename *,? only; -iname
 * folds A-Z), -path GLOB (full path *,? only), -type f|d, -empty (empty
 * regular file or empty directory), -prune (do not descend into a matched
 * directory), -print / -print0 (NUL-terminated paths; pairs with xargs -0),
 * -maxdepth N, -mindepth N (start points are depth 0; GNU-ish). Uses
 * lstat(2). No -o/parens. Thin -prune: when predicates match a directory,
 * skip walk_dir. Depth-first. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern long u_lstat(const char *path, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    ST_MODE = 24,
    ST_SIZE = 48,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    S_IFREG = 0x8000,
    PATH_CAP = 128,
    DENT_BUF = 1920,
    DENT_REC = 48,
    NAME_OFF = 19,
    NAME_LEN = 30,
    MAX_NAMES = 128,
    MAX_PATHS = 8,
    GLOB_CAP = 64,
    DEPTH_UNLIM = 0x7FFFFFFF
};

static int want_name;
static int name_fold; /* 1 = -iname */
static char name_pat[GLOB_CAP];
static int want_path;
static char path_pat[GLOB_CAP];
static int want_type; /* 0=any, 1=file, 2=dir */
static int want_empty;
static int do_print = 1;
static int print_nul; /* -print0 */
static int want_prune;
static int max_depth = DEPTH_UNLIM;
static int min_depth = 0;
static int err_rc;

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

static int is_dir_mode(unsigned int mode) {
    return (mode & S_IFMT) == S_IFDIR;
}

static int is_reg_mode(unsigned int mode) {
    return (mode & S_IFMT) == S_IFREG;
}

/* Basename of path (pointer into path; never empty for non-empty path). */
static const char *base_of(const char *path) {
    const char *p = path;
    const char *last = path;
    if (path[0] == 0) return path;
    while (*p) {
        if (*p == '/' && p[1] != 0) last = p + 1;
        p++;
    }
    return last;
}

static char fold_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* Thin glob: only '*' and '?' meta; match whole string. fold=1 for -iname. */
static int glob_match(const char *pat, const char *s, int fold) {
    for (;;) {
        if (pat[0] == 0) return s[0] == 0;
        if (pat[0] == '*') {
            /* greedy-free: try skip 0..len of s */
            while (pat[0] == '*') pat++;
            if (pat[0] == 0) return 1;
            while (*s) {
                if (glob_match(pat, s, fold)) return 1;
                s++;
            }
            return 0;
        }
        if (pat[0] == '?') {
            if (s[0] == 0) return 0;
            pat++;
            s++;
            continue;
        }
        if (s[0] == 0) return 0;
        if (fold) {
            if (fold_ch(pat[0]) != fold_ch(s[0])) return 0;
        } else if (pat[0] != s[0]) {
            return 0;
        }
        pat++;
        s++;
    }
}

/* Directory empty: no getdents names other than . / .. */
static int dir_is_empty(const char *path) {
    char dent[DENT_BUF];
    long fd;
    long n;
    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
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
            u_close(fd);
            return 0;
        }
    }
    u_close(fd);
    return 1;
}

static int is_empty_path(const char *path, unsigned int mode, const char *st) {
    if (is_reg_mode(mode)) return load_u64(st + ST_SIZE) == 0;
    if (is_dir_mode(mode)) return dir_is_empty(path);
    return 0;
}

static int match_preds(const char *path, unsigned int mode, const char *st) {
    if (want_type == 1 && !is_reg_mode(mode)) return 0;
    if (want_type == 2 && !is_dir_mode(mode)) return 0;
    if (want_name && !glob_match(name_pat, base_of(path), name_fold)) return 0;
    if (want_path && !glob_match(path_pat, path, 0)) return 0;
    if (want_empty && !is_empty_path(path, mode, st)) return 0;
    return 1;
}

static void emit(const char *path) {
    unsigned long n;
    if (!do_print) return;
    n = strlen(path);
    if (n > 0) u_write(1, path, n);
    if (print_nul) {
        char z = 0;
        u_write(1, &z, 1);
    } else {
        u_write(1, "\n", 1);
    }
}

static void join_path(char *out, unsigned long cap, const char *dir,
                      const char *name) {
    unsigned long i = 0;
    unsigned long j = 0;
    if (cap < 2) {
        out[0] = 0;
        return;
    }
    /* Avoid "./foo" when dir is "." — keep relative clean for scripts. */
    if (dir[0] == '.' && dir[1] == 0) {
        while (name[j] != 0 && i + 1 < cap) out[i++] = name[j++];
        out[i] = 0;
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

static int is_primary(const char *a) {
    if (strcmp(a, "-name") == 0) return 1;
    if (strcmp(a, "-iname") == 0) return 1;
    if (strcmp(a, "-path") == 0) return 1;
    if (strcmp(a, "-type") == 0) return 1;
    if (strcmp(a, "-empty") == 0) return 1;
    if (strcmp(a, "-print") == 0) return 1;
    if (strcmp(a, "-print0") == 0) return 1;
    if (strcmp(a, "-prune") == 0) return 1;
    if (strcmp(a, "-maxdepth") == 0) return 1;
    if (strcmp(a, "-mindepth") == 0) return 1;
    return 0;
}

static int parse_depth(const char *s, int *out) {
    int v = 0;
    int i = 0;
    if (s == 0 || s[0] == 0) return 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    if (i == 0 || s[i] != 0) return 0;
    *out = v;
    return 1;
}

static void walk(const char *path, int depth);

static void walk_dir(const char *path, int depth) {
    char dent[DENT_BUF];
    char child[PATH_CAP];
    char names[MAX_NAMES][NAME_LEN];
    int nnames;
    long fd;
    long n;
    int i;

    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("find: %s: cannot open\n", path);
        err_rc = 1;
        return;
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
                printf("find: %s: too many entries\n", path);
                err_rc = 1;
                u_close(fd);
                return;
            }
            strcpy(names[nnames], name);
            nnames++;
        }
    }
    u_close(fd);

    for (i = 0; i < nnames; i++) {
        join_path(child, PATH_CAP, path, names[i]);
        if (child[0] == 0) continue;
        walk(child, depth + 1);
    }
}

static void walk(const char *path, int depth) {
    char st[ST_SIZE_BYTES];
    unsigned int mode;

    if (u_lstat(path, st) != 0) {
        printf("find: %s: no such file\n", path);
        err_rc = 1;
        return;
    }
    mode = load_u32(st + ST_MODE);
    if (depth >= min_depth && match_preds(path, mode, st)) emit(path);
    if (is_dir_mode(mode) && depth < max_depth) {
        /* Thin -prune: matched directory is not descended. */
        if (want_prune && match_preds(path, mode, st)) {
            /* skip */
        } else {
            walk_dir(path, depth);
        }
    }
}

int main(int argc, char **argv) {
    int ai = 1;
    const char *paths[MAX_PATHS];
    int npaths = 0;
    int i;

    /* Paths are leading non-primary args. */
    while (ai < argc && !is_primary(argv[ai])) {
        if (npaths >= MAX_PATHS) {
            u_write(2, "find: too many paths\n", 21);
            return 1;
        }
        paths[npaths++] = argv[ai];
        ai++;
    }
    if (npaths == 0) {
        paths[0] = ".";
        npaths = 1;
    }

    while (ai < argc) {
        if (strcmp(argv[ai], "-name") == 0 || strcmp(argv[ai], "-iname") == 0) {
            int iname = (argv[ai][1] == 'i');
            if (ai + 1 >= argc) {
                u_write(2, "find: -name needs GLOB\n", 23);
                return 1;
            }
            want_name = 1;
            name_fold = iname;
            if (strlen(argv[ai + 1]) >= GLOB_CAP) {
                u_write(2, "find: -name GLOB too long\n", 26);
                return 1;
            }
            strcpy(name_pat, argv[ai + 1]);
            ai += 2;
            continue;
        }
        if (strcmp(argv[ai], "-path") == 0) {
            if (ai + 1 >= argc) {
                u_write(2, "find: -path needs GLOB\n", 23);
                return 1;
            }
            want_path = 1;
            if (strlen(argv[ai + 1]) >= GLOB_CAP) {
                u_write(2, "find: -path GLOB too long\n", 26);
                return 1;
            }
            strcpy(path_pat, argv[ai + 1]);
            ai += 2;
            continue;
        }
        if (strcmp(argv[ai], "-type") == 0) {
            if (ai + 1 >= argc || (argv[ai + 1][0] != 'f' && argv[ai + 1][0] != 'd')
                || argv[ai + 1][1] != 0) {
                u_write(2, "find: -type needs f|d\n", 22);
                return 1;
            }
            want_type = (argv[ai + 1][0] == 'f') ? 1 : 2;
            ai += 2;
            continue;
        }
        if (strcmp(argv[ai], "-empty") == 0) {
            want_empty = 1;
            ai++;
            continue;
        }
        if (strcmp(argv[ai], "-print") == 0) {
            do_print = 1;
            print_nul = 0;
            ai++;
            continue;
        }
        if (strcmp(argv[ai], "-print0") == 0) {
            do_print = 1;
            print_nul = 1;
            ai++;
            continue;
        }
        if (strcmp(argv[ai], "-prune") == 0) {
            want_prune = 1;
            ai++;
            continue;
        }
        if (strcmp(argv[ai], "-maxdepth") == 0) {
            if (ai + 1 >= argc || !parse_depth(argv[ai + 1], &max_depth)) {
                u_write(2, "find: -maxdepth needs N\n", 24);
                return 1;
            }
            ai += 2;
            continue;
        }
        if (strcmp(argv[ai], "-mindepth") == 0) {
            if (ai + 1 >= argc || !parse_depth(argv[ai + 1], &min_depth)) {
                u_write(2, "find: -mindepth needs N\n", 24);
                return 1;
            }
            ai += 2;
            continue;
        }
        u_write(2, "usage: find [PATH...] [-name|-iname|-path GLOB] [-type f|d] [-empty] [-prune] [-maxdepth N] [-mindepth N] [-print|-print0]\n", 123);
        return 1;
    }

    err_rc = 0;
    for (i = 0; i < npaths; i++) walk(paths[i], 0);
    return err_rc;
}
