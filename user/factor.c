/* fakeos userland - factor: print prime factors (thin GNU subset).
 * Forms: factor [NUMBER...]
 * With no operands, read decimal integers from stdin (whitespace-separated).
 * Output: "N: f1 f2 ..." with ascending prime factors (with multiplicity).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern void printf(const char *fmt, ...);

typedef unsigned long u64;

static int parse_u64(const char *s, u64 *out) {
    u64 v = 0;
    int any = 0;
    if (s == 0 || s[0] == 0) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10UL + (u64)(*s - '0');
        s++;
        any = 1;
    }
    if (!any) return 0;
    *out = v;
    return 1;
}

static void factor_one(u64 n) {
    u64 x = n;
    u64 d;

    printf("%lu:", n);
    if (n <= 1UL) {
        u_write(1, "\n", 1);
        return;
    }
    while ((x & 1UL) == 0UL) {
        u_write(1, " 2", 2);
        x >>= 1;
    }
    d = 3UL;
    while (d <= x / d) {
        while (x % d == 0UL) {
            printf(" %lu", d);
            x /= d;
        }
        d += 2UL;
    }
    if (x > 1UL) {
        printf(" %lu", x);
    }
    u_write(1, "\n", 1);
}

static int factor_stdin(void) {
    char buf[256];
    char tok[64];
    long n;
    long i;
    int ti = 0;
    int any = 0;
    int bad = 0;

    while ((n = u_read(0, buf, 256)) > 0) {
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (ti > 0) {
                    u64 v;
                    tok[ti] = 0;
                    if (!parse_u64(tok, &v)) {
                        u_write(2, "factor: invalid number\n", 23);
                        bad = 1;
                    } else {
                        factor_one(v);
                        any = 1;
                    }
                    ti = 0;
                }
            } else {
                if (ti + 1 >= 64) {
                    u_write(2, "factor: invalid number\n", 23);
                    bad = 1;
                    ti = 0;
                    while (i + 1 < n && buf[i + 1] != ' ' &&
                           buf[i + 1] != '\t' && buf[i + 1] != '\n' &&
                           buf[i + 1] != '\r') {
                        i++;
                    }
                } else {
                    tok[ti++] = c;
                }
            }
        }
    }
    if (ti > 0) {
        u64 v;
        tok[ti] = 0;
        if (!parse_u64(tok, &v)) {
            u_write(2, "factor: invalid number\n", 23);
            bad = 1;
        } else {
            factor_one(v);
            any = 1;
        }
    }
    (void)any;
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    int i;
    int bad = 0;

    if (argc <= 1) {
        return factor_stdin();
    }
    for (i = 1; i < argc; i++) {
        u64 v;
        if (!parse_u64(argv[i], &v)) {
            u_write(2, "factor: invalid number\n", 23);
            bad = 1;
            continue;
        }
        factor_one(v);
    }
    return bad ? 1 : 0;
}
