/* fakeos userland - printf: format ARGS with escapes (thin POSIX subset). */
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

/* Expand backslash escapes from fmt into out (NUL-terminated). Returns length. */
static int expand_escapes(const char *fmt, char *out, int cap) {
    int n = 0;
    while (*fmt && n + 1 < cap) {
        if (*fmt != '\\') {
            out[n++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == 0) {
            out[n++] = '\\';
            break;
        }
        if (*fmt == 'n') { out[n++] = '\n'; fmt++; }
        else if (*fmt == 't') { out[n++] = '\t'; fmt++; }
        else if (*fmt == 'r') { out[n++] = '\r'; fmt++; }
        else if (*fmt == 'a') { out[n++] = '\a'; fmt++; }
        else if (*fmt == 'b') { out[n++] = '\b'; fmt++; }
        else if (*fmt == 'f') { out[n++] = '\f'; fmt++; }
        else if (*fmt == 'v') { out[n++] = '\v'; fmt++; }
        else if (*fmt == '\\') { out[n++] = '\\'; fmt++; }
        else if (*fmt == '%') { out[n++] = '%'; fmt++; }
        else if (*fmt >= '0' && *fmt <= '7') {
            int cons = 0;
            int v = parse_oct3(fmt, &cons);
            out[n++] = (char)v;
            fmt += cons;
        } else {
            out[n++] = *fmt++;
        }
    }
    out[n] = 0;
    return n;
}

int main(int argc, char **argv) {
    char expanded[1024];
    const char *fmt;
    int ai;
    int i;

    if (argc < 2) {
        return 0;
    }
    expand_escapes(argv[1], expanded, (int)sizeof(expanded));
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
        } else {
            out_ch('%');
            out_ch(fmt[i++]);
        }
    }
    return 0;
}
