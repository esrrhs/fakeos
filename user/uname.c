/* fakeos userland - uname: print system information (thin; mirrors shell).
 * Forms:
 *   uname
 *   uname [-a] [-snrvm]   (glued short opts OK)
 * Default: sysname. -a prints all five fields. Multiple flags print
 * space-separated in POSIX order: sysname nodename release version machine.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_uname(void *buf);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum { UTS_LEN = 65, UTS_SIZE = 390 };

static void usage(void) {
    u_write(2, "usage: uname [-asnrvm]\n", 24);
}

int main(int argc, char **argv) {
    char uts[UTS_SIZE];
    long i;
    int ai = 1;
    int want_s = 0;
    int want_n = 0;
    int want_r = 0;
    int want_v = 0;
    int want_m = 0;
    int any = 0;
    int first = 1;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) break;
        if (strcmp(argv[ai], "--") == 0) {
            ai++;
            break;
        }
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'a') {
                want_s = want_n = want_r = want_v = want_m = 1;
                any = 1;
            } else if (o[j] == 's') {
                want_s = 1;
                any = 1;
            } else if (o[j] == 'n') {
                want_n = 1;
                any = 1;
            } else if (o[j] == 'r') {
                want_r = 1;
                any = 1;
            } else if (o[j] == 'v') {
                want_v = 1;
                any = 1;
            } else if (o[j] == 'm') {
                want_m = 1;
                any = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }
    if (ai != argc) {
        usage();
        return 1;
    }
    if (!any) want_s = 1;

    for (i = 0; i < UTS_SIZE; i++) uts[i] = 0;
    if (u_uname(uts) < 0) {
        u_write(2, "uname: failed\n", 14);
        return 1;
    }

    if (want_s) {
        if (!first) printf(" ");
        printf("%s", uts);
        first = 0;
    }
    if (want_n) {
        if (!first) printf(" ");
        printf("%s", uts + UTS_LEN);
        first = 0;
    }
    if (want_r) {
        if (!first) printf(" ");
        printf("%s", uts + 2 * UTS_LEN);
        first = 0;
    }
    if (want_v) {
        if (!first) printf(" ");
        printf("%s", uts + 3 * UTS_LEN);
        first = 0;
    }
    if (want_m) {
        if (!first) printf(" ");
        printf("%s", uts + 4 * UTS_LEN);
        first = 0;
    }
    printf("\n");
    return 0;
}
