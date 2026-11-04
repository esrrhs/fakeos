/* fakeos userland - expr: integer arithmetic / string (thin POSIX subset).
 * Forms:
 *   expr INTEGER1 +|-|*|/|% INTEGER2
 *   expr INTEGER1 =|!=|<|>|<=|>= INTEGER2
 *   expr STRING : RE          (anchored thin BRE; length or one \( \) capture)
 *   expr length STRING
 *   expr index STRING CHARS   (1-based; 0 if none)
 *   expr substr STRING POS LEN  (1-based POS; empty if POS/LEN invalid)
 *   expr ARG   (print ARG; exit 0 if non-empty/non-zero)
 * : RE is a thin BRE (implicit ^): literals, '.', '*', '\' escapes, optional
 * trailing '$', one \( \) group (optional '*' after group; no nesting). With a
 * group, result is the captured substring (empty if no match); else match
 * length. Exit: 0 if result non-zero/non-empty, 1 if zero/empty, 2 on error.
 * Quote '*' in the shell: expr 3 '*' 4
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

static void usage(void) {
    u_write(2, "usage: expr INTEGER OP INTEGER | STRING : RE | length|index|substr ...\n", 71);
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

static const char *cap_s;
static const char *cap_e;
static int want_cap; /* pattern contains \( */

static int atom_eq(char atom, int esc, char c) {
    if (!esc && atom == '.') return 1;
    return atom == c;
}

/* Parse one atom from *re; advance *re. Returns 0 on failure.
 * Does not consume \( or \) (handled in match_here). */
static int take_atom(const char **re, char *atom, int *esc) {
    const char *p = *re;
    if (p[0] == 0) return 0;
    if (p[0] == '*') return 0;
    if (p[0] == '\\') {
        if (p[1] == 0) return 0;
        if (p[1] == '(' || p[1] == ')') return 0;
        *atom = p[1];
        *esc = 1;
        *re = p + 2;
        return 1;
    }
    *atom = p[0];
    *esc = 0;
    *re = p + 1;
    return 1;
}

static int pattern_has_group(const char *re) {
    int i;
    for (i = 0; re[i]; i++) {
        if (re[i] == '\\' && re[i + 1] == '(') return 1;
        if (re[i] == '\\' && re[i + 1] != 0) i++;
    }
    return 0;
}

static const char *match_here(const char *re, const char *text);

static const char *match_star(char atom, int esc, const char *re, const char *text) {
    const char *t = text;
    const char *r;
    while (*t && atom_eq(atom, esc, *t)) t++;
    while (1) {
        r = match_here(re, t);
        if (r) return r;
        if (t == text) return 0;
        t--;
    }
}

enum { GBUF = 128, GPOS = 64 };

/* Zero-or-more of body, then rest. Capture is last body match (empty if none). */
static const char *match_group_star(const char *body, const char *rest,
                                    const char *text) {
    const char *pos[GPOS];
    const char *cs[GPOS];
    const char *ce[GPOS];
    int npos = 0;
    const char *t = text;
    const char *r;

    pos[0] = text;
    cs[0] = 0;
    ce[0] = 0;
    npos = 1;
    while (npos < GPOS) {
        const char *saved_s = cap_s;
        const char *saved_e = cap_e;
        r = match_here(body, t);
        cap_s = saved_s;
        cap_e = saved_e;
        if (!r || r == t) break;
        /* Re-run to set capture for this iteration (body match). */
        {
            const char *r2 = match_here(body, t);
            if (!r2) break;
            pos[npos] = r2;
            cs[npos] = t;
            ce[npos] = r2;
            t = r2;
            npos++;
        }
    }
    while (npos > 0) {
        npos--;
        if (cs[npos]) {
            cap_s = cs[npos];
            cap_e = ce[npos];
        } else {
            cap_s = text;
            cap_e = text; /* empty capture */
        }
        r = match_here(rest, pos[npos]);
        if (r) return r;
    }
    return 0;
}

/* Thin BRE from re against text; returns end pointer or 0. */
static const char *match_here(const char *re, const char *text) {
    char atom;
    int esc;
    if (re[0] == '^') re++; /* expr is already anchored; allow leading ^ */
    if (re[0] == 0) return text;
    if (re[0] == '$' && re[1] == 0) return (*text == 0) ? text : 0;

    /* One \( body \) group; optional '*' after. No nesting. */
    if (re[0] == '\\' && re[1] == '(') {
        const char *body = re + 2;
        const char *p = body;
        char gbuf[GBUF];
        unsigned long glen = 0;
        const char *rest;
        const char *gend;
        while (*p) {
            if (p[0] == '\\' && p[1] == '(') return 0; /* no nest */
            if (p[0] == '\\' && p[1] == ')') break;
            if (p[0] == '\\' && p[1] != 0) {
                if (glen + 2 >= GBUF) return 0;
                gbuf[glen++] = p[0];
                gbuf[glen++] = p[1];
                p += 2;
            } else {
                if (glen + 1 >= GBUF) return 0;
                gbuf[glen++] = *p;
                p++;
            }
        }
        if (*p == 0) return 0; /* unmatched \( */
        gbuf[glen] = 0;
        rest = p + 2;
        if (rest[0] == '*') {
            return match_group_star(gbuf, rest + 1, text);
        }
        gend = match_here(gbuf, text);
        if (!gend) return 0;
        cap_s = text;
        cap_e = gend;
        return match_here(rest, gend);
    }
    if (re[0] == '\\' && re[1] == ')') return 0; /* unmatched \) */

    if (!take_atom(&re, &atom, &esc)) return 0;
    if (re[0] == '*') {
        return match_star(atom, esc, re + 1, text);
    }
    if (*text == 0) return 0;
    if (!atom_eq(atom, esc, *text)) return 0;
    return match_here(re, text + 1);
}

/*
 * Anchored : match. If pattern has \( \), on success copies capture into
 * capbuf (may be empty) and returns 1; on failure returns -1.
 * Without a group, returns matched length (>=0) or -1; capbuf unused.
 * Return codes via *kind: 0 = length in *len_out, 1 = string in capbuf.
 */
static int match_colon(const char *text, const char *re, long *len_out,
                       char *capbuf, unsigned long capmax, int *kind) {
    const char *end;
    unsigned long n;
    unsigned long i;
    if (text == 0 || re == 0) return -1;
    want_cap = pattern_has_group(re);
    cap_s = 0;
    cap_e = 0;
    end = match_here(re, text);
    if (end == 0) return -1;
    if (want_cap) {
        if (cap_s == 0 || cap_e == 0 || cap_e < cap_s) {
            capbuf[0] = 0;
        } else {
            n = (unsigned long)(cap_e - cap_s);
            if (n >= capmax) n = capmax - 1;
            for (i = 0; i < n; i++) capbuf[i] = cap_s[i];
            capbuf[n] = 0;
        }
        *kind = 1;
        return 0;
    }
    *len_out = (long)(end - text);
    *kind = 0;
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

    /* expr STRING : RE  (thin BRE; length or one \( \) capture) */
    if (argc == 4 && strcmp(argv[2], ":") == 0) {
        char capbuf[256];
        int kind = 0;
        int m;
        m = match_colon(argv[1], argv[3], &r, capbuf, sizeof(capbuf), &kind);
        if (m < 0) {
            if (pattern_has_group(argv[3])) return finish_str("");
            return finish_num(0);
        }
        if (kind == 1) return finish_str(capbuf);
        return finish_num(r);
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
