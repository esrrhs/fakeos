/* fakeos userland - uname: print system information (thin; mirrors shell).
 *   uname [-a]
 * Default: sysname. -a prints sysname nodename release version machine.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_uname(void *buf);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum { UTS_LEN = 65, UTS_SIZE = 390 };

int main(int argc, char **argv) {
    char uts[UTS_SIZE];
    long i;
    int all = 0;

    if (argc > 2) {
        u_write(2, "usage: uname [-a]\n", 18);
        return 1;
    }
    if (argc == 2) {
        if (strcmp(argv[1], "-a") != 0) {
            u_write(2, "usage: uname [-a]\n", 18);
            return 1;
        }
        all = 1;
    }
    for (i = 0; i < UTS_SIZE; i++) uts[i] = 0;
    if (u_uname(uts) < 0) {
        u_write(2, "uname: failed\n", 14);
        return 1;
    }
    if (all) {
        printf("%s %s %s %s %s\n",
               uts, uts + UTS_LEN, uts + 2 * UTS_LEN,
               uts + 3 * UTS_LEN, uts + 4 * UTS_LEN);
    } else {
        printf("%s\n", uts);
    }
    return 0;
}
