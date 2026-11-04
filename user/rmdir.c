/* fakeos userland - rmdir: remove empty directories (thin shell/POSIX mirror).
 * Forms: rmdir DIR...
 * External form for xargs/scripts that exec rmdir without the shell builtin.
 * No -p (parents) yet — keep thin.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_rmdir(const char *path);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    int i;
    int rc = 0;

    if (argc < 2) {
        u_write(2, "usage: rmdir DIR...\n", 19);
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (u_rmdir(argv[i]) < 0) {
            printf("rmdir: %s: cannot remove directory\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
