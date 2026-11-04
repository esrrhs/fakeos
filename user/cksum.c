/* fakeos userland - cksum: POSIX CRC checksum + length (thin).
 * Forms: cksum [FILE...]
 * Prints "CRC LENGTH [FILE]" per input (stdin if none; no name for stdin/-).
 * Algorithm matches POSIX/GNU cksum (poly 0x04C11DB7, length folded LE bytes,
 * final bitwise complement).
 */
package user;

typedef unsigned int u32;
typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

static u32 crc_update(u32 crc, unsigned char b) {
    int i;
    crc ^= ((u32)b) << 24;
    for (i = 0; i < 8; i++) {
        if (crc & 0x80000000u) {
            crc = (crc << 1) ^ 0x04C11DB7u;
        } else {
            crc = crc << 1;
        }
    }
    return crc;
}

static int cksum_fd(long fd, u32 *crc_out, u64 *len_out) {
    char buf[512];
    long n;
    u32 crc = 0;
    u64 len = 0;
    long i;
    u64 rem;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < n; i++) {
            crc = crc_update(crc, (unsigned char)buf[i]);
        }
        len += (u64)n;
    }
    if (n < 0) return 1;

    rem = len;
    while (rem != 0) {
        crc = crc_update(crc, (unsigned char)(rem & 0xff));
        rem >>= 8;
    }
    *crc_out = ~crc;
    *len_out = len;
    return 0;
}

static void print_one(u32 crc, u64 len, const char *name) {
    if (name != 0 && name[0] != 0) {
        printf("%u %u %s\n", (unsigned)crc, (unsigned)len, name);
    } else {
        printf("%u %u\n", (unsigned)crc, (unsigned)len);
    }
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    u32 crc;
    u64 len;

    if (i >= argc) {
        if (cksum_fd(0, &crc, &len) != 0) return 1;
        print_one(crc, len, 0);
        return 0;
    }
    for (; i < argc; i++) {
        long fd;
        const char *name;
        if (strcmp(argv[i], "-") == 0) {
            if (cksum_fd(0, &crc, &len) != 0) {
                rc = 1;
                continue;
            }
            print_one(crc, len, 0);
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "cksum: open failed\n", 19);
            rc = 1;
            continue;
        }
        if (cksum_fd(fd, &crc, &len) != 0) {
            u_close(fd);
            rc = 1;
            continue;
        }
        u_close(fd);
        name = argv[i];
        print_one(crc, len, name);
    }
    return rc;
}
