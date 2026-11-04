/* fakeos userland - readlink: print symlink target (thin; mirrors shell).
 *   readlink PATH
 * Uses readlink(2). External form for xargs/scripts that exec readlink.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_readlink(const char *path, char *buf, unsigned long n);

int main(int argc, char **argv) {
    char buf[128];
    long n;

    if (argc != 2) {
        u_write(2, "usage: readlink PATH\n", 21);
        return 1;
    }
    n = u_readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        u_write(2, "readlink: failed\n", 17);
        return 1;
    }
    buf[n] = 0;
    u_write(1, buf, (unsigned long)n);
    u_write(1, "\n", 1);
    return 0;
}
