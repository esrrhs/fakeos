/* fakeos userland - cat: concatenate files named on the command line. */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    char buf[1024];
    int i;

    if (argc < 2) {
        u_write(2, "usage: cat FILE...\n", 18);
        return 1;
    }
    for (i = 1; i < argc; i++) {
        long fd = u_open(argv[i], 0, 0);
        long n;
        if (fd < 0) {
            printf("cat: %s: no such file\n", argv[i]);
            return 1;
        }
        while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
            u_write(1, buf, (u64)n);
        }
        u_close(fd);
    }
    return 0;
}
