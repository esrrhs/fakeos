/* fakeos userland - mv: rename(2); falls back to copy+unlink across FS. */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_unlink(const char *path);
extern long u_rename(const char *oldpath, const char *newpath);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200
};

static int copy_unlink(const char *src, const char *dst) {
    char buf[1024];
    long in;
    long out;
    long n;

    in = u_open(src, O_RDONLY, 0);
    if (in < 0) {
        printf("mv: %s: cannot open\n", src);
        return 1;
    }
    out = u_open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0x1A4);
    if (out < 0) {
        printf("mv: %s: cannot create\n", dst);
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
    if (u_unlink(src) < 0) {
        printf("mv: %s: copied but unlink failed\n", src);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        u_write(2, "usage: mv SRC DST\n", 17);
        return 1;
    }
    if (u_rename(argv[1], argv[2]) == 0) {
        return 0;
    }
    /* EXDEV / unsupported: preserve prior copy+unlink behavior. */
    return copy_unlink(argv[1], argv[2]);
}
