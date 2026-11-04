/* fakeos userland - chown: change file owner/group (thin; mirrors shell builtin).
 *   chown OWNER[:GROUP] FILE...
 *   chown :GROUP FILE...
 * OWNER/GROUP are decimal. Missing GROUP leaves group unchanged ((u32)-1).
 * Leading ':' (group-only) leaves owner unchanged ((u32)-1).
 * External form for xargs/scripts (shell builtin still covers interactive).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_chown(const char *path, long uid, long gid);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    unsigned long uid = 0xFFFFFFFFUL; /* unchanged unless digits seen */
    unsigned long gid = 0xFFFFFFFFUL; /* unchanged */
    const char *spec;
    long i = 0;
    int ai;
    int rc = 0;
    int have_uid = 0;

    if (argc < 3) {
        u_write(2, "usage: chown [UID][:GID] FILE...\n", 33);
        return 1;
    }
    spec = argv[1];
    if (spec[0] == ':') {
        /* group-only: :GID */
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
    for (ai = 2; ai < argc; ai++) {
        if (u_chown(argv[ai], (long)uid, (long)gid) != 0) {
            printf("chown: %s: failed\n", argv[ai]);
            rc = 1;
        }
    }
    return rc;
}
