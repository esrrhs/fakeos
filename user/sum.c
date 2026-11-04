/* fakeos userland - sum: BSD / System V checksum (thin).
 * Forms: sum [-r|-s] [FILE...]
 * Default -r (BSD rotating 16-bit checksum, 1024-byte blocks).
 * -s System V (sum of bytes folded to 16 bits, 512-byte blocks).
 * Prints "CHECKSUM BLOCKS [FILE]" (no name for stdin/-).
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

enum { MODE_BSD = 0, MODE_SYSV = 1 };

static int mode = MODE_BSD;

static void usage(void) {
    u_write(2, "usage: sum [-r|-s] [FILE...]\n", 30);
}

static int sum_fd(long fd, u32 *sum_out, u64 *blocks_out) {
    char buf[512];
    long n;
    long i;
    u64 len = 0;
    u32 bsd = 0;
    u32 sysv = 0;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            /* BSD: 16-bit rotate-right then add */
            bsd = (bsd >> 1) + ((bsd & 1u) << 15);
            bsd = (bsd + c) & 0xffffu;
            sysv = (sysv + c) & 0xffffffffu;
        }
        len += (u64)n;
    }
    if (n < 0) return 1;

    if (mode == MODE_BSD) {
        *sum_out = bsd;
        *blocks_out = (len + 1023) / 1024;
    } else {
        u32 r = (sysv & 0xffffu) + ((sysv >> 16) & 0xffffu);
        *sum_out = (r & 0xffffu) + (r >> 16);
        *blocks_out = (len + 511) / 512;
    }
    return 0;
}

static void print_one(u32 sum, u64 blocks, const char *name) {
    if (name != 0 && name[0] != 0) {
        printf("%u %u %s\n", (unsigned)sum, (unsigned)blocks, name);
    } else {
        printf("%u %u\n", (unsigned)sum, (unsigned)blocks);
    }
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    u32 sum;
    u64 blocks;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-") == 0) {
            break;
        }
        if (strcmp(argv[i], "-r") == 0) {
            mode = MODE_BSD;
            i++;
        } else if (strcmp(argv[i], "-s") == 0) {
            mode = MODE_SYSV;
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        if (sum_fd(0, &sum, &blocks) != 0) return 1;
        print_one(sum, blocks, 0);
        return 0;
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (sum_fd(0, &sum, &blocks) != 0) {
                rc = 1;
                continue;
            }
            print_one(sum, blocks, 0);
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "sum: open failed\n", 17);
            rc = 1;
            continue;
        }
        if (sum_fd(fd, &sum, &blocks) != 0) {
            u_close(fd);
            rc = 1;
            continue;
        }
        u_close(fd);
        print_one(sum, blocks, argv[i]);
    }
    return rc;
}
