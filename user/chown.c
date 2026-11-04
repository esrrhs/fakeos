/* fakeos userland - chown: change file owner/group (thin; mirrors shell builtin).
 *   chown UID[:GID] FILE...
 * UID/GID are decimal. Missing GID leaves group unchanged ((u32)-1).
 * External form for xargs/scripts (shell builtin still covers interactive).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_chown(const char *path, long uid, long gid);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    unsigned long uid = 0;
    unsigned long gid = 0xFFFFFFFFUL; /* unchanged */
    const char *spec;
    long i = 0;
    int ai;
    int rc = 0;

    if (argc < 3) {
        u_write(2, "usage: chown UID[:GID] FILE...\n", 30);
        return 1;
    }
    spec = argv[1];
    while (spec[i] >= '0' && spec[i] <= '9') {
        uid = uid * 10 + (unsigned long)(spec[i] - '0');
        i++;
    }
    if (i == 0) {
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
    for (ai = 2; ai < argc; ai++) {
        if (u_chown(argv[ai], (long)uid, (long)gid) != 0) {
            printf("chown: %s: failed\n", argv[ai]);
            rc = 1;
        }
    }
    return rc;
}
