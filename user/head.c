/* fakeos userland - head: print the first N lines of files (default 10). */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);
extern void printf(const char *fmt, ...);

static int head_fd(long fd, long limit) {
    char buf[512];
    long n;
    long left = limit;
    int i;

    if (left < 0) left = 0;
    while (left > 0 && (n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            if (u_write(1, &buf[i], 1) < 0) return 1;
            if (buf[i] == '\n') {
                left--;
                if (left <= 0) return 0;
            }
        }
    }
    return n < 0 ? 1 : 0;
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    long limit = 10;
    int i = 1;
    int rc = 0;
    int files = 0;

    if (i < argc && strcmp(argv[i], "-n") == 0) {
        if (i + 1 >= argc || !is_digits(argv[i + 1])) {
            u_write(2, "usage: head [-n N] [FILE...]\n", 29);
            return 1;
        }
        limit = strtol_dec(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && is_digits(argv[i] + 1)) {
        limit = strtol_dec(argv[i] + 1);
        i++;
    }

    if (i >= argc) {
        return head_fd(0, limit);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (head_fd(0, limit) != 0) rc = 1;
            files++;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            printf("head: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (files > 0 || i + 1 < argc) {
            if (files > 0) u_write(1, "\n", 1);
            printf("==> %s <==\n", argv[i]);
        }
        if (head_fd(fd, limit) != 0) rc = 1;
        u_close(fd);
        files++;
    }
    return rc;
}
