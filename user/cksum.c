/* fakeos userland - cksum: POSIX CRC checksum + length (thin).
 * Forms: cksum [FILE...] | cksum -c [FILE...]
 * Prints "CRC LENGTH [FILE]" per input (stdin if none; no name for stdin/-).
 * -c verifies cksum lines ("CRC LENGTH FILE"). Algorithm matches POSIX/GNU
 * cksum (poly 0x04C11DB7, length folded LE bytes, final bitwise complement).
 * Compact argv for EXEC_MAX_ARGC=8.
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

enum { LINE_MAX = 512 };

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

static void emit_str(const char *s) {
    long i = 0;
    while (s[i] != 0) i++;
    if (i > 0) u_write(1, s, (unsigned long)i);
}

/* Parse decimal u32; advance *pp. Returns 0 on failure. */
static int parse_u32(const char **pp, u32 *out) {
    const char *p = *pp;
    u32 v = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (u32)(*p - '0');
        p++;
        any = 1;
    }
    if (!any) return 0;
    *out = v;
    *pp = p;
    return 1;
}

/* Verify one "CRC LENGTH FILE" line. Returns 0 ok, 1 fail. */
static int check_line(const char *line) {
    const char *p = line;
    u32 expect_crc;
    u32 expect_len;
    u32 got_crc;
    u64 got_len;
    const char *name;
    long fd;

    if (!parse_u32(&p, &expect_crc)) {
        u_write(2, "cksum: bad checksum line\n", 25);
        return 1;
    }
    if (*p != ' ') {
        u_write(2, "cksum: bad checksum line\n", 25);
        return 1;
    }
    p++;
    if (!parse_u32(&p, &expect_len)) {
        u_write(2, "cksum: bad checksum line\n", 25);
        return 1;
    }
    if (*p != ' ') {
        u_write(2, "cksum: bad checksum line\n", 25);
        return 1;
    }
    p++;
    name = p;
    if (name[0] == 0) {
        u_write(2, "cksum: bad checksum line\n", 25);
        return 1;
    }

    if (strcmp(name, "-") == 0) {
        if (cksum_fd(0, &got_crc, &got_len) != 0) {
            emit_str(name);
            u_write(1, ": FAILED\n", 9);
            return 1;
        }
    } else {
        fd = u_open(name, 0, 0);
        if (fd < 0) {
            emit_str(name);
            u_write(1, ": FAILED open\n", 14);
            return 1;
        }
        if (cksum_fd(fd, &got_crc, &got_len) != 0) {
            u_close(fd);
            emit_str(name);
            u_write(1, ": FAILED\n", 9);
            return 1;
        }
        u_close(fd);
    }
    emit_str(name);
    if (got_crc == expect_crc && (u32)got_len == expect_len) {
        u_write(1, ": OK\n", 5);
        return 0;
    }
    u_write(1, ": FAILED\n", 9);
    return 1;
}

static int check_fd(long fd) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;
    int rc = 0;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[li] = 0;
                if (li > 0) {
                    if (check_line(line) != 0) rc = 1;
                }
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        if (check_line(line) != 0) rc = 1;
    }
    return n < 0 ? 1 : rc;
}

static void usage(void) {
    u_write(2, "usage: cksum [-c] [FILE...]\n", 28);
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    int check = 0;
    u32 crc;
    u64 len;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        int j;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-") == 0) {
            break;
        }
        for (j = 1; argv[i][j]; j++) {
            if (argv[i][j] == 'c') check = 1;
            else {
                usage();
                return 1;
            }
        }
        i++;
    }

    if (check) {
        if (i >= argc) return check_fd(0);
        for (; i < argc; i++) {
            long fd;
            if (strcmp(argv[i], "-") == 0) {
                if (check_fd(0) != 0) rc = 1;
                continue;
            }
            fd = u_open(argv[i], 0, 0);
            if (fd < 0) {
                printf("cksum: %s: open failed\n", argv[i]);
                rc = 1;
                continue;
            }
            if (check_fd(fd) != 0) rc = 1;
            u_close(fd);
        }
        return rc;
    }

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
            printf("cksum: %s: open failed\n", argv[i]);
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
