/* fakeos userland - mv: copy then unlink (same as rename for now). */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_unlink(const char *path);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200
};

int main(int argc, char **argv) {
    char buf[1024];
    long in;
    long out;
    long n;

    if (argc != 3) {
        u_write(2, "usage: mv SRC DST\n", 17);
        return 1;
    }
    in = u_open(argv[1], O_RDONLY, 0);
    if (in < 0) {
        printf("mv: %s: cannot open\n", argv[1]);
        return 1;
    }
    out = u_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0x1A4);
    if (out < 0) {
        printf("mv: %s: cannot create\n", argv[2]);
        u_close(in);
        return 1;
    }
    while ((n = u_read(in, buf, 1024)) > 0) {
        if (u_write(out, buf, (unsigned long)n) != n) {
            printf("mv: write error\n");
            u_close(in);
            u_close(out);
            return 1;
        }
    }
    u_close(in);
    u_close(out);
    if (u_unlink(argv[1]) < 0) {
        printf("mv: %s: copied but unlink failed\n", argv[1]);
        return 1;
    }
    return 0;
}
