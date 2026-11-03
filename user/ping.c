/* fakeos userland - ping: ICMP echo via sys_ping (host-order IPv4). */
package user;

extern long u_ping(unsigned int ip);
extern long u_write(long fd, const void *buf, unsigned long n);
extern void printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);

static int parse_ipv4(const char *s, unsigned int *out) {
    unsigned int b[4];
    int i = 0;
    int n = 0;
    unsigned int cur = 0;
    int digits = 0;
    while (1) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (unsigned int)(c - '0');
            if (cur > 255) {
                return 0;
            }
            digits++;
            i++;
            continue;
        }
        if (digits == 0) {
            return 0;
        }
        b[n++] = cur;
        cur = 0;
        digits = 0;
        if (c == '.') {
            if (n >= 4) {
                return 0;
            }
            i++;
            continue;
        }
        if (c == 0) {
            break;
        }
        return 0;
    }
    if (n != 4) {
        return 0;
    }
    *out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return 1;
}

int main(int argc, char **argv) {
    unsigned int ip;
    long r;
    int count = 3;
    int i;
    int ok = 0;

    if (argc < 2) {
        u_write(2, "usage: ping [-c N] IP\n", 22);
        return 1;
    }
    i = 1;
    if (argc >= 4 && strcmp(argv[1], "-c") == 0) {
        /* tiny decimal parse */
        const char *p = argv[2];
        count = 0;
        while (*p >= '0' && *p <= '9') {
            count = count * 10 + (*p - '0');
            p++;
        }
        if (count <= 0) {
            count = 1;
        }
        if (count > 20) {
            count = 20;
        }
        i = 3;
    }
    if (i >= argc || !parse_ipv4(argv[i], &ip)) {
        u_write(2, "ping: bad IPv4 address\n", 23);
        return 1;
    }
    printf("PING %s\n", argv[i]);
    for (; count > 0; count--) {
        r = u_ping(ip);
        if (r == 0) {
            printf("reply from %s: icmp_seq ok\n", argv[i]);
            ok++;
        } else {
            printf("no reply from %s\n", argv[i]);
        }
    }
    printf("%d replies\n", ok);
    return ok > 0 ? 0 : 1;
}
