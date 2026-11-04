/* fakeos userland - hostname: print nodename from uname(2) (thin). */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_uname(void *buf);
extern unsigned long strlen(const char *s);

enum { UTS_LEN = 65, UTS_SIZE = 390 };

int main(int argc, char **argv) {
    char uts[UTS_SIZE];
    long i;
    const char *node;
    unsigned long n;

    if (argc > 1) {
        u_write(2, "usage: hostname\n", 16);
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
    u_write(1, node, n);
    u_write(1, "\n", 1);
    return 0;
}
