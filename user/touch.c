/* fakeos userland - touch: create empty file or update by open/close.
 * Forms: touch [-c] FILE...
 * -c: do not create missing files (POSIX); missing paths are skipped OK.
 * No utimensat(2) yet — existing files are opened and closed.
 */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_write(long fd, const void *buf, unsigned long n);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR   = 2,
    O_CREAT  = 0x40
};

static void usage(void) {
    u_write(2, "usage: touch [-c] FILE...\n", 26);
}

int main(int argc, char **argv) {
    int i;
    int ai = 1;
    int no_create = 0;
    int rc = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) {
            ai++;
            break;
        }
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'c') {
                no_create = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }

    if (ai >= argc) {
        usage();
        return 1;
    }

    for (i = ai; i < argc; i++) {
        long fd;
        if (no_create) {
            fd = u_open(argv[i], O_WRONLY, 0);
            if (fd < 0) {
                fd = u_open(argv[i], O_RDONLY, 0);
            }
            if (fd < 0) {
                /* Missing: POSIX -c is success without creating. */
                continue;
            }
            u_close(fd);
            continue;
        }
        fd = u_open(argv[i], O_WRONLY | O_CREAT, 0x1A4);
        if (fd < 0) {
            /* Try read-only open if create failed on existing specials. */
            fd = u_open(argv[i], O_RDONLY, 0);
        }
        if (fd < 0) {
            printf("touch: %s: cannot touch\n", argv[i]);
            rc = 1;
            continue;
        }
        u_close(fd);
    }
    return rc;
}
