/* fakeos userland - tty: print controlling terminal path (thin POSIX).
 * Forms: tty [-s]
 * If stdin is a TTY (ioctl TCGETS), print /dev/tty; else "not a tty" (exit 1).
 * -s: silent (exit status only).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_ioctl(long fd, unsigned long req, unsigned long arg);
extern int strcmp(const char *a, const char *b);

enum { TCGETS = 0x5401 };

int main(int argc, char **argv) {
    char termios[64];
    int silent = 0;
    int i;
    long r;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            silent = 1;
        } else {
            u_write(2, "usage: tty [-s]\n", 16);
            return 2;
        }
    }

    /* termios blob size is ignored by kernel beyond copy; 64 matches Linux. */
    for (i = 0; i < 64; i++) termios[i] = 0;
    r = u_ioctl(0, (unsigned long)TCGETS, (unsigned long)termios);
    if (r == 0) {
        if (!silent) {
            u_write(1, "/dev/tty\n", 9);
        }
        return 0;
    }
    if (!silent) {
        u_write(1, "not a tty\n", 10);
    }
    return 1;
}
