/* fakeos userland - rm: unlink files. */
package user;

extern long u_unlink(const char *path);
extern long u_write(long fd, const void *buf, unsigned long n);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    int i;
    int rc = 0;
    if (argc < 2) {
        u_write(2, "usage: rm FILE...\n", 17);
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (u_unlink(argv[i]) != 0) {
            printf("rm: %s: cannot remove\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
