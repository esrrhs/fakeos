/* fakeos userland - diff: line-oriented file compare (thin POSIX).
 * Forms: diff [-uqbiw] FILE1 FILE2
 * Default: classic a/c/d hunks with "< "/"> " lines.
 * -u: unified hunks (3 lines context). -q: brief "Files X and Y differ".
 * -b: ignore changes in amount of whitespace. -w: ignore all whitespace.
 * -i: ignore case when comparing. Combines with -b/-w.
 * Exit 0 identical, 1 differ, 2 error. "-" is stdin (at most one).
 * cmp already covers byte compare; this is the line-oriented companion.
 * Compact argv for EXEC_MAX_ARGC=8. Loads lines into RAM (MAX_LINES).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern void *malloc(unsigned long size);
extern void free(void *ptr);
extern void printf(const char *fmt, ...);

enum {
    LINE_MAX = 512,
    MAX_LINES = 1024,
    CTX = 3
};

static char *a_lines[MAX_LINES];
static char *b_lines[MAX_LINES];
static int na;
static int nb;
static int opt_u;
static int opt_q;
static int opt_b; /* ignore amount of whitespace */
static int opt_w; /* ignore all whitespace */
static int opt_i; /* ignore case */
static const char *name1;
static const char *name2;

static void usage(void) {
    u_write(2, "usage: diff [-uqbw] FILE1 FILE2
", 32);
}

static void free_lines(char **lines, int n) {
    int i;
    for (i = 0; i < n; i++) {
        free(lines[i]);
        lines[i] = 0;
    }
}

static int add_line(char **lines, int *n, const char *s, int len) {
    char *p;
    int i;
    if (*n >= MAX_LINES) {
        u_write(2, "diff: too many lines\n", 21);
        return 1;
    }
    if (len >= LINE_MAX) len = LINE_MAX - 1;
    p = (char *)malloc((unsigned long)len + 1);
    if (p == 0) {
        u_write(2, "diff: out of memory\n", 20);
        return 1;
    }
    for (i = 0; i < len; i++) p[i] = s[i];
    p[len] = 0;
    lines[*n] = p;
    (*n)++;
    return 0;
}

static int load_fd(long fd, char **lines, int *n) {
    char buf[512];
    char line[LINE_MAX];
    long nr;
    int li = 0;
    int i;

    *n = 0;
    while ((nr = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)nr; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (add_line(lines, n, line, li) != 0) return 1;
                li = 0;
            } else {
                if (li < LINE_MAX - 1) line[li++] = c;
            }
        }
    }
    if (nr < 0) return 1;
    if (li > 0) {
        if (add_line(lines, n, line, li) != 0) return 1;
    }
    return 0;
}

static int open_in(const char *path, long *fd_out) {
    long fd;
    if (strcmp(path, "-") == 0) {
        *fd_out = 0;
        return 0;
    }
    fd = u_open(path, 0, 0);
    if (fd < 0) {
        printf("diff: %s: no such file\n", path);
        return 1;
    }
    *fd_out = fd;
    return 0;
}

static void emit_dec(unsigned long v) {
    char tmp[24];
    int i = 24;
    if (v == 0) {
        u_write(1, "0", 1);
        return;
    }
    while (v > 0 && i > 0) {
        i--;
        tmp[i] = (char)('0' + (v % 10));
        v /= 10;
    }
    u_write(1, tmp + i, (unsigned long)(24 - i));
}

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

static char fold_c(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int chars_eq(char ca, char cb) {
    if (opt_i) return fold_c(ca) == fold_c(cb);
    return ca == cb;
}

/* Line equality under -b/-w/-i (or plain strcmp). */
static int lines_eq(const char *a, const char *b) {
    int i = 0;
    int j = 0;
    if (!opt_b && !opt_w) {
        if (!opt_i) return strcmp(a, b) == 0;
        while (a[i] && b[i]) {
            if (!chars_eq(a[i], b[i])) return 0;
            i++;
        }
        return a[i] == 0 && b[i] == 0;
    }
    if (opt_w) {
        while (1) {
            while (a[i] && is_ws(a[i])) i++;
            while (b[j] && is_ws(b[j])) j++;
            if (a[i] == 0 || b[j] == 0) return a[i] == 0 && b[j] == 0;
            if (!chars_eq(a[i], b[j])) return 0;
            i++;
            j++;
        }
    }
    /* -b: whitespace runs match each other; trailing ws-only OK. */
    while (1) {
        if (a[i] && is_ws(a[i]) && b[j] && is_ws(b[j])) {
            while (a[i] && is_ws(a[i])) i++;
            while (b[j] && is_ws(b[j])) j++;
            continue;
        }
        if (a[i] == 0) {
            while (b[j] && is_ws(b[j])) j++;
            return b[j] == 0;
        }
        if (b[j] == 0) {
            while (a[i] && is_ws(a[i])) i++;
            return a[i] == 0;
        }
        if (is_ws(a[i]) || is_ws(b[j])) return 0;
        if (!chars_eq(a[i], b[j])) return 0;
        i++;
        j++;
    }
}

/* LCS length matrix: (na+1)*(nb+1) unsigned shorts, row-major. */
static unsigned short *lcs_build(void) {
    unsigned long cells = (unsigned long)(na + 1) * (unsigned long)(nb + 1);
    unsigned short *dp;
    int i;
    int j;

    dp = (unsigned short *)malloc(cells * sizeof(unsigned short));
    if (dp == 0) return 0;
    for (i = 0; i <= na; i++) {
        for (j = 0; j <= nb; j++) {
            unsigned long idx = (unsigned long)i * (unsigned long)(nb + 1)
                              + (unsigned long)j;
            if (i == 0 || j == 0) {
                dp[idx] = 0;
            } else if (lines_eq(a_lines[i - 1], b_lines[j - 1])) {
                unsigned long prev = (unsigned long)(i - 1)
                                   * (unsigned long)(nb + 1)
                                   + (unsigned long)(j - 1);
                dp[idx] = (unsigned short)(dp[prev] + 1);
            } else {
                unsigned long up = (unsigned long)(i - 1)
                                 * (unsigned long)(nb + 1)
                                 + (unsigned long)j;
                unsigned long left = (unsigned long)i
                                   * (unsigned long)(nb + 1)
                                   + (unsigned long)(j - 1);
                dp[idx] = dp[up] >= dp[left] ? dp[up] : dp[left];
            }
        }
    }
    return dp;
}

/* Emit classic change from a[a0..a1) vs b[b0..b1) (0-based half-open). */
static void emit_classic_hunk(int a0, int a1, int b0, int b1) {
    int i;
    /* ed-style range header */
    if (a0 + 1 == a1 || (a0 == a1 && a0 > 0)) {
        /* single line or empty — for empty deletion/addition use a0 */
        if (a0 == a1) {
            emit_dec((unsigned long)a0);
        } else {
            emit_dec((unsigned long)(a0 + 1));
        }
    } else if (a0 == a1) {
        emit_dec((unsigned long)a0);
    } else {
        emit_dec((unsigned long)(a0 + 1));
        u_write(1, ",", 1);
        emit_dec((unsigned long)a1);
    }
    if (a0 == a1) u_write(1, "a", 1);
    else if (b0 == b1) u_write(1, "d", 1);
    else u_write(1, "c", 1);
    if (b0 + 1 == b1 || (b0 == b1 && b0 > 0)) {
        if (b0 == b1) emit_dec((unsigned long)b0);
        else emit_dec((unsigned long)(b0 + 1));
    } else if (b0 == b1) {
        emit_dec((unsigned long)b0);
    } else {
        emit_dec((unsigned long)(b0 + 1));
        u_write(1, ",", 1);
        emit_dec((unsigned long)b1);
    }
    u_write(1, "\n", 1);

    for (i = a0; i < a1; i++) {
        u_write(1, "< ", 2);
        u_write(1, a_lines[i], strlen(a_lines[i]));
        u_write(1, "\n", 1);
    }
    if (a0 != a1 && b0 != b1) u_write(1, "---\n", 4);
    for (i = b0; i < b1; i++) {
        u_write(1, "> ", 2);
        u_write(1, b_lines[i], strlen(b_lines[i]));
        u_write(1, "\n", 1);
    }
}

/* Backtrack LCS and emit classic hunks. */
static void emit_classic(unsigned short *dp) {
    /* Collect ops via reverse walk into stacks of equal/delete/insert spans. */
    int i = na;
    int j = nb;
    /* Store reverse-order change records: kind 0=eq span end, 1=del, 2=ins
     * Then reverse to forward and coalesce. Simpler: walk and buffer indices
     * of matching pairs, then emit gaps. */
    int *ma;
    int *mb;
    int nm = 0;
    int k;

    ma = (int *)malloc((unsigned long)(na + nb + 1) * sizeof(int));
    mb = (int *)malloc((unsigned long)(na + nb + 1) * sizeof(int));
    if (ma == 0 || mb == 0) {
        free(ma);
        free(mb);
        u_write(2, "diff: out of memory\n", 20);
        return;
    }

    while (i > 0 && j > 0) {
        unsigned long idx = (unsigned long)i * (unsigned long)(nb + 1)
                          + (unsigned long)j;
        unsigned long up = (unsigned long)(i - 1) * (unsigned long)(nb + 1)
                         + (unsigned long)j;
        unsigned long left = (unsigned long)i * (unsigned long)(nb + 1)
                           + (unsigned long)(j - 1);
        if (lines_eq(a_lines[i - 1], b_lines[j - 1])) {
            ma[nm] = i - 1;
            mb[nm] = j - 1;
            nm++;
            i--;
            j--;
        } else if (dp[up] >= dp[left]) {
            i--;
        } else {
            j--;
        }
    }
    /* ma/mb are reverse chronological matches; reverse to ascending. */
    for (k = 0; k < nm / 2; k++) {
        int t = ma[k];
        ma[k] = ma[nm - 1 - k];
        ma[nm - 1 - k] = t;
        t = mb[k];
        mb[k] = mb[nm - 1 - k];
        mb[nm - 1 - k] = t;
    }

    {
        int ai = 0;
        int bi = 0;
        for (k = 0; k <= nm; k++) {
            int a_end = (k < nm) ? ma[k] : na;
            int b_end = (k < nm) ? mb[k] : nb;
            if (ai < a_end || bi < b_end) {
                emit_classic_hunk(ai, a_end, bi, b_end);
            }
            if (k < nm) {
                ai = ma[k] + 1;
                bi = mb[k] + 1;
            }
        }
    }
    free(ma);
    free(mb);
}

/* Unified: mark each line -1 delete, +1 insert, 0 equal via LCS matches. */
static void emit_unified(unsigned short *dp) {
    signed char *tag_a;
    signed char *tag_b;
    int i = na;
    int j = nb;
    int p;
    int q;

    tag_a = (signed char *)malloc((unsigned long)na);
    tag_b = (signed char *)malloc((unsigned long)nb);
    if ((na > 0 && tag_a == 0) || (nb > 0 && tag_b == 0)) {
        free(tag_a);
        free(tag_b);
        u_write(2, "diff: out of memory\n", 20);
        return;
    }
    for (p = 0; p < na; p++) tag_a[p] = -1; /* default delete */
    for (q = 0; q < nb; q++) tag_b[q] = 1;  /* default insert */

    while (i > 0 && j > 0) {
        unsigned long up = (unsigned long)(i - 1) * (unsigned long)(nb + 1)
                         + (unsigned long)j;
        unsigned long left = (unsigned long)i * (unsigned long)(nb + 1)
                           + (unsigned long)(j - 1);
        if (lines_eq(a_lines[i - 1], b_lines[j - 1])) {
            tag_a[i - 1] = 0;
            tag_b[j - 1] = 0;
            i--;
            j--;
        } else if (dp[up] >= dp[left]) {
            i--;
        } else {
            j--;
        }
    }

    printf("--- %s\n", name1);
    printf("+++ %s\n", name2);

    /* Walk a and b in lockstep using tags to emit hunks with context. */
    {
        int ai = 0;
        int bi = 0;
        while (ai < na || bi < nb) {
            int hunk_a0;
            int hunk_a1;
            int hunk_b0;
            int hunk_b1;
            int ca;
            int cb;
            int changed;

            /* Skip equal run. */
            while (ai < na && bi < nb && tag_a[ai] == 0 && tag_b[bi] == 0) {
                ai++;
                bi++;
            }
            if (ai >= na && bi >= nb) break;

            /* Change starts at ai/bi. Back up CTX equal lines for context. */
            hunk_a0 = ai - CTX;
            hunk_b0 = bi - CTX;
            if (hunk_a0 < 0) hunk_a0 = 0;
            if (hunk_b0 < 0) hunk_b0 = 0;
            /* But don't include equals before the previous hunk end —
             * simplify: clamp to start of this equal-preceded change. */
            {
                int ba = ai;
                int bb = bi;
                int c;
                for (c = 0; c < CTX; c++) {
                    if (ba > 0 && bb > 0 && tag_a[ba - 1] == 0
                        && tag_b[bb - 1] == 0) {
                        ba--;
                        bb--;
                    } else break;
                }
                hunk_a0 = ba;
                hunk_b0 = bb;
            }

            /* Advance through change (+ trailing context). */
            ca = ai;
            cb = bi;
            changed = 1;
            while (changed) {
                changed = 0;
                while (ca < na && tag_a[ca] == -1) {
                    ca++;
                    changed = 1;
                }
                while (cb < nb && tag_b[cb] == 1) {
                    cb++;
                    changed = 1;
                }
                /* absorb following equals as possible middle of multi-change
                 * within context window — greedy: take equals then peek */
                {
                    int ea = ca;
                    int eb = cb;
                    int eq = 0;
                    while (ea < na && eb < nb && tag_a[ea] == 0
                           && tag_b[eb] == 0) {
                        ea++;
                        eb++;
                        eq++;
                    }
                    if (eq > 0 && eq <= 2 * CTX
                        && ((ea < na && tag_a[ea] == -1)
                            || (eb < nb && tag_b[eb] == 1))) {
                        ca = ea;
                        cb = eb;
                        changed = 1;
                    }
                }
            }
            /* Trailing context. */
            {
                int t;
                for (t = 0; t < CTX; t++) {
                    if (ca < na && cb < nb && tag_a[ca] == 0
                        && tag_b[cb] == 0) {
                        ca++;
                        cb++;
                    } else break;
                }
            }
            hunk_a1 = ca;
            hunk_b1 = cb;

            /* Header: @@ -start,count +start,count @@ (1-based start) */
            u_write(1, "@@ -", 4);
            if (hunk_a1 == hunk_a0) {
                emit_dec((unsigned long)hunk_a0);
                u_write(1, ",0", 2);
            } else {
                emit_dec((unsigned long)(hunk_a0 + 1));
                if (hunk_a1 - hunk_a0 != 1) {
                    u_write(1, ",", 1);
                    emit_dec((unsigned long)(hunk_a1 - hunk_a0));
                }
            }
            u_write(1, " +", 2);
            if (hunk_b1 == hunk_b0) {
                emit_dec((unsigned long)hunk_b0);
                u_write(1, ",0", 2);
            } else {
                emit_dec((unsigned long)(hunk_b0 + 1));
                if (hunk_b1 - hunk_b0 != 1) {
                    u_write(1, ",", 1);
                    emit_dec((unsigned long)(hunk_b1 - hunk_b0));
                }
            }
            u_write(1, " @@\n", 4);

            /* Emit lines: walk a and b from hunk starts. */
            {
                int xa = hunk_a0;
                int xb = hunk_b0;
                while (xa < hunk_a1 || xb < hunk_b1) {
                    if (xa < hunk_a1 && tag_a[xa] == -1) {
                        u_write(1, "-", 1);
                        u_write(1, a_lines[xa], strlen(a_lines[xa]));
                        u_write(1, "\n", 1);
                        xa++;
                    } else if (xb < hunk_b1 && tag_b[xb] == 1) {
                        u_write(1, "+", 1);
                        u_write(1, b_lines[xb], strlen(b_lines[xb]));
                        u_write(1, "\n", 1);
                        xb++;
                    } else if (xa < hunk_a1 && xb < hunk_b1
                               && tag_a[xa] == 0 && tag_b[xb] == 0) {
                        u_write(1, " ", 1);
                        u_write(1, a_lines[xa], strlen(a_lines[xa]));
                        u_write(1, "\n", 1);
                        xa++;
                        xb++;
                    } else if (xa < hunk_a1) {
                        u_write(1, "-", 1);
                        u_write(1, a_lines[xa], strlen(a_lines[xa]));
                        u_write(1, "\n", 1);
                        xa++;
                    } else if (xb < hunk_b1) {
                        u_write(1, "+", 1);
                        u_write(1, b_lines[xb], strlen(b_lines[xb]));
                        u_write(1, "\n", 1);
                        xb++;
                    } else {
                        break;
                    }
                }
            }

            ai = hunk_a1;
            bi = hunk_b1;
        }
    }
    free(tag_a);
    free(tag_b);
}

int main(int argc, char **argv) {
    int ai = 1;
    long fd1;
    long fd2;
    int stdin_used = 0;
    unsigned short *dp;
    int identical;
    int i;

    opt_u = 0;
    opt_q = 0;
    opt_b = 0;
    opt_w = 0;
    opt_i = 0;
    na = 0;
    nb = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o;
        int j;
        if (strcmp(argv[ai], "--") == 0) {
            ai++;
            break;
        }
        if (strcmp(argv[ai], "-") == 0) break; /* stdin file operand */
        o = argv[ai] + 1;
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'u') opt_u = 1;
            else if (o[j] == 'q') opt_q = 1;
            else if (o[j] == 'b') opt_b = 1;
            else if (o[j] == 'w') opt_w = 1;
            else if (o[j] == 'i') opt_i = 1;
            else {
                usage();
                return 2;
            }
        }
        ai++;
    }

    if (ai + 2 != argc) {
        usage();
        return 2;
    }

    name1 = argv[ai];
    name2 = argv[ai + 1];
    if (strcmp(name1, "-") == 0) stdin_used = 1;
    if (strcmp(name2, "-") == 0) {
        if (stdin_used) {
            u_write(2, "diff: only one stdin\n", 21);
            return 2;
        }
    }

    if (open_in(name1, &fd1) != 0) return 2;
    if (load_fd(fd1, a_lines, &na) != 0) {
        if (fd1 != 0) u_close(fd1);
        free_lines(a_lines, na);
        return 2;
    }
    if (fd1 != 0) u_close(fd1);

    if (open_in(name2, &fd2) != 0) {
        free_lines(a_lines, na);
        return 2;
    }
    if (load_fd(fd2, b_lines, &nb) != 0) {
        if (fd2 != 0) u_close(fd2);
        free_lines(a_lines, na);
        free_lines(b_lines, nb);
        return 2;
    }
    if (fd2 != 0) u_close(fd2);

    identical = 1;
    if (na != nb) {
        identical = 0;
    } else {
        for (i = 0; i < na; i++) {
            if (!lines_eq(a_lines[i], b_lines[i])) {
                identical = 0;
                break;
            }
        }
    }
    if (identical) {
        free_lines(a_lines, na);
        free_lines(b_lines, nb);
        return 0;
    }

    if (opt_q) {
        printf("Files %s and %s differ\n", name1, name2);
        free_lines(a_lines, na);
        free_lines(b_lines, nb);
        return 1;
    }

    dp = lcs_build();
    if (dp == 0) {
        u_write(2, "diff: out of memory\n", 20);
        free_lines(a_lines, na);
        free_lines(b_lines, nb);
        return 2;
    }

    if (opt_u) emit_unified(dp);
    else emit_classic(dp);

    free(dp);
    free_lines(a_lines, na);
    free_lines(b_lines, nb);
    return 1;
}
