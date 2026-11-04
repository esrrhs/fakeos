/* fakeos userland - mktemp: create unique temp file/dir (thin POSIX subset).
 * Forms: mktemp [-d] [-u] [TEMPLATE]
 * TEMPLATE must end in XXXXXX (replaced). Default /tmp/tmp.XXXXXX.
 * -d make directory; -u dry-run (print name, do not keep created object).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_unlink(const char *path);
extern long u_mkdir(const char *path);
extern long u_rmdir(const char *path);
extern long u_getpid(void);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);

enum {
    O_RDWR  = 2,
    O_CREAT = 0x40,
    O_EXCL  = 0x080,
    MODE_0600 = 0x180,
    PATH_MAX = 128,
    MAX_TRIES = 64
};

static const char alphabet[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static void usage(void) {
    u_write(2, "usage: mktemp [-d] [-u] [TEMPLATE]\n", 35);
}

static int ends_with_xxxxxx(const char *s, unsigned long n) {
    if (n < 6) return 0;
    return s[n - 6] == 'X' && s[n - 5] == 'X' && s[n - 4] == 'X'
        && s[n - 3] == 'X' && s[n - 2] == 'X' && s[n - 1] == 'X';
}

static void fill_xxxxxx(char *path, unsigned long n, unsigned long seed) {
    int i;
    unsigned long x = seed;
    for (i = 0; i < 6; i++) {
        x = x * 1103515245u + 12345u;
        path[n - 6 + i] = alphabet[(x >> 16) % 62];
    }
}

int main(int argc, char **argv) {
    int i = 1;
    int do_dir = 0;
    int dry = 0;
    const char *tmpl = "/tmp/tmp.XXXXXX";
    char path[PATH_MAX];
    unsigned long n;
    unsigned long seed;
    int try;
    long pid;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        int j;
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[1] == '-' && a[2] == 0) {
            i++;
            break;
        }
        /* allow glued -du / -ud */
        for (j = 1; a[j]; j++) {
            if (a[j] == 'd') do_dir = 1;
            else if (a[j] == 'u') dry = 1;
            else { usage(); return 1; }
        }
        i++;
    }
    if (i < argc) {
        tmpl = argv[i];
        i++;
    }
    if (i != argc) { usage(); return 1; }

    n = strlen(tmpl);
    if (n >= PATH_MAX || !ends_with_xxxxxx(tmpl, n)) {
        u_write(2, "mktemp: invalid template\n", 25);
        return 1;
    }
    strcpy(path, tmpl);

    pid = u_getpid();
    seed = (unsigned long)pid * 2654435761u + 7u;

    for (try = 0; try < MAX_TRIES; try++) {
        fill_xxxxxx(path, n, seed + (unsigned long)try * 97u);
        if (do_dir) {
            if (u_mkdir(path) == 0) {
                if (dry) {
                    u_rmdir(path);
                }
                u_write(1, path, strlen(path));
                u_write(1, "\n", 1);
                return 0;
            }
        } else {
            long fd = u_open(path, O_RDWR | O_CREAT | O_EXCL, MODE_0600);
            if (fd >= 0) {
                u_close(fd);
                if (dry) {
                    u_unlink(path);
                }
                u_write(1, path, strlen(path));
                u_write(1, "\n", 1);
                return 0;
            }
        }
    }
    u_write(2, "mktemp: failed\n", 15);
    return 1;
}
