/* fakeos userland - arch: print machine hardware name (thin; uname -m).
 * Forms: arch
 * Prints the utsname.machine field from uname(2). No options.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_uname(void *buf);
extern unsigned long strlen(const char *s);

enum { UTS_LEN = 65, UTS_SIZE = 390 };

int main(int argc, char **argv) {
    char uts[UTS_SIZE];
    long i;
    const char *mach;
    unsigned long n;

    if (argc > 1) {
        u_write(2, "usage: arch\n", 12);
        return 1;
    }
    for (i = 0; i < UTS_SIZE; i++) uts[i] = 0;
    if (u_uname(uts) < 0) {
        u_write(2, "arch: uname failed\n", 19);
        return 1;
    }
    /* machine is the fifth 65-byte field (index 4). */
    mach = uts + 4 * UTS_LEN;
    n = strlen(mach);
    if (n == 0) {
        u_write(1, "unknown\n", 8);
        return 0;
    }
    u_write(1, mach, n);
    u_write(1, "\n", 1);
    return 0;
}
