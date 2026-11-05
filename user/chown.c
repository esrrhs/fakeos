/* fakeos userland - chown: change file owner/group (thin; mirrors shell builtin).
 *   chown [-v] OWNER[:GROUP] FILE...
 *   chown [-v] :GROUP FILE...
 * OWNER/GROUP are decimal. Missing GROUP leaves group unchanged ((u32)-1).
 * Leading ':' (group-only) leaves owner unchanged ((u32)-1).
 * -v prints changed ownership of 'PATH' from OLD to NEW (or retained as OWN)
 * using decimal UID:GID (GNU-ish; names unavailable). Compact argv for
 * EXEC_MAX_ARGC=8. External form for xargs/scripts.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_chown(const char *path, long uid, long gid);
extern long u_lstat(const char *path, void *statbuf);
extern void printf(const char *fmt, ...);

enum {
    ST_UID = 28,
    ST_GID = 32,
    ST_SIZE_BYTES = 144,
    UNCHANGED = 0xFFFFFFFFUL
};

static int verbose;

static unsigned int load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
}

static void announce(const char *path, unsigned int ou, unsigned int og,
                     unsigned int nu, unsigned int ng) {
    if (ou == nu && og == ng) {
        printf("ownership of '%s' retained as %u:%u\n", path, nu, ng);
    } else {
        printf("changed ownership of '%s' from %u:%u to %u:%u\n",
               path, ou, og, nu, ng);
    }
}

static void usage(void) {
    u_write(2, "usage: chown [-v] [UID][:GID] FILE...\n", 38);
}

int main(int argc, char **argv) {
    unsigned long uid = UNCHANGED;
    unsigned long gid = UNCHANGED;
    const char *spec;
    long i = 0;
    int ai = 1;
    int rc = 0;
    int have_uid = 0;

    verbose = 0;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) {
            ai++;
            break;
        }
        /* Negative UIDs are not supported; treat lone '-' digits as bad later. */
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

    if (spec[0] == ':') {
        long j = 1;
        gid = 0;
        if (spec[j] == 0) {
            u_write(2, "chown: bad group\n", 17);
            return 1;
        }
        while (spec[j] >= '0' && spec[j] <= '9') {
            gid = gid * 10 + (unsigned long)(spec[j] - '0');
            j++;
        }
        if (spec[j] != 0) {
            u_write(2, "chown: bad group\n", 17);
            return 1;
        }
    } else {
        while (spec[i] >= '0' && spec[i] <= '9') {
            if (!have_uid) {
                uid = 0;
                have_uid = 1;
            }
            uid = uid * 10 + (unsigned long)(spec[i] - '0');
            i++;
        }
        if (!have_uid) {
            u_write(2, "chown: bad owner\n", 17);
            return 1;
        }
        if (spec[i] == ':') {
            long j = i + 1;
            gid = 0;
            if (spec[j] == 0) {
                u_write(2, "chown: bad group\n", 17);
                return 1;
            }
            while (spec[j] >= '0' && spec[j] <= '9') {
                gid = gid * 10 + (unsigned long)(spec[j] - '0');
                j++;
            }
            if (spec[j] != 0) {
                u_write(2, "chown: bad group\n", 17);
                return 1;
            }
        } else if (spec[i] != 0) {
            u_write(2, "chown: bad owner\n", 17);
            return 1;
        }
    }

    for (; ai < argc; ai++) {
        char st[ST_SIZE_BYTES];
        unsigned int ou = 0;
        unsigned int og = 0;
        unsigned int nu;
        unsigned int ng;
        if (verbose) {
            if (u_lstat(argv[ai], st) != 0) {
                printf("chown: %s: failed\n", argv[ai]);
                rc = 1;
                continue;
            }
            ou = load_u32(st + ST_UID);
            og = load_u32(st + ST_GID);
        }
        if (u_chown(argv[ai], (long)uid, (long)gid) != 0) {
            printf("chown: %s: failed\n", argv[ai]);
            rc = 1;
            continue;
        }
        if (verbose) {
            nu = (uid == UNCHANGED) ? ou : (unsigned int)uid;
            ng = (gid == UNCHANGED) ? og : (unsigned int)gid;
            announce(argv[ai], ou, og, nu, ng);
        }
    }
    return rc;
}
