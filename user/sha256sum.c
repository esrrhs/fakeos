/* fakeos userland - sha256sum: SHA-256 digest (thin, FIPS 180-4).
 * Forms: sha256sum [FILE...]
 * Prints "DIGEST  FILE" (two spaces; "-" for stdin/-). Real SHA-256, not a stub.
 */
package user;

typedef unsigned int u32;
typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);

static u32 rotr(u32 x, int n) {
    return (x >> n) | (x << (32 - n));
}

static u32 Ch(u32 x, u32 y, u32 z) { return (x & y) ^ ((~x) & z); }
static u32 Maj(u32 x, u32 y, u32 z) { return (x & y) ^ (x & z) ^ (y & z); }
static u32 Sig0(u32 x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
static u32 Sig1(u32 x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
static u32 sig0(u32 x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
static u32 sig1(u32 x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

static const u32 K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

struct sha256_ctx {
    u32 h[8];
    u64 nbits;
    unsigned char buf[64];
    int nbuf;
};

static void sha256_init(struct sha256_ctx *ctx) {
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->nbits = 0;
    ctx->nbuf = 0;
}

static void sha256_block(struct sha256_ctx *ctx, const unsigned char *blk) {
    u32 w[64];
    u32 a, b, c, d, e, f, g, hh, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (((u32)blk[i * 4]) << 24) |
               (((u32)blk[i * 4 + 1]) << 16) |
               (((u32)blk[i * 4 + 2]) << 8) |
               ((u32)blk[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        w[i] = sig1(w[i - 2]) + w[i - 7] + sig0(w[i - 15]) + w[i - 16];
    }
    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];
    f = ctx->h[5];
    g = ctx->h[6];
    hh = ctx->h[7];
    for (i = 0; i < 64; i++) {
        t1 = hh + Sig1(e) + Ch(e, f, g) + K[i] + w[i];
        t2 = Sig0(a) + Maj(a, b, c);
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += hh;
}

static void sha256_update(struct sha256_ctx *ctx, const unsigned char *data, long n) {
    long i = 0;
    ctx->nbits += ((u64)n) * 8;
    while (i < n) {
        ctx->buf[ctx->nbuf++] = data[i++];
        if (ctx->nbuf == 64) {
            sha256_block(ctx, ctx->buf);
            ctx->nbuf = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *ctx, unsigned char out[32]) {
    u64 bits;
    int i;
    ctx->buf[ctx->nbuf++] = 0x80;
    if (ctx->nbuf > 56) {
        while (ctx->nbuf < 64) ctx->buf[ctx->nbuf++] = 0;
        sha256_block(ctx, ctx->buf);
        ctx->nbuf = 0;
    }
    while (ctx->nbuf < 56) ctx->buf[ctx->nbuf++] = 0;
    bits = ctx->nbits;
    /* SHA-256 length is big-endian */
    for (i = 0; i < 8; i++) {
        ctx->buf[63 - i] = (unsigned char)(bits & 0xff);
        bits >>= 8;
    }
    sha256_block(ctx, ctx->buf);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(ctx->h[i]);
    }
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

static void print_one(const unsigned char dig[32], const char *name) {
    emit_hex(dig, 32);
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

static int sha256_fd(long fd, unsigned char dig[32]) {
    struct sha256_ctx ctx;
    char buf[512];
    long n;
    sha256_init(&ctx);
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        sha256_update(&ctx, (const unsigned char *)buf, n);
    }
    if (n < 0) return 1;
    sha256_final(&ctx, dig);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    unsigned char dig[32];

    if (i >= argc) {
        if (sha256_fd(0, dig) != 0) return 1;
        print_one(dig, 0);
        return 0;
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (sha256_fd(0, dig) != 0) {
                rc = 1;
                continue;
            }
            print_one(dig, 0);
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "sha256sum: open failed\n", 23);
            rc = 1;
            continue;
        }
        if (sha256_fd(fd, dig) != 0) {
            u_close(fd);
            rc = 1;
            continue;
        }
        u_close(fd);
        print_one(dig, argv[i]);
    }
    return rc;
}
