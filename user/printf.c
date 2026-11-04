/* fakeos userland - printf: format ARGS with escapes (thin POSIX subset).
 *   printf FORMAT [ARGUMENT...]
 * Format escapes: \n \t \r \a \b \f \v \\ \% \0ooo \c (stop). Specifiers:
 * %s %c %d %i %u %x %X %o %% and %b (ARGUMENT with the same escapes; \c stops
 * all further output). Thin width/flags: %5d %04d %-10s (also u/x/X/o).
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

typedef unsigned long u64;
typedef long long i64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern long strtol_dec(const char *s);

enum { PAD_CAP = 128 };

static void out_ch(char c) {
    u_write(1, &c, 1);
}

static void out_str(const char *s) {
    unsigned long n;
    if (s == 0) s = "(null)";
    n = strlen(s);
    if (n > 0) u_write(1, s, n);
}

static void out_nch(char c, int n) {
    while (n > 0) {
        out_ch(c);
        n--;
    }
}

/* Emit buf[0..len) with width/left/zero padding. */
static void out_pad(const char *buf, int len, int width, int left, int zero) {
    int pad;
    if (width < 0) width = 0;
    if (len < 0) len = 0;
    pad = width - len;
    if (pad < 0) pad = 0;
    if (left) {
        if (len > 0) u_write(1, buf, (unsigned long)len);
        out_nch(' ', pad);
    } else if (zero && pad > 0) {
        /* Zero-pad only for numeric (caller); sign already in buf if any. */
        int si = 0;
        if (len > 0 && (buf[0] == '-' || buf[0] == '+')) {
            out_ch(buf[0]);
            si = 1;
            len--;
        }
        out_nch('0', pad);
        if (len > 0) u_write(1, buf + si, (unsigned long)len);
    } else {
        out_nch(' ', pad);
        if (len > 0) u_write(1, buf, (unsigned long)len);
    }
}

static int fmt_udec(char *tmp, int cap, u64 v) {
    char rev[24];
    int i = 0;
    int k;
    int n = 0;
    if (cap < 2) return 0;
    if (v == 0) {
        tmp[0] = '0';
        tmp[1] = 0;
        return 1;
    }
    while (v > 0 && i < 24) {
        rev[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (k = i - 1; k >= 0 && n + 1 < cap; k--) tmp[n++] = rev[k];
    tmp[n] = 0;
    return n;
}

static int fmt_hex(char *tmp, int cap, u64 v, int upper) {
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[20];
    int i = 0;
    int k;
    int n = 0;
    if (cap < 2) return 0;
    if (v == 0) {
        tmp[0] = '0';
        tmp[1] = 0;
        return 1;
    }
    while (v > 0 && i < 20) {
        rev[i++] = d[v & 0xF];
        v >>= 4;
    }
    for (k = i - 1; k >= 0 && n + 1 < cap; k--) tmp[n++] = rev[k];
    tmp[n] = 0;
    return n;
}

static int fmt_oct(char *tmp, int cap, u64 v) {
    char rev[24];
    int i = 0;
    int k;
    int n = 0;
    if (cap < 2) return 0;
    if (v == 0) {
        tmp[0] = '0';
        tmp[1] = 0;
        return 1;
    }
    while (v > 0 && i < 24) {
        rev[i++] = (char)('0' + (v & 7));
        v >>= 3;
    }
    for (k = i - 1; k >= 0 && n + 1 < cap; k--) tmp[n++] = rev[k];
    tmp[n] = 0;
    return n;
}

static int parse_oct3(const char *s, int *consumed) {
    int v = 0;
    int n = 0;
    while (n < 3 && s[n] >= '0' && s[n] <= '7') {
        v = (v << 3) + (s[n] - '0');
        n++;
    }
    *consumed = n;
    return v;
}

/* Expand backslash escapes from src into out (NUL-terminated).
 * Returns 1 if \c stopped expansion (remaining src ignored), else 0. */
static int expand_escapes(const char *src, char *out, int cap) {
    int n = 0;
    int stopped = 0;
    while (*src && n + 1 < cap) {
        if (*src != '\\') {
            out[n++] = *src++;
            continue;
        }
        src++;
        if (*src == 0) {
            out[n++] = '\\';
            break;
        }
        if (*src == 'c') {
            /* \c: produce no further output from this string / printf. */
            src++;
            stopped = 1;
            break;
        }
        if (*src == 'n') { out[n++] = '\n'; src++; }
        else if (*src == 't') { out[n++] = '\t'; src++; }
        else if (*src == 'r') { out[n++] = '\r'; src++; }
        else if (*src == 'a') { out[n++] = '\a'; src++; }
        else if (*src == 'b') { out[n++] = '\b'; src++; }
        else if (*src == 'f') { out[n++] = '\f'; src++; }
        else if (*src == 'v') { out[n++] = '\v'; src++; }
        else if (*src == '\\') { out[n++] = '\\'; src++; }
        else if (*src == '%') { out[n++] = '%'; src++; }
        else if (*src >= '0' && *src <= '7') {
            int cons = 0;
            int v = parse_oct3(src, &cons);
            out[n++] = (char)v;
            src += cons;
        } else {
            out[n++] = *src++;
        }
    }
    out[n] = 0;
    return stopped;
}

int main(int argc, char **argv) {
    char expanded[1024];
    char bbuf[1024];
    char numbuf[40];
    const char *fmt;
    int ai;
    int i;
    int fmt_stop;

    if (argc < 2) {
        return 0;
    }
    fmt_stop = expand_escapes(argv[1], expanded, (int)sizeof(expanded));
    fmt = expanded;
    ai = 2;

    for (i = 0; fmt[i]; ) {
        int left = 0;
        int zero = 0;
        int width = 0;
        char spec;

        if (fmt[i] != '%') {
            out_ch(fmt[i++]);
            continue;
        }
        i++;
        if (fmt[i] == 0) {
            out_ch('%');
            break;
        }
        if (fmt[i] == '%') {
            out_ch('%');
            i++;
            continue;
        }
        /* flags */
        while (fmt[i] == '-' || fmt[i] == '0') {
            if (fmt[i] == '-') left = 1;
            else zero = 1;
            i++;
        }
        if (left) zero = 0; /* '-' wins over '0' */
        /* width */
        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i] - '0');
            if (width > PAD_CAP) width = PAD_CAP;
            i++;
        }
        if (fmt[i] == 0) {
            out_ch('%');
            break;
        }
        spec = fmt[i++];
        if (spec == 's') {
            const char *a = (ai < argc) ? argv[ai] : "";
            int n;
            if (ai < argc) ai++;
            if (a == 0) a = "";
            n = (int)strlen(a);
            out_pad(a, n, width, left, 0);
        } else if (spec == 'c') {
            const char *a = (ai < argc) ? argv[ai] : "";
            char cb[2];
            if (ai < argc) ai++;
            cb[0] = a[0];
            cb[1] = 0;
            out_pad(cb, a[0] ? 1 : 0, width, left, 0);
        } else if (spec == 'b') {
            const char *a = (ai < argc) ? argv[ai] : "";
            int bstop;
            int n;
            if (ai < argc) ai++;
            bstop = expand_escapes(a, bbuf, (int)sizeof(bbuf));
            n = (int)strlen(bbuf);
            out_pad(bbuf, n, width, left, 0);
            if (bstop) {
                return 0;
            }
        } else if (spec == 'd' || spec == 'i') {
            long v = 0;
            const char *a = (ai < argc) ? argv[ai] : "0";
            int neg = 0;
            int n = 0;
            if (ai < argc) ai++;
            if (a[0] == '-') { neg = 1; a++; }
            v = strtol_dec(a);
            if (neg) {
                numbuf[0] = '-';
                n = 1 + fmt_udec(numbuf + 1, (int)sizeof(numbuf) - 1, (u64)v);
            } else {
                n = fmt_udec(numbuf, (int)sizeof(numbuf), (u64)v);
            }
            out_pad(numbuf, n, width, left, zero);
        } else if (spec == 'u') {
            const char *a = (ai < argc) ? argv[ai] : "0";
            int n;
            if (ai < argc) ai++;
            n = fmt_udec(numbuf, (int)sizeof(numbuf), (u64)strtol_dec(a));
            out_pad(numbuf, n, width, left, zero);
        } else if (spec == 'x' || spec == 'X') {
            const char *a = (ai < argc) ? argv[ai] : "0";
            int n;
            if (ai < argc) ai++;
            n = fmt_hex(numbuf, (int)sizeof(numbuf), (u64)strtol_dec(a),
                        spec == 'X');
            out_pad(numbuf, n, width, left, zero);
        } else if (spec == 'o') {
            const char *a = (ai < argc) ? argv[ai] : "0";
            int n;
            if (ai < argc) ai++;
            n = fmt_oct(numbuf, (int)sizeof(numbuf), (u64)strtol_dec(a));
            out_pad(numbuf, n, width, left, zero);
        } else {
            /* Unknown: echo % and char literally (no width consume). */
            out_ch('%');
            out_ch(spec);
        }
    }
    (void)fmt_stop;
    return 0;
}
