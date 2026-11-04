/* fakeos userland - unlink: remove directory entries (thin POSIX).
 * Forms: unlink FILE...
 * Calls unlink(2); refuses directories (use rmdir). For scripts/xargs that
 * want precise file removal without rm's recursive options.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_unlink(const char *path);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    int i;
    int rc = 0;

    if (argc < 2) {
        u_write(2, "usage: unlink FILE...\n", 22);
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (u_unlink(argv[i]) < 0) {
            printf("unlink: %s: cannot unlink\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
