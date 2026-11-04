/* fakeos userland - truncate: set file size via ftruncate(2) (thin).
 *   truncate [-c] -s SIZE FILE...
 * SIZE is a non-negative decimal integer; optional k/K (=1024), b/B (=512),
 * or M (=1024*1024) suffix (same as dd). Missing files are created (0644)
 * unless -c (GNU --no-create): then missing paths are skipped (exit 0).
 * Uses open(2) + ftruncate(2); Ext2/Ramfs already implement ftruncate.
 * Compact argv for EXEC_MAX_ARGC=8 (-c before -s).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_ftruncate(long fd, unsigned long length);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum {
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    MODE_0644 = 0x1A4
};

static int opt_c; /* -c: do not create missing files */

static void usage(void) {
    u_write(2, "usage: truncate [-c] -s SIZE FILE...\n", 37);
}

static int parse_size(const char *s, unsigned long *out) {
    unsigned long v = 0;
    int i = 0;
    char c;
    if (s == 0 || s[0] == 0) return 0;
    if (s[0] == '+') {
        i = 1;
        if (s[i] == 0) return 0;
    }
    if (s[i] < '0' || s[i] > '9') return 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (unsigned long)(s[i] - '0');
        i++;
    }
    c = s[i];
    if (c == 'k' || c == 'K') {
        v = v * 1024;
        i++;
    } else if (c == 'b' || c == 'B') {
        v = v * 512;
        i++;
    } else if (c == 'M') {
        v = v * 1024 * 1024;
        i++;
    }
    if (s[i] != 0) return 0;
    *out = v;
    return 1;
}

int main(int argc, char **argv) {
    unsigned long size = 0;
    int i = 1;
    int rc = 0;
    long flags;

    opt_c = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-c") == 0) {
            opt_c = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-s") == 0) {
            break;
        }
        usage();
        return 1;
    }
    if (i + 2 >= argc || strcmp(argv[i], "-s") != 0) {
        usage();
        return 1;
    }
    if (!parse_size(argv[i + 1], &size)) {
        u_write(2, "truncate: invalid size\n", 23);
        return 1;
    }
    i += 2;
    flags = O_WRONLY;
    if (!opt_c) flags = flags | O_CREAT;
    for (; i < argc; i++) {
        long fd = u_open(argv[i], flags, MODE_0644);
        if (fd < 0) {
            if (opt_c) {
                /* GNU --no-create: missing is OK, leave size alone. */
                continue;
            }
            printf("truncate: %s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        if (u_ftruncate(fd, size) < 0) {
            printf("truncate: %s: ftruncate failed\n", argv[i]);
            u_close(fd);
            rc = 1;
            continue;
        }
        u_close(fd);
    }
    return rc;
}
