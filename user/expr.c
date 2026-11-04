/* fakeos userland - expr: integer arithmetic / string (thin POSIX subset).
 * Forms:
 *   expr INTEGER1 +|-|*|/|% INTEGER2
 *   expr INTEGER1 =|!=|<|>|<=|>= INTEGER2
 *   expr length STRING
 *   expr index STRING CHARS   (1-based; 0 if none)
 *   expr substr STRING POS LEN  (1-based POS; empty if POS/LEN invalid)
 *   expr ARG   (print ARG; exit 0 if non-empty/non-zero)
 * Exit: 0 if result non-zero/non-empty, 1 if zero/empty, 2 on error.
 * Quote '*' in the shell: expr 3 '*' 4
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

static void usage(void) {
    u_write(2, "usage: expr INTEGER OP INTEGER | length|index|substr ...\n", 58);
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int parse_long(const char *s, long *out) {
    long v = 0;
    int i = 0;
    int neg = 0;
    if (s == 0 || s[0] == 0) return 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (s[i] == 0) return 0;
    } else if (s[0] == '+') {
        i = 1;
        if (s[i] == 0) return 0;
    }
    if (!is_digit(s[i])) return 0;
    while (s[i]) {
        if (!is_digit(s[i])) return 0;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    *out = neg ? -v : v;
    return 1;
}

static int looks_integer(const char *s) {
    long tmp;
    return parse_long(s, &tmp);
}

static int finish_num(long v) {
    printf("%d\n", (int)v);
    return v == 0 ? 1 : 0;
}

static int finish_str(const char *s) {
    unsigned long n = strlen(s);
    if (n > 0) u_write(1, s, n);
    u_write(1, "\n", 1);
    if (n == 0) return 1;
    if (looks_integer(s)) {
        long v;
        parse_long(s, &v);
        return v == 0 ? 1 : 0;
    }
    return 0;
}

static int do_arith(long a, const char *op, long b, long *out) {
    if (strcmp(op, "+") == 0) {
        *out = a + b;
        return 0;
    }
    if (strcmp(op, "-") == 0) {
        *out = a - b;
        return 0;
    }
    if (strcmp(op, "*") == 0) {
        *out = a * b;
        return 0;
    }
    if (strcmp(op, "/") == 0) {
        if (b == 0) return -1;
        *out = a / b;
        return 0;
    }
    if (strcmp(op, "%") == 0) {
        if (b == 0) return -1;
        *out = a % b;
        return 0;
    }
    return 1;
}

static int do_cmp(long a, const char *op, long b, long *out) {
    int ok = 0;
    if (strcmp(op, "=") == 0) ok = (a == b);
    else if (strcmp(op, "!=") == 0) ok = (a != b);
    else if (strcmp(op, "<") == 0) ok = (a < b);
    else if (strcmp(op, ">") == 0) ok = (a > b);
    else if (strcmp(op, "<=") == 0) ok = (a <= b);
    else if (strcmp(op, ">=") == 0) ok = (a >= b);
    else return 1;
    *out = ok ? 1 : 0;
    return 0;
}

int main(int argc, char **argv) {
    long a;
    long b;
    long r;
    int rc;

    if (argc < 2) {
        usage();
        return 2;
    }

    /* expr length STRING */
    if (argc == 3 && strcmp(argv[1], "length") == 0) {
        return finish_num((long)strlen(argv[2]));
    }

    /* expr index STRING CHARS */
    if (argc == 4 && strcmp(argv[1], "index") == 0) {
        const char *s = argv[2];
        const char *chars = argv[3];
        unsigned long i;
        unsigned long j;
        unsigned long n = strlen(s);
        unsigned long m = strlen(chars);
        for (i = 0; i < n; i++) {
            for (j = 0; j < m; j++) {
                if (s[i] == chars[j]) {
                    return finish_num((long)(i + 1));
                }
            }
        }
        return finish_num(0);
    }

    /* expr substr STRING POS LENGTH */
    if (argc == 5 && strcmp(argv[1], "substr") == 0) {
        long pos = 0;
        long len = 0;
        const char *s = argv[2];
        unsigned long n;
        unsigned long start;
        unsigned long take;
        char buf[256];
        unsigned long k;
        if (!parse_long(argv[3], &pos) || !parse_long(argv[4], &len)) {
            usage();
            return 2;
        }
        n = strlen(s);
        if (pos < 1 || len < 1 || (unsigned long)pos > n) {
            return finish_str("");
        }
        start = (unsigned long)(pos - 1);
        take = (unsigned long)len;
        if (start + take > n) take = n - start;
        if (take >= sizeof(buf)) take = sizeof(buf) - 1;
        for (k = 0; k < take; k++) buf[k] = s[start + k];
        buf[take] = 0;
        return finish_str(buf);
    }

    /* expr ARG */
    if (argc == 2) {
        return finish_str(argv[1]);
    }

    /* expr INTEGER OP INTEGER */
    if (argc == 4) {
        if (!parse_long(argv[1], &a) || !parse_long(argv[3], &b)) {
            /* String compare for = / != only when either side is non-integer. */
            if (strcmp(argv[2], "=") == 0) {
                r = (strcmp(argv[1], argv[3]) == 0) ? 1 : 0;
                return finish_num(r);
            }
            if (strcmp(argv[2], "!=") == 0) {
                r = (strcmp(argv[1], argv[3]) != 0) ? 1 : 0;
                return finish_num(r);
            }
            usage();
            return 2;
        }
        rc = do_arith(a, argv[2], b, &r);
        if (rc == 0) return finish_num(r);
        if (rc < 0) {
            u_write(2, "expr: division by zero\n", 23);
            return 2;
        }
        rc = do_cmp(a, argv[2], b, &r);
        if (rc == 0) return finish_num(r);
        usage();
        return 2;
    }

    usage();
    return 2;
}
