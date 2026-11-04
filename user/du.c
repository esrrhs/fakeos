/* fakeos userland - du: summarize file/directory space (thin).
 *   du [-a] [-s] [FILE...]
 * Apparent size via lstat st_size, reported in 1024-byte units (ceil).
 * Default: print each subdirectory total + top-level. -a also prints files.
 * -s: summary only (one line per operand). Default operand: ".".
 * No -h/-d/hardlink dedup. Compact argv for EXEC_MAX_ARGC=8.
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
    PATH_CAP = 128,
    DENT_BUF = 1920,
    DENT_REC = 48,
    NAME_OFF = 19,
    NAME_LEN = 30,
    MAX_NAMES = 128,
    MAX_ARGS = 8
};

static int opt_all;
static int opt_summarize;
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

static unsigned long to_kib(unsigned long bytes) {
    if (bytes == 0) return 0;
    return (bytes + 1023UL) / 1024UL;
}

static void join_path(char *out, unsigned long cap, const char *dir,
                      const char *name) {
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

static void emit(unsigned long bytes, const char *path) {
    printf("%u\t%s\n", (unsigned int)to_kib(bytes), path);
}

/* Return total apparent bytes under path; emit per policy. */
static unsigned long du_path(const char *path, int at_top);

static unsigned long du_dir(const char *path, unsigned long self_bytes, int at_top) {
    char dent[DENT_BUF];
    char child[PATH_CAP];
    char names[MAX_NAMES][NAME_LEN];
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
                printf("du: %s: too many entries\n", path);
                err_rc = 1;
                u_close(fd);
                return total;
            }
            strcpy(names[nnames], name);
            nnames++;
        }
    }
    u_close(fd);

    for (i = 0; i < nnames; i++) {
        join_path(child, PATH_CAP, path, names[i]);
        if (child[0] == 0) continue;
        total += du_path(child, 0);
    }

    if (!opt_summarize || at_top) emit(total, path);
    return total;
}

static unsigned long du_path(const char *path, int at_top) {
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
        return du_dir(path, sz, at_top);
    }
    if (opt_all || at_top) emit(sz, path);
    return sz;
}

int main(int argc, char **argv) {
    int ai = 1;
    const char *args[MAX_ARGS];
    int nargs = 0;
    int i;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) break; /* lone "-" operand */
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'a') {
                opt_all = 1;
            } else if (o[j] == 's') {
                opt_summarize = 1;
            } else {
                u_write(2, "usage: du [-a] [-s] [FILE...]\n", 30);
                return 1;
            }
        }
        ai++;
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
        (void)du_path(args[i], 1);
    }
    return err_rc;
}
