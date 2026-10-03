/* dumpbin - dump a file from inside fakeos to the serial console as
 * length-framed base64. The host test harness decodes the frames to
 * pull /tmp/fakeos-new (the in-OS linked kernel) back to a host file
 * for a second QEMU boot.
 *
 * Frame layout (one frame per 12 KiB chunk):
 *   ===DUMP/BEGIN name=<n> len=<bytes>===
 *   <base64, 76 cols>
 *   ===DUMP/END===
 *
 * FakeOS user program: minimal fakeos libc. */
package user;

extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_read(long fd, void *buf, unsigned long len);
extern long u_write(long fd, const void *buf, unsigned long len);
extern void u_exit(long code);

extern void printf(const char *fmt, ...);
extern void puts(const char *s);

enum { O_RDONLY = 0, CHUNK = 12288, LINE = 76 };

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void emit_u32(unsigned long v, char *out) {
    out[0] = (char)('0' + (v / 1000000000UL) % 10);
    out[1] = (char)('0' + (v / 100000000UL) % 10);
    out[2] = (char)('0' + (v / 10000000UL) % 10);
    out[3] = (char)('0' + (v / 1000000UL) % 10);
    out[4] = (char)('0' + (v / 100000UL) % 10);
    out[5] = (char)('0' + (v / 10000UL) % 10);
    out[6] = (char)('0' + (v / 1000UL) % 10);
    out[7] = (char)('0' + (v / 100UL) % 10);
    out[8] = (char)('0' + (v / 10UL) % 10);
    out[9] = (char)('0' + v % 10);
    int i = 0;
    while (i < 9 && out[i] == '0') {
        i++;
    }
    /* shift left in place (no memmove in dialect issues) */
    if (i) {
        int j;
        for (j = 0; j < 10 - i; j++) {
            out[j] = out[j + i];
        }
        out[10 - i] = 0;
    } else {
        out[10] = 0;
    }
}

int main(int argc, char **argv) {
    /* Both buffers are static: the base64 output is ~16 KiB per frame,
     * larger than what user stacks are sized for. */
    static unsigned char in[CHUNK];
    static char out[((CHUNK + 2) / 3) * 4 + 4];
    const char *path = argv[1];
    long fd;
    long total = 0;
    if (argc < 2) {
        puts("usage: dumpbin <path>");
        return 1;
    }
    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("dumpbin: cannot open %s\n", (long)path);
        return 1;
    }
    for (;;) {
        long n = u_read(fd, in, CHUNK);
        unsigned long i;
        unsigned long oi;
        if (n <= 0) {
            break;
        }
        char hdr[64];
        char num[16];
        unsigned long h = 0;
        const char *p;
        emit_u32((unsigned long)n, num);
        p = "===DUMP/BEGIN name=";
        while (*p) { hdr[h++] = *p++; }
        p = "fakeos-new len=";
        while (*p) { hdr[h++] = *p++; }
        {
            unsigned long k = 0;
            while (num[k]) { hdr[h++] = num[k++]; }
        }
        hdr[h++] = '=';
        hdr[h++] = '=';
        hdr[h++] = '=';
        hdr[h++] = '\n';
        u_write(1, hdr, h);

        oi = 0;
        for (i = 0; i + 3 <= (unsigned long)n; i += 3) {
            unsigned long v = ((unsigned long)in[i] << 16) |
                             ((unsigned long)in[i + 1] << 8) |
                             (unsigned long)in[i + 2];
            out[oi++] = b64tab[(v >> 18) & 63];
            out[oi++] = b64tab[(v >> 12) & 63];
            out[oi++] = b64tab[(v >> 6) & 63];
            out[oi++] = b64tab[v & 63];
        }
        if ((unsigned long)n - i == 1) {
            unsigned long v = (unsigned long)in[i] << 16;
            out[oi++] = b64tab[(v >> 18) & 63];
            out[oi++] = b64tab[(v >> 12) & 63];
            out[oi++] = '=';
            out[oi++] = '=';
        } else if ((unsigned long)n - i == 2) {
            unsigned long v = ((unsigned long)in[i] << 16) |
                             ((unsigned long)in[i + 1] << 8);
            out[oi++] = b64tab[(v >> 18) & 63];
            out[oi++] = b64tab[(v >> 12) & 63];
            out[oi++] = b64tab[(v >> 6) & 63];
            out[oi++] = '=';
        }
        out[oi++] = '\n';
        u_write(1, out, oi);
        u_write(1, "===DUMP/END===\n", 14);
        total += n;
    }
    u_close(fd);
    printf("dumpbin: sent %ld bytes\n", total);
    return total ? 0 : 1;
}
