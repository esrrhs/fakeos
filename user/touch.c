/* fakeos userland - touch: create empty file or update by open/close. */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_write(long fd, const void *buf, unsigned long n);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR   = 2,
    O_CREAT  = 0x40
};

int main(int argc, char **argv) {
    int i;
    if (argc < 2) {
        u_write(2, "usage: touch FILE...\n", 20);
        return 1;
    }
    for (i = 1; i < argc; i++) {
        long fd = u_open(argv[i], O_WRONLY | O_CREAT, 0x1A4);
        if (fd < 0) {
            /* Try read-only open if create failed on existing specials. */
            fd = u_open(argv[i], O_RDONLY, 0);
        }
        if (fd < 0) {
            printf("touch: %s: cannot touch\n", argv[i]);
            return 1;
        }
        u_close(fd);
    }
    return 0;
}
