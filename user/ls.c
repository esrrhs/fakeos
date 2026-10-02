/* fakeos userland - ls: list one directory (default: current directory).
 * Parses the kernel's fixed 48-byte dirent records (name at offset 19). */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getdents(long fd, void *buf, unsigned long len);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    char buf[1920];
    long fd;
    long n;

    fd = u_open(path, 0, 0);
    if (fd < 0) {
        printf("ls: %s: no such directory\n", path);
        return 1;
    }
    for (;;) {
        long off;
        n = u_getdents(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        for (off = 0; off + 48 <= n; off += 48) {
            char name[32];
            unsigned long k = 0;
            while (k < 29 && buf[off + 19 + k] != 0) {
                name[k] = buf[off + 19 + k];
                k++;
            }
            name[k] = 0;
            if (buf[off + 18] == 4) {
                name[k++] = '/';
                name[k] = 0;
            }
            u_write(1, name, k);
            u_write(1, "\n", 1);
        }
    }
    u_close(fd);
    return 0;
}
