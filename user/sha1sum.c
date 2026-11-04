/* fakeos userland - sha1sum: SHA-1 digest (thin, FIPS 180-1).
 * Forms: sha1sum [FILE...] | sha1sum -c [FILE...]
 * Prints "DIGEST  FILE" (two spaces; "-" for stdin/-). -c verifies GNU
 * checksum lines (DIGEST  FILE or DIGEST *FILE). Real SHA-1, not a stub.
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

struct sha1_ctx {
    u32 h0, h1, h2, h3, h4;
    u64 nbits;
    unsigned char buf[64];
    int nbuf;
};

static void sha1_init(struct sha1_ctx *ctx) {
    ctx->h0 = 0x67452301u;
    ctx->h1 = 0xefcdab89u;
    ctx->h2 = 0x98badcfeu;
    ctx->h3 = 0x10325476u;
    ctx->h4 = 0xc3d2e1f0u;
    ctx->nbits = 0;
    ctx->nbuf = 0;
}

static void sha1_block(struct sha1_ctx *ctx, const unsigned char *blk) {
    u32 w[80];
    u32 a, b, c, d, e, f, k, temp;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (((u32)blk[i * 4]) << 24) |
               (((u32)blk[i * 4 + 1]) << 16) |
               (((u32)blk[i * 4 + 2]) << 8) |
               ((u32)blk[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = ctx->h0;
    b = ctx->h1;
    c = ctx->h2;
    d = ctx->h3;
    e = ctx->h4;
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        temp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }
    ctx->h0 += a;
    ctx->h1 += b;
    ctx->h2 += c;
    ctx->h3 += d;
    ctx->h4 += e;
}

static void sha1_update(struct sha1_ctx *ctx, const unsigned char *data, long n) {
    long i = 0;
    ctx->nbits += ((u64)n) * 8;
    while (i < n) {
        ctx->buf[ctx->nbuf++] = data[i++];
        if (ctx->nbuf == 64) {
            sha1_block(ctx, ctx->buf);
            ctx->nbuf = 0;
        }
    }
}

static void sha1_final(struct sha1_ctx *ctx, unsigned char out[20]) {
    u64 bits;
    int i;
    ctx->buf[ctx->nbuf++] = 0x80;
    if (ctx->nbuf > 56) {
        while (ctx->nbuf < 64) ctx->buf[ctx->nbuf++] = 0;
        sha1_block(ctx, ctx->buf);
        ctx->nbuf = 0;
    }
    while (ctx->nbuf < 56) ctx->buf[ctx->nbuf++] = 0;
    bits = ctx->nbits;
    /* SHA-1 length is big-endian */
    for (i = 0; i < 8; i++) {
        ctx->buf[63 - i] = (unsigned char)(bits & 0xff);
        bits >>= 8;
    }
    sha1_block(ctx, ctx->buf);
    out[0] = (unsigned char)(ctx->h0 >> 24);
    out[1] = (unsigned char)(ctx->h0 >> 16);
    out[2] = (unsigned char)(ctx->h0 >> 8);
    out[3] = (unsigned char)(ctx->h0);
    out[4] = (unsigned char)(ctx->h1 >> 24);
    out[5] = (unsigned char)(ctx->h1 >> 16);
    out[6] = (unsigned char)(ctx->h1 >> 8);
    out[7] = (unsigned char)(ctx->h1);
    out[8] = (unsigned char)(ctx->h2 >> 24);
    out[9] = (unsigned char)(ctx->h2 >> 16);
    out[10] = (unsigned char)(ctx->h2 >> 8);
    out[11] = (unsigned char)(ctx->h2);
    out[12] = (unsigned char)(ctx->h3 >> 24);
    out[13] = (unsigned char)(ctx->h3 >> 16);
    out[14] = (unsigned char)(ctx->h3 >> 8);
    out[15] = (unsigned char)(ctx->h3);
    out[16] = (unsigned char)(ctx->h4 >> 24);
    out[17] = (unsigned char)(ctx->h4 >> 16);
    out[18] = (unsigned char)(ctx->h4 >> 8);
    out[19] = (unsigned char)(ctx->h4);
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

static void print_one(const unsigned char dig[20], const char *name) {
    emit_hex(dig, 20);
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

static int sha1_fd(long fd, unsigned char dig[20]) {
    struct sha1_ctx ctx;
    char buf[512];
    long n;
    sha1_init(&ctx);
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        sha1_update(&ctx, (const unsigned char *)buf, n);
    }
    if (n < 0) return 1;
    sha1_final(&ctx, dig);
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

static int check_line(const char *line) {
    unsigned char expect[20];
    unsigned char got[20];
    const char *name;
    long fd;
    int i;
    for (i = 0; i < 40; i++) {
        if (hex_val(line[i]) < 0) {
            u_write(2, "sha1sum: bad checksum line\n", 27);
            return 1;
        }
    }
    if (!parse_hex(line, expect, 20)) {
        u_write(2, "sha1sum: bad checksum line\n", 27);
        return 1;
    }
    if (line[40] != ' ' || (line[41] != ' ' && line[41] != '*')) {
        u_write(2, "sha1sum: bad checksum line\n", 27);
        return 1;
    }
    name = line + 42;
    if (name[0] == 0) {
        u_write(2, "sha1sum: bad checksum line\n", 27);
        return 1;
    }
    if (strcmp(name, "-") == 0) {
        if (sha1_fd(0, got) != 0) {
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
        if (sha1_fd(fd, got) != 0) {
            u_close(fd);
            emit_str(name);
            u_write(1, ": FAILED\n", 9);
            return 1;
        }
        u_close(fd);
    }
    emit_str(name);
    if (dig_eq(expect, got, 20)) {
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
    unsigned char dig[20];

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-c") == 0 ||
            (argv[i][1] == 'c' && argv[i][2] == 0)) {
            check = 1;
            i++;
            continue;
        }
        u_write(2, "usage: sha1sum [-c] [FILE...]\n", 30);
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
                u_write(2, "sha1sum: open failed\n", 21);
                rc = 1;
                continue;
            }
            if (check_fd(fd) != 0) rc = 1;
            u_close(fd);
        }
        return rc;
    }

    if (i >= argc) {
        if (sha1_fd(0, dig) != 0) return 1;
        print_one(dig, 0);
        return 0;
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (sha1_fd(0, dig) != 0) {
                rc = 1;
                continue;
            }
            print_one(dig, 0);
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "sha1sum: open failed\n", 21);
            rc = 1;
            continue;
        }
        if (sha1_fd(fd, dig) != 0) {
            u_close(fd);
            rc = 1;
            continue;
        }
        u_close(fd);
        print_one(dig, argv[i]);
    }
    return rc;
}
