/* fakeos userland - UDP/TCP netcat.
 *   nc -u -l PORT          UDP listen
 *   nc -u HOST PORT        UDP send stdin lines
 *   nc -l PORT             TCP listen (one accept, echo payload to stdout)
 *   nc HOST PORT           TCP connect; stdin -> send, recv -> stdout
 */
package user;

extern long u_socket(long domain, long type, long protocol);
extern long u_bind(long fd, const void *addr, unsigned long len);
extern long u_listen(long fd, long backlog);
extern long u_accept(long fd, void *addr, unsigned int *alen);
extern long u_connect(long fd, const void *addr, unsigned long len);
extern long u_sendto(long fd, const void *buf, unsigned long len, long flags,
                     const void *addr, unsigned long alen);
extern long u_recvfrom(long fd, void *buf, unsigned long len, long flags,
                       void *addr, unsigned int *alen);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_close(long fd);
extern void printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);
extern int resolve_host(const char *name, unsigned int *out);

enum {
    AF_INET = 2,
    SOCK_STREAM = 1,
    SOCK_DGRAM = 2
};

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
            if (cur > 255) return 0;
            digits++;
            i++;
            continue;
        }
        if (digits == 0) return 0;
        b[n++] = cur;
        cur = 0;
        digits = 0;
        if (c == '.') {
            if (n >= 4) return 0;
            i++;
            continue;
        }
        if (c == 0) break;
        return 0;
    }
    if (n != 4) return 0;
    *out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return 1;
}

static int parse_port(const char *s, unsigned int *out) {
    unsigned int v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned int)(*s - '0');
        if (v > 65535) return 0;
        digits++;
        s++;
    }
    if (digits == 0 || *s != 0) return 0;
    *out = v;
    return 1;
}

static void make_addr(unsigned char *sa, unsigned int ip_host, unsigned short port) {
    unsigned int i;
    for (i = 0; i < 16; i++) sa[i] = 0;
    sa[0] = AF_INET;
    sa[2] = (unsigned char)(port >> 8);
    sa[3] = (unsigned char)(port & 0xFF);
    sa[4] = (unsigned char)(ip_host >> 24);
    sa[5] = (unsigned char)(ip_host >> 16);
    sa[6] = (unsigned char)(ip_host >> 8);
    sa[7] = (unsigned char)(ip_host & 0xFF);
}

static void usage(void) {
    u_write(2, "usage: nc [-u] -l PORT | nc [-u] HOST PORT\n", 44);
}

static int tcp_listen_mode(unsigned int port) {
    long s;
    long c;
    unsigned char addr[16];
    unsigned char from[16];
    unsigned int fromlen = 16;
    make_addr(addr, 0, (unsigned short)port);
    s = u_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        u_write(2, "nc: socket failed\n", 18);
        return 1;
    }
    if (u_bind(s, addr, 16) != 0) {
        u_write(2, "nc: bind failed\n", 16);
        return 1;
    }
    if (u_listen(s, 1) != 0) {
        u_write(2, "nc: listen failed\n", 18);
        return 1;
    }
    printf("TCP listen on %u\n", port);
    c = u_accept(s, from, &fromlen);
    if (c < 0) {
        u_write(2, "nc: accept failed\n", 18);
        return 1;
    }
    u_close(s);
    for (;;) {
        char buf[512];
        long n = u_recvfrom(c, buf, 512, 0, 0, 0);
        if (n < 0) {
            u_write(2, "nc: recv failed\n", 16);
            return 1;
        }
        if (n == 0) break;
        u_write(1, buf, (unsigned long)n);
    }
    u_close(c);
    return 0;
}

static int tcp_client_mode(unsigned int ip, unsigned int port) {
    long s;
    unsigned char addr[16];
    make_addr(addr, ip, (unsigned short)port);
    s = u_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        u_write(2, "nc: socket failed\n", 18);
        return 1;
    }
    if (u_connect(s, addr, 16) != 0) {
        u_write(2, "nc: connect failed\n", 19);
        return 1;
    }
    for (;;) {
        char line[256];
        long n = u_read(0, line, 256);
        long w;
        if (n <= 0) break;
        w = u_sendto(s, line, (unsigned long)n, 0, 0, 0);
        if (w != n) {
            u_write(2, "nc: send failed\n", 16);
            return 1;
        }
    }
    u_close(s);
    return 0;
}

int main(int argc, char **argv) {
    int udp = 0;
    int listen = 0;
    int i = 1;
    long s;
    unsigned char addr[16];
    unsigned int port;
    unsigned int ip;

    while (i < argc) {
        if (strcmp(argv[i], "-u") == 0) {
            udp = 1;
            i++;
        } else if (strcmp(argv[i], "-l") == 0) {
            listen = 1;
            i++;
        } else {
            break;
        }
    }

    if (!udp) {
        if (listen) {
            if (i >= argc || !parse_port(argv[i], &port)) {
                usage();
                return 1;
            }
            return tcp_listen_mode(port);
        }
        if (i + 1 >= argc || !resolve_host(argv[i], &ip) || !parse_port(argv[i + 1], &port)) {
            usage();
            return 1;
        }
        return tcp_client_mode(ip, port);
    }

    s = u_socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        u_write(2, "nc: socket failed\n", 18);
        return 1;
    }
    if (listen) {
        if (i >= argc || !parse_port(argv[i], &port)) {
            usage();
            return 1;
        }
        make_addr(addr, 0, (unsigned short)port);
        if (u_bind(s, addr, 16) != 0) {
            u_write(2, "nc: bind failed\n", 16);
            return 1;
        }
        printf("UDP listen on %u\n", port);
        for (;;) {
            char buf[512];
            unsigned char from[16];
            unsigned int fromlen = 16;
            long n = u_recvfrom(s, buf, 512, 0, from, &fromlen);
            if (n < 0) {
                u_write(2, "nc: recvfrom failed\n", 20);
                return 1;
            }
            if (n > 0) u_write(1, buf, (unsigned long)n);
        }
    }
    if (i + 1 >= argc || !resolve_host(argv[i], &ip) || !parse_port(argv[i + 1], &port)) {
        usage();
        return 1;
    }
    make_addr(addr, ip, (unsigned short)port);
    if (u_connect(s, addr, 16) != 0) {
        u_write(2, "nc: connect failed\n", 19);
        return 1;
    }
    for (;;) {
        char line[256];
        long n = u_read(0, line, 256);
        long w;
        if (n <= 0) break;
        w = u_sendto(s, line, (unsigned long)n, 0, 0, 0);
        if (w != n) {
            u_write(2, "nc: sendto failed\n", 18);
            return 1;
        }
    }
    u_close(s);
    return 0;
}
