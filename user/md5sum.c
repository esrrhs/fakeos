/* fakeos userland - md5sum: MD5 digest (thin, RFC 1321).
 * Forms: md5sum [FILE...] | md5sum -c [FILE...]
 * Prints "DIGEST  FILE" (two spaces; "-" for stdin/-). -c verifies GNU
 * checksum lines (DIGEST  FILE or DIGEST *FILE). Real MD5, not a stub.
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

enum { LINE_MAX = 512 };

static u32 rol(u32 x, int n) {
    return (x << n) | (x >> (32 - n));
}

static u32 F(u32 x, u32 y, u32 z) { return (x & y) | ((~x) & z); }
static u32 G(u32 x, u32 y, u32 z) { return (x & z) | (y & (~z)); }
static u32 H(u32 x, u32 y, u32 z) { return x ^ y ^ z; }
static u32 I(u32 x, u32 y, u32 z) { return y ^ (x | (~z)); }

static const u32 K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const int S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

struct md5_ctx {
    u32 a, b, c, d;
    u64 nbits;
    unsigned char buf[64];
    int nbuf;
};

static void md5_init(struct md5_ctx *ctx) {
    ctx->a = 0x67452301u;
    ctx->b = 0xefcdab89u;
    ctx->c = 0x98badcfeu;
    ctx->d = 0x10325476u;
    ctx->nbits = 0;
    ctx->nbuf = 0;
}

static void md5_block(struct md5_ctx *ctx, const unsigned char *blk) {
    u32 M[16];
    u32 A, B, C, D, f, g, tmp;
    int i;
    for (i = 0; i < 16; i++) {
        M[i] = ((u32)blk[i * 4]) |
               (((u32)blk[i * 4 + 1]) << 8) |
               (((u32)blk[i * 4 + 2]) << 16) |
               (((u32)blk[i * 4 + 3]) << 24);
    }
    A = ctx->a;
    B = ctx->b;
    C = ctx->c;
    D = ctx->d;
    for (i = 0; i < 64; i++) {
        if (i < 16) {
            f = F(B, C, D);
            g = i;
        } else if (i < 32) {
            f = G(B, C, D);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = H(B, C, D);
            g = (3 * i + 5) & 15;
        } else {
            f = I(B, C, D);
            g = (7 * i) & 15;
        }
        tmp = D;
        D = C;
        C = B;
        B = B + rol(A + f + K[i] + M[g], S[i]);
        A = tmp;
    }
    ctx->a += A;
    ctx->b += B;
    ctx->c += C;
    ctx->d += D;
}

static void md5_update(struct md5_ctx *ctx, const unsigned char *data, long n) {
    long i = 0;
    ctx->nbits += ((u64)n) * 8;
    while (i < n) {
        ctx->buf[ctx->nbuf++] = data[i++];
        if (ctx->nbuf == 64) {
            md5_block(ctx, ctx->buf);
            ctx->nbuf = 0;
        }
    }
}

static void md5_final(struct md5_ctx *ctx, unsigned char out[16]) {
    u64 bits;
    int i;
    ctx->buf[ctx->nbuf++] = 0x80;
    if (ctx->nbuf > 56) {
        while (ctx->nbuf < 64) ctx->buf[ctx->nbuf++] = 0;
        md5_block(ctx, ctx->buf);
        ctx->nbuf = 0;
    }
    while (ctx->nbuf < 56) ctx->buf[ctx->nbuf++] = 0;
    bits = ctx->nbits;
    for (i = 0; i < 8; i++) {
        ctx->buf[56 + i] = (unsigned char)(bits & 0xff);
        bits >>= 8;
    }
    md5_block(ctx, ctx->buf);
    out[0] = (unsigned char)(ctx->a);
    out[1] = (unsigned char)(ctx->a >> 8);
    out[2] = (unsigned char)(ctx->a >> 16);
    out[3] = (unsigned char)(ctx->a >> 24);
    out[4] = (unsigned char)(ctx->b);
    out[5] = (unsigned char)(ctx->b >> 8);
    out[6] = (unsigned char)(ctx->b >> 16);
    out[7] = (unsigned char)(ctx->b >> 24);
    out[8] = (unsigned char)(ctx->c);
    out[9] = (unsigned char)(ctx->c >> 8);
    out[10] = (unsigned char)(ctx->c >> 16);
    out[11] = (unsigned char)(ctx->c >> 24);
    out[12] = (unsigned char)(ctx->d);
    out[13] = (unsigned char)(ctx->d >> 8);
    out[14] = (unsigned char)(ctx->d >> 16);
    out[15] = (unsigned char)(ctx->d >> 24);
}

static void emit_hex(const unsigned char *dig, int n) {
    const char *h = "0123456789abcdef";
    char tmp[2];
    int i;
    for (i = 0; i < n; i++) {
        tmp[0] = h[(dig[i] >> 4) & 0xf];
        tmp[1] = h[dig[i] & 0xf];
        u_write(1, tmp, 2);
    }
}

static void print_one(const unsigned char dig[16], const char *name) {
    emit_hex(dig, 16);
    u_write(1, "  ", 2);
    if (name != 0 && name[0] != 0) {
        long i = 0;
        while (name[i] != 0) i++;
        u_write(1, name, (unsigned long)i);
    } else {
        u_write(1, "-", 1);
    }
    u_write(1, "\n", 1);
}

static int md5_fd(long fd, unsigned char dig[16]) {
    struct md5_ctx ctx;
    char buf[512];
    long n;
    md5_init(&ctx);
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        md5_update(&ctx, (const unsigned char *)buf, n);
    }
    if (n < 0) return 1;
    md5_final(&ctx, dig);
    return 0;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, unsigned char *out, int nbytes) {
    int i;
    for (i = 0; i < nbytes; i++) {
        int hi = hex_val(s[i * 2]);
        int lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 1;
}

static int dig_eq(const unsigned char *a, const unsigned char *b, int n) {
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void emit_str(const char *s) {
    long i = 0;
    while (s[i] != 0) i++;
    if (i > 0) u_write(1, s, (unsigned long)i);
}

/* Verify one GNU "DIGEST  FILE" / "DIGEST *FILE" line. Returns 0 ok, 1 fail. */
static int check_line(const char *line) {
    unsigned char expect[16];
    unsigned char got[16];
    const char *name;
    long fd;
    int i;
    /* MD5 hex is 32 chars */
    for (i = 0; i < 32; i++) {
        if (hex_val(line[i]) < 0) {
            u_write(2, "md5sum: bad checksum line\n", 26);
            return 1;
        }
    }
    if (!parse_hex(line, expect, 16)) {
        u_write(2, "md5sum: bad checksum line\n", 26);
        return 1;
    }
    if (line[32] != ' ' || (line[33] != ' ' && line[33] != '*')) {
        u_write(2, "md5sum: bad checksum line\n", 26);
        return 1;
    }
    name = line + 34;
    if (name[0] == 0) {
        u_write(2, "md5sum: bad checksum line\n", 26);
        return 1;
    }
    if (strcmp(name, "-") == 0) {
        if (md5_fd(0, got) != 0) {
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
        if (md5_fd(fd, got) != 0) {
            u_close(fd);
            emit_str(name);
            u_write(1, ": FAILED\n", 9);
            return 1;
        }
        u_close(fd);
    }
    emit_str(name);
    if (dig_eq(expect, got, 16)) {
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

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    int check = 0;
    unsigned char dig[16];

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-c") == 0) {
            check = 1;
            i++;
            continue;
        }
        /* glued -c only for thin */
        if (argv[i][1] == 'c' && argv[i][2] == 0) {
            check = 1;
            i++;
            continue;
        }
        u_write(2, "usage: md5sum [-c] [FILE...]\n", 29);
        return 1;
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
                u_write(2, "md5sum: open failed\n", 20);
                rc = 1;
                continue;
            }
            if (check_fd(fd) != 0) rc = 1;
            u_close(fd);
        }
        return rc;
    }

    if (i >= argc) {
        if (md5_fd(0, dig) != 0) return 1;
        print_one(dig, 0);
        return 0;
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (md5_fd(0, dig) != 0) {
                rc = 1;
                continue;
            }
            print_one(dig, 0);
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "md5sum: open failed\n", 20);
            rc = 1;
            continue;
        }
        if (md5_fd(fd, dig) != 0) {
            u_close(fd);
            rc = 1;
            continue;
        }
        u_close(fd);
        print_one(dig, argv[i]);
    }
    return rc;
}
