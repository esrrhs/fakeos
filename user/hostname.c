/* fakeos userland - hostname: print nodename from uname(2) (thin).
 *   hostname
 *   hostname -s
 * -s  short hostname: truncate at first '.' (nodename may already be short).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_uname(void *buf);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);

enum { UTS_LEN = 65, UTS_SIZE = 390 };

static void usage(void) {
    u_write(2, "usage: hostname [-s]\n", 21);
}

int main(int argc, char **argv) {
    char uts[UTS_SIZE];
    long i;
    const char *node;
    unsigned long n;
    int short_name = 0;
    int ai = 1;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) break;
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 's') {
                short_name = 1;
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
    for (i = 0; i < UTS_SIZE; i++) uts[i] = 0;
    if (u_uname(uts) < 0) {
        u_write(2, "hostname: uname failed\n", 23);
        return 1;
    }
    /* nodename is the second 65-byte field. */
    node = uts + UTS_LEN;
    n = strlen(node);
    if (n == 0) {
        u_write(1, "(none)\n", 7);
        return 0;
    }
    if (short_name) {
        unsigned long k = 0;
        while (k < n && node[k] != '.') k++;
        n = k;
    }
    if (n == 0) {
        u_write(1, "(none)\n", 7);
        return 0;
    }
    u_write(1, node, n);
    u_write(1, "\n", 1);
    return 0;
}
