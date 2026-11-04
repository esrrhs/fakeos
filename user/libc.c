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
 * Host resolution: dotted IPv4, /etc/hosts, then DNS A via UDP.
 * Richer stub: multiple nameservers, search list, short retries, tiny cache.
 * Kernel DHCP fills the lease; /etc/resolv.conf ships QEMU user-net defaults.
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

static unsigned int dns_cache_ip[8];
static char dns_cache_name[8][64];
static int dns_cache_used;

static int dns_cache_get(const char *name, unsigned int *out) {
    int i;
    for (i = 0; i < dns_cache_used; i++) {
        if (strcmp(dns_cache_name[i], name) == 0) {
            *out = dns_cache_ip[i];
            return 1;
        }
    }
    return 0;
}

static void dns_cache_put(const char *name, unsigned int ip) {
    int n = 0;
    int i;
    while (name[n] && n < 63) n++;
    if (n == 0) return;
    if (dns_cache_used < 8) {
        i = dns_cache_used++;
    } else {
        i = 0; /* simple overwrite slot 0 */
    }
    for (n = 0; name[n] && n < 63; n++) dns_cache_name[i][n] = name[n];
    dns_cache_name[i][n] = 0;
    dns_cache_ip[i] = ip;
}

/* Collect up to 4 nameservers + up to 4 search domains from resolv.conf. */
static void load_resolv(unsigned int *ns, int *nns, char search[][64], int *nsearch) {
    char buf[256];
    long fd;
    long n;
    long i;
    *nns = 0;
    *nsearch = 0;
    ns[0] = 0x0A000202u;
    fd = u_open("/etc/resolv.conf", 0, 0);
    if (fd < 0) {
        *nns = 1;
        return;
    }
    n = u_read(fd, buf, sizeof(buf) - 1);
    u_close(fd);
    if (n <= 0) {
        *nns = 1;
        return;
    }
    buf[n] = 0;
    i = 0;
    while (i < n) {
        char *line = buf + i;
        while (i < n && buf[i] != '\n') i++;
        if (i < n) {
            buf[i] = 0;
            i++;
        }
        if (line[0] == '#' || line[0] == 0) continue;
        if (line[0] == 'n' && line[1] == 'a' && line[2] == 'm' &&
            line[3] == 'e' && line[4] == 's' && line[5] == 'e' &&
            line[6] == 'r' && line[7] == 'v' && line[8] == 'e' &&
            line[9] == 'r' && (line[10] == ' ' || line[10] == '\t')) {
            char *p = line + 10;
            unsigned int addr;
            while (*p == ' ' || *p == '\t') p++;
            if (*nns < 4 && parse_ipv4_lit(p, &addr)) {
                ns[*nns] = addr;
                (*nns)++;
            }
        } else if ((line[0] == 's' && line[1] == 'e' && line[2] == 'a' &&
                    line[3] == 'r' && line[4] == 'c' && line[5] == 'h' &&
                    (line[6] == ' ' || line[6] == '\t')) ||
                   (line[0] == 'd' && line[1] == 'o' && line[2] == 'm' &&
                    line[3] == 'a' && line[4] == 'i' && line[5] == 'n' &&
                    (line[6] == ' ' || line[6] == '\t'))) {
            char *p = line + 6;
            while (*p == ' ' || *p == '\t') p++;
            while (*p != 0 && *nsearch < 4) {
                int k = 0;
                while (*p != 0 && *p != ' ' && *p != '\t') {
                    if (k < 63) search[*nsearch][k++] = *p;
                    p++;
                }
                search[*nsearch][k] = 0;
                if (k > 0) (*nsearch)++;
                while (*p == ' ' || *p == '\t') p++;
            }
        }
    }
    if (*nns == 0) {
        ns[0] = 0x0A000202u;
        *nns = 1;
    }
}

static int dns_query_a(const char *name, unsigned int nameserver, unsigned int *out) {
    unsigned char q[512];
    unsigned char resp[512];
    unsigned char sa[16];
    unsigned char bindsa[16];
    long s;
    long n;
    int qi;
    int i;
    int attempt;
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
    for (attempt = 0; attempt < 2; attempt++) {
        if (u_sendto(s, q, (unsigned long)qi, 0, sa, 16) != qi) {
            continue;
        }
        n = u_recvfrom(s, resp, sizeof(resp), 0, 0, 0);
        if (n < 12) {
            u_yield();
            continue;
        }
        if (resp[0] != q[0] || resp[1] != q[1]) continue;
        {
            int ancount = ((int)resp[6] << 8) | (int)resp[7];
            int off = 12;
            while (off < n && resp[off] != 0) {
                if ((resp[off] & 0xC0) == 0xC0) { off += 2; break; }
                off += 1 + resp[off];
            }
            if (off < n && resp[off] == 0) off++;
            off += 4;
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
                if (off + 10 > n) break;
                typ = ((int)resp[off] << 8) | (int)resp[off + 1];
                rdlen = ((int)resp[off + 8] << 8) | (int)resp[off + 9];
                off += 10;
                if (typ == 1 && rdlen == 4 && off + 4 <= n) {
                    *out = ((unsigned int)resp[off] << 24) |
                           ((unsigned int)resp[off + 1] << 16) |
                           ((unsigned int)resp[off + 2] << 8) |
                           (unsigned int)resp[off + 3];
                    u_close(s);
                    return 1;
                }
                /* Skip CNAME / other RRs and keep walking. */
                off += rdlen;
                ancount--;
            }
        }
    }
    u_close(s);
    return 0;
}

static int resolve_one(const char *name, unsigned int *out) {
    unsigned int ns[4];
    int nns = 0;
    char search[4][64];
    int nsearch = 0;
    int i;
    if (dns_cache_get(name, out)) return 1;
    if (hosts_lookup(name, out)) {
        dns_cache_put(name, *out);
        return 1;
    }
    load_resolv(ns, &nns, search, &nsearch);
    for (i = 0; i < nns; i++) {
        if (dns_query_a(name, ns[i], out)) {
            dns_cache_put(name, *out);
            return 1;
        }
    }
    (void)nsearch;
    (void)search;
    return 0;
}

/* Resolve name to host-order IPv4. Returns 1 on success. */
int resolve_host(const char *name, unsigned int *out) {
    unsigned int ns[4];
    int nns = 0;
    char search[4][64];
    int nsearch = 0;
    int i;
    int has_dot = 0;
    if (name == 0 || out == 0 || name[0] == 0) return 0;
    if (parse_ipv4_lit(name, out)) return 1;
    if (resolve_one(name, out)) return 1;
    for (i = 0; name[i]; i++) {
        if (name[i] == '.') has_dot = 1;
    }
    if (has_dot) return 0;
    load_resolv(ns, &nns, search, &nsearch);
    for (i = 0; i < nsearch; i++) {
        char fqdn[128];
        int a = 0;
        int b = 0;
        while (name[a] && a < 63) {
            fqdn[a] = name[a];
            a++;
        }
        fqdn[a++] = '.';
        while (search[i][b] && a < 126) {
            fqdn[a++] = search[i][b++];
        }
        fqdn[a] = 0;
        if (resolve_one(fqdn, out)) return 1;
    }
    (void)ns;
    (void)nns;
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

/* ---- signals (delivery frames + restorer) ------------------------------- */
enum {
    SIG_DFL_V = 0,
    SIG_IGN_V = 1,
    SA_RESTART = 0x10000000
};

extern long u_rt_sigreturn(void);

/* Kernel redirects handler return here; issues rt_sigreturn. */
void __sig_restorer(void) {
    u_rt_sigreturn();
    for (;;) {
    }
}

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
        if (tmp.sa_handler != (void *)SIG_DFL_V &&
            tmp.sa_handler != (void *)SIG_IGN_V &&
            tmp.sa_restorer == 0) {
            tmp.sa_restorer = (void *)__sig_restorer;
        }
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
        oldact->sa_flags = old.sa_flags;
        oldact->sa_restorer = old.sa_restorer;
        oldact->sa_mask = old.sa_mask;
    }
    return 0;
}

void *signal(int sig, void *handler) {
    struct sigaction act;
    struct sigaction old;
    act.sa_handler = handler;
    act.sa_flags = 0;
    act.sa_restorer = (void *)__sig_restorer;
    act.sa_mask = 0;
    if (sigaction(sig, &act, &old) != 0) {
        return (void *)(long)-1;
    }
    return old.sa_handler;
}


/* ---- wait status helpers (subset) --------------------------------------- */
enum {
    WNOHANG_V = 1,
    WUNTRACED_V = 2
};

int WIFEXITED(long st) { return (st & 0x7f) == 0; }
int WEXITSTATUS(long st) { return (int)((st >> 8) & 0xff); }
int WIFSTOPPED(long st) { return (st & 0xff) == 0x7f; }
int WSTOPSIG(long st) { return (int)((st >> 8) & 0xff); }
int WIFSIGNALED(long st) { return ((st & 0x7f) != 0) && ((st & 0x7f) != 0x7f); }

extern long u_fsync(long fd);
int fsync(int fd) {
    long r = u_fsync((long)fd);
    return r < 0 ? -1 : 0;
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

/* ---- POSIX shm path: named objects under /dev/shm + ftruncate ------------ */
extern long u_ftruncate(long fd, unsigned long length);
extern long u_unlink(const char *path);

enum {
    SHM_NAME_MAX = 64
};

int ftruncate(int fd, long length) {
    long r;
    if (length < 0) {
        return -1;
    }
    r = u_ftruncate((long)fd, (unsigned long)length);
    return r < 0 ? -1 : 0;
}

static int shm_path(char *dst, unsigned int cap, const char *name) {
    const char *prefix = "/dev/shm/";
    unsigned int i = 0;
    unsigned int j = 0;
    if (name == 0 || name[0] == 0 || name[0] == '/') {
        return -1;
    }
    while (prefix[i] != 0) {
        if (i + 1 >= cap) {
            return -1;
        }
        dst[i] = prefix[i];
        i++;
    }
    while (name[j] != 0) {
        if (name[j] == '/' || j >= SHM_NAME_MAX) {
            return -1;
        }
        if (i + 1 >= cap) {
            return -1;
        }
        dst[i++] = name[j++];
    }
    dst[i] = 0;
    return 0;
}

int shm_open(const char *name, int oflag, unsigned int mode) {
    char path[96];
    long fd;
    (void)mode;
    if (shm_path(path, (unsigned int)sizeof(path), name) != 0) {
        return -1;
    }
    fd = u_open(path, (long)oflag, 0);
    return fd < 0 ? -1 : (int)fd;
}

int shm_unlink(const char *name) {
    char path[96];
    long r;
    if (shm_path(path, (unsigned int)sizeof(path), name) != 0) {
        return -1;
    }
    r = u_unlink(path);
    return r < 0 ? -1 : 0;
}


/* ---- rename(2) ----------------------------------------------------------- */
extern long u_rename(const char *oldpath, const char *newpath);

int rename(const char *oldpath, const char *newpath) {
    long r;
    if (oldpath == 0 || newpath == 0) {
        return -1;
    }
    r = u_rename(oldpath, newpath);
    return (int)r;
}

/* ---- futex(2) basics ---------------------------------------------------- */
extern long u_futex(volatile unsigned int *uaddr, int op, unsigned int val,
                    void *timeout, volatile unsigned int *uaddr2,
                    unsigned int val3);

enum {
    FUTEX_WAIT = 0,
    FUTEX_WAKE = 1,
    FUTEX_PRIVATE = 128
};

long futex(volatile unsigned int *uaddr, int op, unsigned int val,
           void *timeout, volatile unsigned int *uaddr2, unsigned int val3) {
    return u_futex(uaddr, op, val, timeout, uaddr2, val3);
}

/* ---- pthread_mutex on futex (NPTL-style 0/1/2 word) ---------------------- */
extern unsigned int atomic_xchg(volatile unsigned int *p, unsigned int v);
extern unsigned int atomic_cmpxchg(volatile unsigned int *p, unsigned int expected,
                                  unsigned int desired);

typedef struct {
    volatile unsigned int state; /* 0 unlocked, 1 locked, 2 contended */
} pthread_mutex_t;

int pthread_mutex_init(pthread_mutex_t *m, void *attr) {
    (void)attr;
    if (m == 0) {
        return -1;
    }
    m->state = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    if (m == 0) {
        return -1;
    }
    m->state = 0;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    unsigned int old;
    if (m == 0) {
        return -1;
    }
    old = atomic_cmpxchg(&m->state, 0, 1);
    return old == 0 ? 0 : -1;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    unsigned int old;
    long r;
    if (m == 0) {
        return -1;
    }
    old = atomic_cmpxchg(&m->state, 0, 1);
    if (old == 0) {
        return 0;
    }
    for (;;) {
        if (old == 2 || atomic_cmpxchg(&m->state, 1, 2) != 0) {
            r = futex(&m->state, FUTEX_WAIT, 2, 0, 0, 0);
            while (r == -4) {
                r = futex(&m->state, FUTEX_WAIT, 2, 0, 0, 0);
            }
        }
        old = atomic_cmpxchg(&m->state, 0, 2);
        if (old == 0) {
            return 0;
        }
    }
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    unsigned int old;
    if (m == 0) {
        return -1;
    }
    old = atomic_xchg(&m->state, 0);
    if (old == 2) {
        futex(&m->state, FUTEX_WAKE, 1, 0, 0, 0);
    }
    return 0;
}
