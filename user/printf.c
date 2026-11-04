/* fakeos userland - printf: format ARGS with escapes (thin POSIX subset).
 *   printf FORMAT [ARGUMENT...]
 * Format escapes: \n \t \r \a \b \f \v \\ \% \0ooo \c (stop). Specifiers:
 * %s %c %d %i %u %x %X %o %% and %b (ARGUMENT with the same escapes; \c stops
 * all further output). Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

typedef unsigned long u64;
typedef long long i64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern long strtol_dec(const char *s);

static void out_ch(char c) {
    u_write(1, &c, 1);
}

static void out_str(const char *s) {
    unsigned long n;
    if (s == 0) s = "(null)";
    n = strlen(s);
    if (n > 0) u_write(1, s, n);
}

static void out_udec(u64 v) {
    char tmp[24];
    int i = 0;
    int k;
    if (v == 0) {
        out_ch('0');
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (k = i - 1; k >= 0; k--) out_ch(tmp[k]);
}

static void out_hex(u64 v) {
    const char *d = "0123456789abcdef";
    char tmp[20];
    int i = 0;
    int k;
    if (v == 0) {
        out_ch('0');
        return;
    }
    while (v > 0) {
        tmp[i++] = d[v & 0xF];
        v >>= 4;
    }
    for (k = i - 1; k >= 0; k--) out_ch(tmp[k]);
}

static void out_oct(u64 v) {
    char tmp[24];
    int i = 0;
    int k;
    if (v == 0) {
        out_ch('0');
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (v & 7));
        v >>= 3;
    }
    for (k = i - 1; k >= 0; k--) out_ch(tmp[k]);
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
        if (fmt[i] == 's') {
            out_str(ai < argc ? argv[ai] : "");
            if (ai < argc) ai++;
            i++;
        } else if (fmt[i] == 'c') {
            const char *a = (ai < argc) ? argv[ai] : "";
            out_ch(a[0]);
            if (ai < argc) ai++;
            i++;
        } else if (fmt[i] == 'b') {
            const char *a = (ai < argc) ? argv[ai] : "";
            int bstop;
            if (ai < argc) ai++;
            bstop = expand_escapes(a, bbuf, (int)sizeof(bbuf));
            out_str(bbuf);
            i++;
            if (bstop) {
                return 0;
            }
        } else if (fmt[i] == 'd' || fmt[i] == 'i') {
            long v = 0;
            const char *a = (ai < argc) ? argv[ai] : "0";
            int neg = 0;
            if (a[0] == '-') { neg = 1; a++; }
            v = strtol_dec(a);
            if (neg) {
                out_ch('-');
            }
            out_udec((u64)v);
            if (ai < argc) ai++;
            i++;
        } else if (fmt[i] == 'u') {
            const char *a = (ai < argc) ? argv[ai] : "0";
            out_udec((u64)strtol_dec(a));
            if (ai < argc) ai++;
            i++;
        } else if (fmt[i] == 'x' || fmt[i] == 'X') {
            const char *a = (ai < argc) ? argv[ai] : "0";
            out_hex((u64)strtol_dec(a));
            if (ai < argc) ai++;
            i++;
        } else if (fmt[i] == 'o') {
            const char *a = (ai < argc) ? argv[ai] : "0";
            out_oct((u64)strtol_dec(a));
            if (ai < argc) ai++;
            i++;
        } else {
            out_ch('%');
            out_ch(fmt[i++]);
        }
    }
    (void)fmt_stop;
    return 0;
}
