/* fakeos userland minimal libc: syscall-backed I/O, string/mem routines,
 * printf and a brk-based bump/first-fit allocator. No preprocessor, no
 * external headers - fakecc package ecosystem only. */
package user;

extern long u_rt_sigaction(long sig, void *act, void *oldact, unsigned long setsize);

typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

extern long u_write(long fd, const void *buf, unsigned long len);
extern unsigned long u_brk(unsigned long addr);

/* ---------------------------------------------------------------------------
 * Memory / string routines
 * ------------------------------------------------------------------------- */

void *memset(void *dst, int c, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned long i;
    for (i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    unsigned long i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        unsigned long i;
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        while (n > 0) {
            n--;
            d[n] = s[n];
        }
    }
    return dst;
}

unsigned long strlen(const char *s) {
    unsigned long n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, unsigned long n) {
    unsigned long i = 0;
    while (i < n && a[i] != 0 && a[i] == b[i]) {
        i++;
    }
    if (i == n) {
        return 0;
    }
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

char *strcpy(char *dst, const char *src) {
    unsigned long i = 0;
    while (src[i] != 0) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return dst;
}

char *strncpy(char *dst, const char *src, unsigned long n) {
    unsigned long i = 0;
    while (i < n && src[i] != 0) {
        dst[i] = src[i];
        i++;
    }
    while (i < n) {
        dst[i] = 0;
        i++;
    }
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s != 0) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return c == 0 ? (char *)s : 0;
}

/* Parse a non-negative decimal; skips leading digits and stops at the first
 * non-digit. Enough for "exit [code]" / $? usage. */
long strtol_dec(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* In-place tokenizer: splits on spaces/tabs/CR/LF, skips runs of whitespace,
 * returns the next token (NUL-terminated inside the original buffer). */
char *strtok_ws(char **save) {
    char *s = *save;
    char *out;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    if (*s == 0) {
        *save = s;
        return 0;
    }
    out = s;
    while (*s != 0 && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
        s++;
    }
    if (*s != 0) {
        *s = 0;
        s++;
    }
    *save = s;
    return out;
}

/* ---------------------------------------------------------------------------
 * Formatted output (write(2) backed)
 * ------------------------------------------------------------------------- */

static void pf_udec(u64 v, char *buf, long *n) {
    char tmp[24];
    long i = 0;
    long k;
    if (v == 0) {
        buf[(*n)++] = '0';
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (k = i - 1; k >= 0; k--) {
        buf[(*n)++] = tmp[k];
    }
}

static void pf_hex(u64 v, char *buf, long *n) {
    const char *d = "0123456789abcdef";
    char tmp[20];
    long i = 0;
    long k;
    if (v == 0) {
        buf[(*n)++] = '0';
        return;
    }
    while (v > 0) {
        tmp[i++] = d[v & 0xF];
        v >>= 4;
    }
    for (k = i - 1; k >= 0; k--) {
        buf[(*n)++] = tmp[k];
    }
}

static long vformat(char *buf, const char *fmt, va_list ap) {
    long n = 0;
    while (*fmt != 0) {
        if (*fmt != '%') {
            buf[n++] = *fmt;
            fmt++;
            continue;
        }
        fmt++;
        /* Length modifiers are accepted but ignored: every integer
         * argument arrives in a 64-bit slot on x86-64. */
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h' ||
               *fmt == 'j' || *fmt == 't') {
            fmt++;
        }
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            if (s == 0) {
                s = "(null)";
            }
            while (*s != 0) {
                buf[n++] = *s;
                s++;
            }
        } else if (*fmt == 'd' || *fmt == 'i') {
            i64 v = (i64)va_arg(ap, long long);
            if (v < 0) {
                buf[n++] = '-';
                v = -v;
            }
            pf_udec((u64)v, buf, &n);
        } else if (*fmt == 'u') {
            u64 v = (u64)va_arg(ap, unsigned long long);
            pf_udec(v, buf, &n);
        } else if (*fmt == 'x' || *fmt == 'X') {
            u64 v = (u64)va_arg(ap, unsigned long long);
            pf_hex(v, buf, &n);
        } else if (*fmt == 'p') {
            u64 v = (u64)va_arg(ap, unsigned long long);
            const char *p = "0x";
            while (*p) {
                buf[n++] = *p++;
            }
            pf_hex(v, buf, &n);
        } else if (*fmt == 'c') {
            buf[n++] = (char)va_arg(ap, int);
        } else if (*fmt == '%') {
            buf[n++] = '%';
        } else if (*fmt == 0) {
            break;
        } else {
            buf[n++] = '%';
            buf[n++] = *fmt;
        }
        fmt++;
    }
    return n;
}

void printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    long n;
    va_start(ap, fmt);
    n = vformat(buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        u_write(1, buf, (unsigned long)n);
    }
}

void puts(const char *s) {
    unsigned long n = strlen(s);
    if (n > 0) {
        u_write(1, s, n);
    }
    u_write(1, "\n", 1);
}

void puterr(const char *s) {
    unsigned long n = strlen(s);
    if (n > 0) {
        u_write(2, s, n);
    }
    u_write(2, "\n", 1);
}

/* ---------------------------------------------------------------------------
 * Heap: 16-byte aligned first-fit allocator over the brk heap. Each block has
 * a 16-byte header; freed blocks go onto a singly linked free list and are
 * reused before the program break is extended.
 * ------------------------------------------------------------------------- */

struct block_hdr {
    struct block_hdr *next_free;
    unsigned long size;             /* usable payload size (16-aligned) */
    unsigned long used;
    unsigned long pad;
};

static struct block_hdr *free_list;
static unsigned long heap_ready;

static unsigned long align16(unsigned long v) {
    return (v + 15) & ~15UL;
}

void *malloc(unsigned long size) {
    unsigned long asize;
    struct block_hdr *b;
    struct block_hdr **pp;
    char *raw;

    if (size == 0) {
        size = 1;
    }
    asize = align16(size);

    /* First-fit scan of freed blocks. */
    pp = &free_list;
    while (*pp != 0) {
        b = *pp;
        if (b->size >= asize) {
            *pp = b->next_free;
            b->used = 1;
            return (char *)b + 16;
        }
        pp = &b->next_free;
    }

    /* Grow the program break by header + payload. */
    raw = (char *)u_brk(0);
    if (!heap_ready) {
        raw = (char *)u_brk((unsigned long)raw);
        heap_ready = 1;
    }
    if (u_brk((unsigned long)raw + 16 + asize) == (unsigned long)-1) {
        return 0;
    }
    b = (struct block_hdr *)raw;
    b->next_free = 0;
    b->size = asize;
    b->used = 1;
    return (char *)b + 16;
}

void free(void *ptr) {
    struct block_hdr *b;
    if (ptr == 0) {
        return;
    }
    b = (struct block_hdr *)((char *)ptr - 16);
    b->used = 0;
    b->next_free = free_list;
    free_list = b;
}

/* ---------------------------------------------------------------------------
 * Host resolution: dotted IPv4, /etc/hosts, then DNS A query via UDP.
 * Soft DHCP is not implemented; QEMU user-net gives a static lease and we
 * publish /etc/resolv.conf with nameserver 10.0.2.2.
 * ------------------------------------------------------------------------- */

extern long u_open(const char *path, long flags, long mode);
extern long u_read(long fd, void *buf, unsigned long len);
extern long u_close(long fd);
extern long u_socket(long domain, long type, long protocol);
extern long u_bind(long fd, const void *addr, unsigned long alen);
extern long u_sendto(long fd, const void *buf, unsigned long len, long flags,
                     const void *addr, unsigned long alen);
extern long u_recvfrom(long fd, void *buf, unsigned long len, long flags,
                       void *addr, unsigned int *alen);
extern long u_yield(void);

static int parse_ipv4_lit(const char *s, unsigned int *out) {
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
        if (c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        return 0;
    }
    if (n != 4) return 0;
    *out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return 1;
}

static int hosts_lookup(const char *name, unsigned int *out) {
    char buf[512];
    long fd;
    long n;
    long i;
    fd = u_open("/etc/hosts", 0, 0);
    if (fd < 0) return 0;
    n = u_read(fd, buf, sizeof(buf) - 1);
    u_close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    i = 0;
    while (i < n) {
        char *line = buf + i;
        char *ip;
        char *host;
        unsigned int addr;
        while (i < n && buf[i] != '\n') i++;
        if (i < n) {
            buf[i] = 0;
            i++;
        }
        if (line[0] == '#' || line[0] == 0) continue;
        ip = line;
        while (*line != 0 && *line != ' ' && *line != '\t') line++;
        if (*line == 0) continue;
        *line = 0;
        line++;
        while (*line == ' ' || *line == '\t') line++;
        if (!parse_ipv4_lit(ip, &addr)) continue;
        while (*line != 0) {
            host = line;
            while (*line != 0 && *line != ' ' && *line != '\t') line++;
            if (*line != 0) {
                *line = 0;
                line++;
            }
            if (strcmp(host, name) == 0) {
                *out = addr;
                return 1;
            }
            while (*line == ' ' || *line == '\t') line++;
        }
    }
    return 0;
}

static unsigned int read_nameserver(void) {
    char buf[128];
    long fd;
    long n;
    unsigned int ns = 0x0A000202u; /* 10.0.2.2 default */
    fd = u_open("/etc/resolv.conf", 0, 0);
    if (fd < 0) return ns;
    n = u_read(fd, buf, sizeof(buf) - 1);
    u_close(fd);
    if (n <= 0) return ns;
    buf[n] = 0;
    {
        char *p = buf;
        const char *key = "nameserver";
        int k;
        while (*p != 0) {
            for (k = 0; key[k] != 0 && p[k] == key[k]; k++) {
            }
            if (key[k] == 0 && (p[k] == ' ' || p[k] == '\t')) {
                p += k;
                while (*p == ' ' || *p == '\t') p++;
                parse_ipv4_lit(p, &ns);
                return ns;
            }
            while (*p != 0 && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }
    return ns;
}

static int dns_query_a(const char *name, unsigned int nameserver, unsigned int *out) {
    unsigned char q[512];
    unsigned char resp[512];
    unsigned char sa[16];
    unsigned char bindsa[16];
    unsigned int alen = 16;
    long s;
    long n;
    int qi;
    int i;
    const char *p;
    unsigned short id = 0x4F53;
    for (i = 0; i < 16; i++) {
        sa[i] = 0;
        bindsa[i] = 0;
    }
    sa[0] = 2; /* AF_INET */
    sa[2] = 0;
    sa[3] = 53;
    sa[4] = (unsigned char)(nameserver >> 24);
    sa[5] = (unsigned char)(nameserver >> 16);
    sa[6] = (unsigned char)(nameserver >> 8);
    sa[7] = (unsigned char)(nameserver & 0xFF);
    bindsa[0] = 2;
    /* Header */
    q[0] = (unsigned char)(id >> 8);
    q[1] = (unsigned char)(id & 0xFF);
    q[2] = 0x01; q[3] = 0x00; /* RD */
    q[4] = 0; q[5] = 1;       /* QDCOUNT */
    q[6] = 0; q[7] = 0;
    q[8] = 0; q[9] = 0;
    q[10] = 0; q[11] = 0;
    qi = 12;
    p = name;
    while (*p != 0) {
        const char *dot = p;
        int lab;
        while (*dot != 0 && *dot != '.') dot++;
        lab = (int)(dot - p);
        if (lab <= 0 || lab > 63 || qi + 1 + lab >= 500) return 0;
        q[qi++] = (unsigned char)lab;
        for (i = 0; i < lab; i++) q[qi++] = (unsigned char)p[i];
        if (*dot == '.') p = dot + 1;
        else p = dot;
    }
    q[qi++] = 0;
    q[qi++] = 0; q[qi++] = 1; /* A */
    q[qi++] = 0; q[qi++] = 1; /* IN */

    s = u_socket(2, 2, 0); /* AF_INET, SOCK_DGRAM */
    if (s < 0) return 0;
    if (u_bind(s, bindsa, 16) != 0) {
        u_close(s);
        return 0;
    }
    if (u_sendto(s, q, (unsigned long)qi, 0, sa, 16) != qi) {
        u_close(s);
        return 0;
    }
    n = u_recvfrom(s, resp, sizeof(resp), 0, 0, 0);
    u_close(s);
    if (n < 12) return 0;
    if (resp[0] != q[0] || resp[1] != q[1]) return 0;
    /* Skip question, then walk answers for type A. */
    {
        int ancount = ((int)resp[6] << 8) | (int)resp[7];
        int off = 12;
        /* skip QNAME */
        while (off < n && resp[off] != 0) {
            if ((resp[off] & 0xC0) == 0xC0) { off += 2; break; }
            off += 1 + resp[off];
        }
        if (off < n && resp[off] == 0) off++;
        off += 4; /* QTYPE+QCLASS */
        while (ancount > 0 && off + 12 <= n) {
            int typ;
            int rdlen;
            if ((resp[off] & 0xC0) == 0xC0) off += 2;
            else {
                while (off < n && resp[off] != 0) {
                    if ((resp[off] & 0xC0) == 0xC0) { off += 2; break; }
                    off += 1 + resp[off];
                }
                if (off < n && resp[off] == 0) off++;
            }
            if (off + 10 > n) return 0;
            typ = ((int)resp[off] << 8) | (int)resp[off + 1];
            rdlen = ((int)resp[off + 8] << 8) | (int)resp[off + 9];
            off += 10;
            if (typ == 1 && rdlen == 4 && off + 4 <= n) {
                *out = ((unsigned int)resp[off] << 24) |
                       ((unsigned int)resp[off + 1] << 16) |
                       ((unsigned int)resp[off + 2] << 8) |
                       (unsigned int)resp[off + 3];
                return 1;
            }
            off += rdlen;
            ancount--;
        }
    }
    (void)alen;
    return 0;
}

/* Resolve name to host-order IPv4. Returns 1 on success. */
int resolve_host(const char *name, unsigned int *out) {
    unsigned int ns;
    if (name == 0 || out == 0 || name[0] == 0) return 0;
    if (parse_ipv4_lit(name, out)) return 1;
    if (hosts_lookup(name, out)) return 1;
    ns = read_nameserver();
    if (dns_query_a(name, ns, out)) return 1;
    return 0;
}

/* Quote-aware tokenizer: supports "..." and '...' (no escapes). */
char *strtok_sh(char **save) {
    char *s = *save;
    char *out;
    char quote;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == 0) {
        *save = s;
        return 0;
    }
    if (*s == '"' || *s == '\'') {
        quote = *s;
        s++;
        out = s;
        while (*s != 0 && *s != quote) s++;
        if (*s == quote) {
            *s = 0;
            s++;
        }
        *save = s;
        return out;
    }
    out = s;
    while (*s != 0 && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') s++;
    if (*s != 0) {
        *s = 0;
        s++;
    }
    *save = s;
    return out;
}

/* ---- signals (stub) ----------------------------------------------------- */
enum { SIG_DFL_V = 0, SIG_IGN_V = 1 };

struct sigaction {
    void *sa_handler;
    unsigned long sa_flags;
    void *sa_restorer;
    unsigned long sa_mask;
};

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    struct sigaction tmp;
    struct sigaction old;
    long r;
    void *ap = 0;
    void *op = 0;
    if (act) {
        tmp = *act;
        ap = &tmp;
    }
    if (oldact) {
        op = &old;
    }
    r = u_rt_sigaction((long)sig, ap, op, 8);
    if (r < 0) {
        return -1;
    }
    if (oldact) {
        oldact->sa_handler = old.sa_handler;
        oldact->sa_flags = 0;
        oldact->sa_restorer = 0;
        oldact->sa_mask = 0;
    }
    return 0;
}

void *signal(int sig, void *handler) {
    struct sigaction act;
    struct sigaction old;
    act.sa_handler = handler;
    act.sa_flags = 0;
    act.sa_restorer = 0;
    act.sa_mask = 0;
    if (sigaction(sig, &act, &old) != 0) {
        return (void *)(long)-1;
    }
    return old.sa_handler;
}

/* ---- auth crypt stub ---------------------------------------------------- */
/* Format: $fake$<4-char-salt>$<8-hex> where hash = FNV-1a(salt || ":" || key). */
static unsigned int fnv1a(const char *s) {
    unsigned int h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)(*s);
        h *= 16777619u;
        s++;
    }
    return h;
}

static void hex8(unsigned int v, char *out) {
    const char *h = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++) {
        out[7 - i] = h[v & 0xf];
        v >>= 4;
    }
    out[8] = 0;
}

char *crypt(const char *key, const char *salt) {
    static char out[32];
    char buf[96];
    unsigned int i;
    unsigned int h;
    const char *sp = salt;
    if (key == 0) key = "";
    if (sp == 0 || sp[0] == 0) {
        sp = "$fake$abcd$";
    }
    /* Accept full $fake$salt$hash or $fake$salt$ as setting salt. */
    if (sp[0] == '$' && sp[1] == 'f') {
        /* copy "$fake$XXXX$" prefix into out */
        i = 0;
        while (sp[i] && i < 11) {
            out[i] = sp[i];
            i++;
        }
        /* ensure form $fake$ssss$ */
        if (i < 11) {
            while (i < 6) out[i++] = 'a';
            out[0] = '$'; out[1] = 'f'; out[2] = 'a'; out[3] = 'k';
            out[4] = 'e'; out[5] = '$';
            out[6] = 'a'; out[7] = 'b'; out[8] = 'c'; out[9] = 'd';
            out[10] = '$';
            i = 11;
        }
        out[11] = 0;
    } else {
        out[0] = '$'; out[1] = 'f'; out[2] = 'a'; out[3] = 'k';
        out[4] = 'e'; out[5] = '$';
        out[6] = sp[0] ? sp[0] : 'a';
        out[7] = sp[1] ? sp[1] : 'b';
        out[8] = sp[2] ? sp[2] : 'c';
        out[9] = sp[3] ? sp[3] : 'd';
        out[10] = '$';
        out[11] = 0;
        i = 11;
    }
    /* buf = salt4 + ":" + key */
    buf[0] = out[6]; buf[1] = out[7]; buf[2] = out[8]; buf[3] = out[9];
    buf[4] = ':';
    {
        unsigned int k = 0;
        while (key[k] && k < 80) {
            buf[5 + k] = key[k];
            k++;
        }
        buf[5 + k] = 0;
    }
    h = fnv1a(buf);
    hex8(h, out + 11);
    return out;
}

int crypt_check(const char *key, const char *setting) {
    char *got;
    if (setting == 0 || setting[0] == 0) {
        return key != 0 && key[0] == 0; /* empty hash => empty password only */
    }
    if (setting[0] != '$') {
        /* legacy plaintext field: match exact or (root aliases handled by login) */
        return key != 0 && strcmp(key, setting) == 0;
    }
    got = crypt(key, setting);
    return strcmp(got, setting) == 0;
}
