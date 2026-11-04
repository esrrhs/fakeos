/* fakeos userland - tee: copy stdin to stdout and optional files (-a append). */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum {
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200,
    O_APPEND = 0x400,
    MAX_OUT  = 8
};

int main(int argc, char **argv) {
    long fds[MAX_OUT];
    int nfds = 0;
    int append = 0;
    int i = 1;
    int rc = 0;
    char buf[512];
    long n;
    long flags;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "-a") == 0) {
            append = 1;
            i++;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            u_write(2, "usage: tee [-a] [FILE...]\n", 26);
            return 1;
        }
    }

    flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    for (; i < argc; i++) {
        long fd;
        if (nfds >= MAX_OUT) {
            u_write(2, "tee: too many files\n", 20);
            rc = 1;
            break;
        }
        fd = u_open(argv[i], flags, 0x1A4);
        if (fd < 0) {
            printf("tee: %s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        fds[nfds] = fd;
        nfds++;
    }

    while ((n = u_read(0, buf, sizeof(buf))) > 0) {
        long w = u_write(1, buf, (unsigned long)n);
        if (w < 0) {
            rc = 1;
            break;
        }
        for (i = 0; i < nfds; i++) {
            if (u_write(fds[i], buf, (unsigned long)n) < 0) {
                rc = 1;
            }
        }
    }
    if (n < 0) rc = 1;

    for (i = 0; i < nfds; i++) {
        u_close(fds[i]);
    }
    return rc;
}
