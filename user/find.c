/* fakeos userland - find: walk trees and print matching paths (thin).
 *   find [PATH...] [EXPRESSION]
 * Paths are argv before the first primary; default PATH is ".".
 * Primaries (AND): -name GLOB (basename *,? only), -type f|d, -print.
 * Uses lstat(2). No -o/parens/actions beyond print. Depth-first.
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
    GLOB_CAP = 64
};

static int want_name;
static char name_pat[GLOB_CAP];
static int want_type; /* 0=any, 1=file, 2=dir */
static int do_print = 1;
static int err_rc;

static unsigned int load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
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

/* Thin glob: only '*' and '?' meta; match whole string. */
static int glob_match(const char *pat, const char *s) {
    for (;;) {
        if (pat[0] == 0) return s[0] == 0;
        if (pat[0] == '*') {
            /* greedy-free: try skip 0..len of s */
            while (pat[0] == '*') pat++;
            if (pat[0] == 0) return 1;
            while (*s) {
                if (glob_match(pat, s)) return 1;
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
        if (s[0] == 0 || pat[0] != s[0]) return 0;
        pat++;
        s++;
    }
}

static int match_preds(const char *path, unsigned int mode) {
    if (want_type == 1 && !is_reg_mode(mode)) return 0;
    if (want_type == 2 && !is_dir_mode(mode)) return 0;
    if (want_name && !glob_match(name_pat, base_of(path))) return 0;
    return 1;
}

static void emit(const char *path) {
    if (!do_print) return;
    printf("%s\n", path);
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
    if (strcmp(a, "-type") == 0) return 1;
    if (strcmp(a, "-print") == 0) return 1;
    return 0;
}

static void walk(const char *path);

static void walk_dir(const char *path) {
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
        walk(child);
    }
}

static void walk(const char *path) {
    char st[ST_SIZE_BYTES];
    unsigned int mode;

    if (u_lstat(path, st) != 0) {
        printf("find: %s: no such file\n", path);
        err_rc = 1;
        return;
    }
    mode = load_u32(st + ST_MODE);
    if (match_preds(path, mode)) emit(path);
    if (is_dir_mode(mode)) walk_dir(path);
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
        if (strcmp(argv[ai], "-name") == 0) {
            if (ai + 1 >= argc) {
                u_write(2, "find: -name needs GLOB\n", 23);
                return 1;
            }
            want_name = 1;
            if (strlen(argv[ai + 1]) >= GLOB_CAP) {
                u_write(2, "find: -name GLOB too long\n", 26);
                return 1;
            }
            strcpy(name_pat, argv[ai + 1]);
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
        if (strcmp(argv[ai], "-print") == 0) {
            do_print = 1;
            ai++;
            continue;
        }
        u_write(2, "usage: find [PATH...] [-name GLOB] [-type f|d] [-print]\n", 56);
        return 1;
    }

    err_rc = 0;
    for (i = 0; i < npaths; i++) walk(paths[i]);
    return err_rc;
}
