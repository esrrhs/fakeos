/* fakeos userland - chgrp: change file group (thin POSIX).
 *   chgrp GID FILE...
 * GID is decimal. Owner is left unchanged via chown(2) uid=(u32)-1.
 * External form for xargs/scripts (no shell builtin yet).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_chown(const char *path, long uid, long gid);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    unsigned long gid = 0;
    const char *spec;
    long i = 0;
    int ai;
    int rc = 0;

    if (argc < 3) {
        u_write(2, "usage: chgrp GID FILE...\n", 25);
        return 1;
    }
    spec = argv[1];
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
    for (ai = 2; ai < argc; ai++) {
        /* uid (u32)-1 = leave owner unchanged */
        if (u_chown(argv[ai], (long)0xFFFFFFFFUL, (long)gid) != 0) {
            printf("chgrp: %s: failed\n", argv[ai]);
            rc = 1;
        }
    }
    return rc;
}
