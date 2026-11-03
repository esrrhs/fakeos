/* fakeos userland - cat: concatenate files, or stdin when none given. */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

static int cat_fd(long fd) {
    char buf[1024];
    long n;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        if (u_write(1, buf, (u64)n) < 0) {
            return 1;
        }
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i;
    int rc = 0;

    if (argc < 2) {
        return cat_fd(0);
    }
    for (i = 1; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (cat_fd(0) != 0) {
                rc = 1;
            }
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            printf("cat: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (cat_fd(fd) != 0) {
            rc = 1;
        }
        u_close(fd);
    }
    return rc;
}
