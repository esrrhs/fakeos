/* fakeos userland - UDP/TCP netcat with select duplex + shutdown half-close.
 *   nc -u -l PORT          UDP listen (reply to last peer; stdin EOF exits)
 *   nc -u HOST PORT        UDP connected duplex
 *   nc -l PORT             TCP listen (one accept, then duplex)
 *   nc HOST PORT           TCP connect duplex
 * On stream stdin EOF: shutdown(SHUT_WR) and keep reading until peer EOF.
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
extern long u_shutdown(long fd, long how);
extern long u_select(long nfds, void *readfds, void *writefds, void *exceptfds,
                     void *timeout);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_close(long fd);
extern void printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);
extern int resolve_host(const char *name, unsigned int *out);

enum {
    AF_INET = 2,
    SOCK_STREAM = 1,
    SOCK_DGRAM = 2,
    SHUT_RD = 0,
    SHUT_WR = 1,
    SHUT_RDWR = 2,
    EINTR_SEL = -4
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

static void fdset_zero(unsigned long *set) {
    *set = 0;
}

static void fdset_set(unsigned long *set, int fd) {
    *set |= (unsigned long)1 << fd;
}

static int fdset_isset(unsigned long set, int fd) {
    return (set & ((unsigned long)1 << fd)) != 0;
}

/* Copy 16-byte sockaddr_in. */
static void addr_copy(unsigned char *dst, const unsigned char *src) {
    int i;
    for (i = 0; i < 16; i++) dst[i] = src[i];
}

/* Connected stream/dgram relay. Stream: stdin EOF -> shutdown(WR), keep RX
 * until peer EOF; peer EOF ends the session even if stdin is still open
 * (do not park forever on a TTY). Dgram: stdin EOF ends the session. */
static int relay_duplex(long sock, int is_stream) {
    int stdin_open = 1;

    while (1) {
        unsigned long rfds;
        long nready;
        int nfds;

        if (!is_stream && !stdin_open) return 0;

        fdset_zero(&rfds);
        nfds = 0;
        if (stdin_open) {
            fdset_set(&rfds, 0);
            nfds = 1;
        }
        fdset_set(&rfds, (int)sock);
        if ((int)sock + 1 > nfds) nfds = (int)sock + 1;

        nready = u_select((long)nfds, &rfds, 0, 0, 0);
        if (nready == EINTR_SEL) continue;
        if (nready < 0) {
            u_write(2, "nc: select failed\n", 18);
            return 1;
        }
        if (nready == 0) continue;

        if (stdin_open && fdset_isset(rfds, 0)) {
            char buf[512];
            long n = u_read(0, buf, 512);
            if (n < 0) {
                u_write(2, "nc: stdin read failed\n", 22);
                return 1;
            }
            if (n == 0) {
                if (is_stream) {
                    u_shutdown(sock, SHUT_WR);
                } else {
                    return 0;
                }
                stdin_open = 0;
            } else {
                long w = u_sendto(sock, buf, (unsigned long)n, 0, 0, 0);
                if (w != n) {
                    u_write(2, "nc: send failed\n", 16);
                    return 1;
                }
            }
        }

        if (fdset_isset(rfds, (int)sock)) {
            char buf[512];
            long n = u_recvfrom(sock, buf, 512, 0, 0, 0);
            if (n < 0) {
                u_write(2, "nc: recv failed\n", 16);
                return 1;
            }
            if (n == 0) {
                return 0; /* peer EOF / FIN: done */
            }
            if (u_write(1, buf, (unsigned long)n) < 0) {
                u_write(2, "nc: write failed\n", 17);
                return 1;
            }
        }
    }
    return 0;
}

/* UDP listen: select on stdin + sock; reply to last peer; stdin EOF exits. */
static int udp_listen_duplex(long s) {
    int stdin_open = 1;
    int have_peer = 0;
    unsigned char peer[16];
    unsigned int peerlen = 16;

    while (stdin_open) {
        unsigned long rfds;
        long nready;
        int nfds;

        fdset_zero(&rfds);
        fdset_set(&rfds, 0);
        fdset_set(&rfds, (int)s);
        nfds = (int)s + 1;
        if (nfds < 1) nfds = 1;

        nready = u_select((long)nfds, &rfds, 0, 0, 0);
        if (nready == EINTR_SEL) continue;
        if (nready < 0) {
            u_write(2, "nc: select failed\n", 18);
            return 1;
        }

        if (fdset_isset(rfds, (int)s)) {
            char buf[512];
            unsigned char from[16];
            unsigned int fromlen = 16;
            long n = u_recvfrom(s, buf, 512, 0, from, &fromlen);
            if (n < 0) {
                u_write(2, "nc: recvfrom failed\n", 20);
                return 1;
            }
            if (n > 0) {
                addr_copy(peer, from);
                peerlen = fromlen;
                have_peer = 1;
                if (u_write(1, buf, (unsigned long)n) < 0) {
                    u_write(2, "nc: write failed\n", 17);
                    return 1;
                }
            }
        }

        if (fdset_isset(rfds, 0)) {
            char buf[512];
            long n = u_read(0, buf, 512);
            if (n < 0) {
                u_write(2, "nc: stdin read failed\n", 22);
                return 1;
            }
            if (n == 0) {
                stdin_open = 0;
            } else if (have_peer) {
                long w = u_sendto(s, buf, (unsigned long)n, 0, peer, peerlen);
                if (w != n) {
                    u_write(2, "nc: sendto failed\n", 18);
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int tcp_listen_mode(unsigned int port) {
    long s;
    long c;
    unsigned char addr[16];
    unsigned char from[16];
    unsigned int fromlen = 16;
    int rc;

    make_addr(addr, 0, (unsigned short)port);
    s = u_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        u_write(2, "nc: socket failed\n", 18);
        return 1;
    }
    if (u_bind(s, addr, 16) != 0) {
        u_write(2, "nc: bind failed\n", 16);
        u_close(s);
        return 1;
    }
    if (u_listen(s, 1) != 0) {
        u_write(2, "nc: listen failed\n", 18);
        u_close(s);
        return 1;
    }
    printf("TCP listen on %u\n", port);
    c = u_accept(s, from, &fromlen);
    if (c < 0) {
        u_write(2, "nc: accept failed\n", 18);
        u_close(s);
        return 1;
    }
    u_close(s);
    rc = relay_duplex(c, 1);
    u_close(c);
    return rc;
}

static int tcp_client_mode(unsigned int ip, unsigned int port) {
    long s;
    unsigned char addr[16];
    int rc;

    make_addr(addr, ip, (unsigned short)port);
    s = u_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        u_write(2, "nc: socket failed\n", 18);
        return 1;
    }
    if (u_connect(s, addr, 16) != 0) {
        u_write(2, "nc: connect failed\n", 19);
        u_close(s);
        return 1;
    }
    rc = relay_duplex(s, 1);
    u_close(s);
    return rc;
}

int main(int argc, char **argv) {
    int udp = 0;
    int listen = 0;
    int i = 1;
    long s;
    unsigned char addr[16];
    unsigned int port;
    unsigned int ip;
    int rc;

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
            u_close(s);
            return 1;
        }
        make_addr(addr, 0, (unsigned short)port);
        if (u_bind(s, addr, 16) != 0) {
            u_write(2, "nc: bind failed\n", 16);
            u_close(s);
            return 1;
        }
        printf("UDP listen on %u\n", port);
        rc = udp_listen_duplex(s);
        u_close(s);
        return rc;
    }
    if (i + 1 >= argc || !resolve_host(argv[i], &ip) || !parse_port(argv[i + 1], &port)) {
        usage();
        u_close(s);
        return 1;
    }
    make_addr(addr, ip, (unsigned short)port);
    if (u_connect(s, addr, 16) != 0) {
        u_write(2, "nc: connect failed\n", 19);
        u_close(s);
        return 1;
    }
    rc = relay_duplex(s, 0);
    u_close(s);
    return rc;
}
