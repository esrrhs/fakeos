/* fakeos userland - UDP socket loopback self-test. */
package user;

extern long u_socket(long domain, long type, long protocol);
extern long u_bind(long fd, const void *addr, unsigned long len);
extern long u_sendto(long fd, const void *buf, unsigned long len, long flags,
                     const void *addr, unsigned long alen);
extern long u_recvfrom(long fd, void *buf, unsigned long len, long flags,
                       void *addr, unsigned int *alen);
extern long u_close(long fd);
extern long u_write(long fd, const void *buf, unsigned long n);
extern void printf(const char *fmt, ...);

enum {
    AF_INET = 2,
    SOCK_DGRAM = 2,
    IPPROTO_UDP = 17
};

/* Linux sockaddr_in layout (16 bytes). */
static void make_addr(unsigned char *sa, unsigned int ip_host, unsigned short port) {
    unsigned int i;
    for (i = 0; i < 16; i++) {
        sa[i] = 0;
    }
    sa[0] = AF_INET;
    sa[1] = 0;
    sa[2] = (unsigned char)(port >> 8);
    sa[3] = (unsigned char)(port & 0xFF);
    sa[4] = (unsigned char)(ip_host >> 24);
    sa[5] = (unsigned char)(ip_host >> 16);
    sa[6] = (unsigned char)(ip_host >> 8);
    sa[7] = (unsigned char)(ip_host & 0xFF);
}

int main(int argc, char **argv) {
    long s;
    unsigned char addr[16];
    unsigned char from[16];
    unsigned int fromlen;
    char msg[16];
    char buf[64];
    long n;
    unsigned int i;
    unsigned int local = 0x0A00020Fu; /* 10.0.2.15 */
    (void)argc;
    (void)argv;

    s = u_socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        u_write(2, "udptest: socket failed\n", 22);
        return 1;
    }
    make_addr(addr, 0, 9999);
    if (u_bind(s, addr, 16) != 0) {
        u_write(2, "udptest: bind failed\n", 20);
        u_close(s);
        return 1;
    }
    msg[0] = 'u'; msg[1] = 'd'; msg[2] = 'p'; msg[3] = '-';
    msg[4] = 'o'; msg[5] = 'k'; msg[6] = 0;
    make_addr(addr, local, 9999);
    n = u_sendto(s, msg, 6, 0, addr, 16);
    if (n != 6) {
        printf("udptest: sendto failed (%d)\n", (int)n);
        u_close(s);
        return 1;
    }
    fromlen = 16;
    n = u_recvfrom(s, buf, 64, 0, from, &fromlen);
    if (n != 6) {
        printf("udptest: recvfrom failed (%d)\n", (int)n);
        u_close(s);
        return 1;
    }
    for (i = 0; i < 6; i++) {
        if (buf[i] != msg[i]) {
            u_write(2, "udptest: payload mismatch\n", 25);
            u_close(s);
            return 1;
        }
    }
    u_close(s);
    printf("UDP loopback OK (%d bytes)\n", (int)n);
    return 0;
}
