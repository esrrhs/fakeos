/* fakeos userland - chmod: change file mode bits (thin; mirrors shell builtin).
 *   chmod MODE FILE...
 * MODE is an octal integer (e.g. 755, 0644). External form for xargs/scripts.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_chmod(const char *path, long mode);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    unsigned int mode = 0;
    const char *s;
    int j;
    int ai;
    int rc = 0;

    if (argc < 3) {
        u_write(2, "usage: chmod MODE FILE...\n", 26);
        return 1;
    }
    s = argv[1];
    if (s[0] == 0) {
        u_write(2, "chmod: bad mode\n", 16);
        return 1;
    }
    for (j = 0; s[j] != 0; j++) {
        if (s[j] < '0' || s[j] > '7') {
            u_write(2, "chmod: bad mode\n", 16);
            return 1;
        }
        mode = (mode << 3) | (unsigned int)(s[j] - '0');
    }
    for (ai = 2; ai < argc; ai++) {
        if (u_chmod(argv[ai], (long)mode) != 0) {
            printf("chmod: %s: failed\n", argv[ai]);
            rc = 1;
        }
    }
    return rc;
}
