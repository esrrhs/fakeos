/* fakeos userland - chgrp: change file group (thin POSIX).
 *   chgrp [-v] GID FILE...
 * GID is decimal. Owner is left unchanged via chown(2) uid=(u32)-1.
 * -v prints changed group of 'PATH' from OLD to NEW (or retained as GID)
 * using decimal GID (GNU-ish; names unavailable). Compact argv for
 * EXEC_MAX_ARGC=8. External form for xargs/scripts (no shell builtin).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_chown(const char *path, long uid, long gid);
extern long u_lstat(const char *path, void *statbuf);
extern void printf(const char *fmt, ...);

enum {
    ST_GID = 32,
    ST_SIZE_BYTES = 144,
    OWNER_UNCHANGED = 0xFFFFFFFFUL
};

static int verbose;

static unsigned int load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
}

static void announce(const char *path, unsigned int oldg, unsigned int newg) {
    if (oldg == newg) {
        printf("group of '%s' retained as %u\n", path, newg);
    } else {
        printf("changed group of '%s' from %u to %u\n", path, oldg, newg);
    }
}

static void usage(void) {
    u_write(2, "usage: chgrp [-v] GID FILE...\n", 30);
}

int main(int argc, char **argv) {
    unsigned long gid = 0;
    const char *spec;
    long i = 0;
    int ai = 1;
    int rc = 0;

    verbose = 0;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) {
            ai++;
            break;
        }
        /* Digits are a GID operand, not an option. */
        if (o[0] >= '0' && o[0] <= '9') break;
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'v') {
                verbose = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    if (ai + 1 >= argc) {
        usage();
        return 1;
    }
    spec = argv[ai];
    ai++;
    if (spec[0] == 0) {
        u_write(2, "chgrp: bad group\n", 17);
        return 1;
    }
    while (spec[i] >= '0' && spec[i] <= '9') {
        gid = gid * 10 + (unsigned long)(spec[i] - '0');
        i++;
    }
    if (i == 0 || spec[i] != 0) {
        u_write(2, "chgrp: bad group\n", 17);
        return 1;
    }

    for (; ai < argc; ai++) {
        char st[ST_SIZE_BYTES];
        unsigned int oldg = 0;
        if (verbose) {
            if (u_lstat(argv[ai], st) != 0) {
                printf("chgrp: %s: failed\n", argv[ai]);
                rc = 1;
                continue;
            }
            oldg = load_u32(st + ST_GID);
        }
        /* uid (u32)-1 = leave owner unchanged */
        if (u_chown(argv[ai], (long)OWNER_UNCHANGED, (long)gid) != 0) {
            printf("chgrp: %s: failed\n", argv[ai]);
            rc = 1;
            continue;
        }
        if (verbose) {
            announce(argv[ai], oldg, (unsigned int)gid);
        }
    }
    return rc;
}
