/* fakeos userland - env: print process environment (thin; mirrors shell).
 *   env
 * Reads /proc/self/environ (NUL-separated NAME=val) and prints one entry
 * per line. Reflects execve envp from the parent (shell export table).
 * External form for xargs/scripts that cannot use the shell builtin.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_close(long fd);

int main(int argc, char **argv) {
    char buf[256];
    long fd;
    long n;
    long i;
    long start;

    (void)argv;
    if (argc != 1) {
        u_write(2, "usage: env\n", 11);
        return 1;
    }
    fd = u_open("/proc/self/environ", 0, 0);
    if (fd < 0) {
        u_write(2, "env: open failed\n", 17);
        return 1;
    }
    n = u_read(fd, buf, sizeof(buf));
    u_close(fd);
    if (n < 0) {
        u_write(2, "env: read failed\n", 17);
        return 1;
    }
    if (n == 0) return 0;
    start = 0;
    for (i = 0; i < n; i++) {
        if (buf[i] == 0) {
            if (i > start) {
                u_write(1, buf + start, (unsigned long)(i - start));
                u_write(1, "\n", 1);
            }
            start = i + 1;
        }
    }
    if (start < n) {
        u_write(1, buf + start, (unsigned long)(n - start));
        u_write(1, "\n", 1);
    }
    return 0;
}
