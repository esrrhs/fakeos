/* fakeos userland - du: summarize file/directory space (thin).
 *   du [-a] [-s] [-h] [-d N|-dN] [FILE...]
 * Apparent size via lstat st_size. Default: 1024-byte units (ceil).
 * -h: human-readable (integer 1024 powers: bare / K / M / G).
 * Default: print each subdirectory total + top-level. -a also prints files.
 * -s: summary only (one line per operand; same as -d 0).
 * -d N: print totals only for depth <= N below the operand (GNU-ish).
 * Default operand: ".". No hardlink dedup. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern void *malloc(unsigned long n);
extern void free(void *p);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern long u_dirent_reclen(const char *p, unsigned long avail);
extern long u_lstat(const char *path, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);
extern long strtol_dec(const char *s);

enum {
    O_RDONLY = 0,
    ST_MODE = 24,
    ST_SIZE = 48,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    PATH_CAP = 512,
    DENT_BUF = 1920,
    NAME_OFF = 19,
    NAME_LEN = 256,
    MAX_NAMES = 128,
    MAX_ARGS = 8
};

static int opt_all;
static int opt_summarize;
static int opt_human;
static int opt_depth; /* -1 = unlimited; 0+ = max depth to emit */
static int have_depth;
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

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static unsigned long to_kib(unsigned long bytes) {
    if (bytes == 0) return 0;
    return (bytes + 1023UL) / 1024UL;
}

static void join_path(char *out, unsigned long cap, const char *dir,
                      const char *name) {
    unsigned long dl = strlen(dir);
    unsigned long nl = strlen(name);
    unsigned long need = dl + nl + 1 + (dl && dir[dl - 1] != '/' ? 1 : 0);
    if (dir[0] == '.' && dir[1] == 0) need = nl + 1;
    if (need > cap) { if (cap) out[0] = 0; return; }
    unsigned long i = 0;
    unsigned long j = 0;
    if (cap < 2) {
        out[0] = 0;
        return;
    }
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

static void emit_human(unsigned long bytes, const char *path) {
    unsigned long v;
    char suf;
    /* Integer 1024 powers; no decimals (thin). */
    if (bytes < 1024UL) {
        printf("%u\t%s\n", (unsigned int)bytes, path);
        return;
    }
    if (bytes < 1024UL * 1024UL) {
        v = (bytes + 1023UL) / 1024UL;
        suf = 'K';
    } else if (bytes < 1024UL * 1024UL * 1024UL) {
        v = (bytes + 1024UL * 1024UL - 1UL) / (1024UL * 1024UL);
        suf = 'M';
    } else {
        v = (bytes + 1024UL * 1024UL * 1024UL - 1UL) / (1024UL * 1024UL * 1024UL);
        suf = 'G';
    }
    printf("%u%c\t%s\n", (unsigned int)v, suf, path);
}

static void emit(unsigned long bytes, const char *path) {
    if (opt_human) {
        emit_human(bytes, path);
        return;
    }
    printf("%u\t%s\n", (unsigned int)to_kib(bytes), path);
}

/* Whether to print this node at depth (0 = operand). */
static int emit_ok(int depth) {
    if (opt_summarize) return depth == 0;
    if (have_depth) return depth <= opt_depth;
    return 1;
}

/* Return total apparent bytes under path; emit per policy. */
static unsigned long du_path(const char *path, int depth);

static unsigned long du_dir(const char *path, unsigned long self_bytes, int depth) {
    char dent[DENT_BUF];
    char child[PATH_CAP];
    char *names;
    int nnames;
    long fd;
    long n;
    int i;
    unsigned long total = self_bytes;

    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("du: %s: cannot open\n", path);
        err_rc = 1;
        return self_bytes;
    }
    names = (char *)malloc(MAX_NAMES * NAME_LEN);
    if (!names) { err_rc = 1; u_close(fd); return total; }
    nnames = 0;
    for (;;) {
        long off;
        n = u_getdents(fd, dent, sizeof(dent));
        if (n == -4) continue;
        if (n < 0) { err_rc = 1; u_close(fd); free(names); return total; }
        if (n == 0) break;
        long reclen;
        for (off = 0; off < n; off += reclen) {
            reclen = u_dirent_reclen(dent + off, n - off);
            if (reclen < 0) { err_rc = 1; u_close(fd); free(names); return total; }
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
                printf("du: %s: too many entries\n", path);
                err_rc = 1;
                u_close(fd);
                free(names);
                return total;
            }
            strcpy((names + nnames * NAME_LEN), name);
            nnames++;
        }
    }
    u_close(fd);

    for (i = 0; i < nnames; i++) {
        join_path(child, PATH_CAP, path, (names + i * NAME_LEN));
        if (child[0] == 0) { err_rc = 1; continue; }
        total += du_path(child, depth + 1);
    }

    if (emit_ok(depth)) emit(total, path);
    free(names);
    return total;
}

static unsigned long du_path(const char *path, int depth) {
    char st[ST_SIZE_BYTES];
    unsigned int mode;
    unsigned long sz;

    if (u_lstat(path, st) != 0) {
        printf("du: %s: no such file\n", path);
        err_rc = 1;
        return 0;
    }
    mode = load_u32(st + ST_MODE);
    sz = load_u64(st + ST_SIZE);
    if (is_dir_mode(mode)) {
        return du_dir(path, sz, depth);
    }
    if ((opt_all || depth == 0) && emit_ok(depth)) emit(sz, path);
    return sz;
}

static void usage(void) {
    u_write(2, "usage: du [-a] [-s] [-h] [-d N] [FILE...]\n", 42);
}

int main(int argc, char **argv) {
    int ai = 1;
    const char *args[MAX_ARGS];
    int nargs = 0;
    int i;

    opt_all = 0;
    opt_summarize = 0;
    opt_human = 0;
    opt_depth = -1;
    have_depth = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *a = argv[ai];
        int j;
        if (a[1] == '-' && a[2] == 0) {
            ai++;
            break;
        }
        if (strcmp(a, "-") == 0) break;
        if (strcmp(a, "-d") == 0) {
            if (ai + 1 >= argc || !is_digits(argv[ai + 1])) {
                usage();
                return 1;
            }
            opt_depth = (int)strtol_dec(argv[ai + 1]);
            have_depth = 1;
            opt_summarize = 0;
            ai += 2;
            continue;
        }
        if (a[1] == 'd' && a[2] != 0 && is_digits(a + 2)) {
            opt_depth = (int)strtol_dec(a + 2);
            have_depth = 1;
            opt_summarize = 0;
            ai++;
            continue;
        }
        for (j = 1; a[j] != 0; j++) {
            if (a[j] == 'a') {
                opt_all = 1;
            } else if (a[j] == 's') {
                opt_summarize = 1;
                have_depth = 0;
            } else if (a[j] == 'h') {
                opt_human = 1;
            } else if (a[j] == 'd') {
                /* -d at end of cluster needs next argv */
                if (a[j + 1] != 0) {
                    if (!is_digits(a + j + 1)) {
                        usage();
                        return 1;
                    }
                    opt_depth = (int)strtol_dec(a + j + 1);
                    have_depth = 1;
                    opt_summarize = 0;
                    ai++;
                    goto next_arg;
                }
                if (ai + 1 >= argc || !is_digits(argv[ai + 1])) {
                    usage();
                    return 1;
                }
                opt_depth = (int)strtol_dec(argv[ai + 1]);
                have_depth = 1;
                opt_summarize = 0;
                ai += 2;
                goto next_arg;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    next_arg:
        ;
    }
    for (; ai < argc; ai++) {
        if (nargs >= MAX_ARGS) {
            u_write(2, "du: too many operands\n", 22);
            return 1;
        }
        args[nargs++] = argv[ai];
    }
    if (nargs == 0) {
        args[0] = ".";
        nargs = 1;
    }

    err_rc = 0;
    for (i = 0; i < nargs; i++) {
        (void)du_path(args[i], 0);
    }
    return err_rc;
}
