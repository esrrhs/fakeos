/* fakeos userland minimal libc: syscall-backed I/O, string/mem routines,
 * printf and a brk-based bump/first-fit allocator. No preprocessor, no
 * external headers - fakecc package ecosystem only. */
package user;

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
        } else if (*fmt == 'c') {
            buf[n++] = (char)va_arg(ap, int);
        } else if (*fmt == '%') {
            buf[n++] = '%';
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
