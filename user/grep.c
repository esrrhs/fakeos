/* fakeos userland - grep: fixed-string line filter; thin -E ERE subset.
 * Options: -i -v -n -F -c -E. Glued short opts. -F forces fixed (wins over -E).
 * -c suppresses line output; multi-FILE prints name:count per file.
 * ERE subset: | . * + ? () ^ $ and \ escapes. No [] classes, no {} bounds.
 */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, unsigned long n);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);

enum { LINE_MAX = 512 };

static const char *USAGE = "usage: grep [-ivncFE] PATTERN [FILE...]\n";

static char lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int contains(const char *hay, const char *needle, int ign) {
    unsigned long nlen = strlen(needle);
    unsigned long i;
    unsigned long hlen;
    if (nlen == 0) return 1;
    hlen = strlen(hay);
    if (hlen < nlen) return 0;
    for (i = 0; i + nlen <= hlen; i++) {
        unsigned long j;
        int ok = 1;
        for (j = 0; j < nlen; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (ign) {
                a = lower(a);
                b = lower(b);
            }
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/* Find top-level '|' in pat[pi..pe), respecting (). Returns index or -1. */
static int find_alt(const char *pat, int pi, int pe) {
    int depth = 0;
    int i;
    for (i = pi; i < pe; i++) {
        char c = pat[i];
        if (c == '\\' && i + 1 < pe) { i++; continue; }
        if (c == '(') depth++;
        else if (c == ')') { if (depth > 0) depth--; }
        else if (c == '|' && depth == 0) return i;
    }
    return -1;
}

/* Find matching ')' for '(' at pi; scan pat[pi+1..pe). Returns index or -1. */
static int find_rparen(const char *pat, int pi, int pe) {
    int depth = 1;
    int i;
    for (i = pi + 1; i < pe; i++) {
        char c = pat[i];
        if (c == '\\' && i + 1 < pe) { i++; continue; }
        if (c == '(') depth++;
        else if (c == ')') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

/* Parse one atom at pi; set *atom_e exclusive end of atom text in pat,
 * *next to position after atom (before any quantifier). Returns 0 ok / -1 bad. */
static int parse_atom(const char *pat, int pi, int pe, int *atom_s, int *atom_e,
                      int *next) {
    if (pi >= pe) return -1;
    *atom_s = pi;
    if (pat[pi] == '\\') {
        if (pi + 1 >= pe) {
            *atom_e = pe;
            *next = pe;
            return 0;
        }
        *atom_e = pi + 2;
        *next = pi + 2;
        return 0;
    }
    if (pat[pi] == '(') {
        int rp = find_rparen(pat, pi, pe);
        if (rp < 0) return -1;
        *atom_e = rp + 1;
        *next = rp + 1;
        return 0;
    }
    /* single-char atom: . ^ $ or literal */
    *atom_e = pi + 1;
    *next = pi + 1;
    return 0;
}

static int eqc(char a, char b, int ign) {
    if (ign) return lower(a) == lower(b);
    return a == b;
}

/* Forward: match pat[pi..pe) against s[si..se); must consume all of pattern.
 * Returns ending si on success (>= si), or -1. */
static int ere_match(const char *pat, int pi, int pe, const char *s, int si,
                     int se, int ign);

/* Match one atom pat[as..ae) once at s[si]; return new si or -1. */
static int ere_atom(const char *pat, int as, int ae, const char *s, int si,
                    int se, int ign) {
    char c;
    if (as >= ae) return si;
    c = pat[as];
    if (c == '\\') {
        if (as + 1 >= ae) {
            /* lone trailing \ — treat as literal backslash */
            if (si >= se || s[si] != '\\') return -1;
            return si + 1;
        }
        if (si >= se || !eqc(s[si], pat[as + 1], ign)) return -1;
        return si + 1;
    }
    if (c == '(') {
        /* group: pat[as+1 .. ae-1) */
        if (ae < as + 2 || pat[ae - 1] != ')') return -1;
        return ere_match(pat, as + 1, ae - 1, s, si, se, ign);
    }
    if (c == '^') {
        return (si == 0) ? si : -1;
    }
    if (c == '$') {
        return (si == se) ? si : -1;
    }
    if (c == '.') {
        if (si >= se) return -1;
        return si + 1;
    }
    /* literal */
    if (si >= se || !eqc(s[si], c, ign)) return -1;
    return si + 1;
}

static int ere_match(const char *pat, int pi, int pe, const char *s, int si,
                     int se, int ign) {
    int alt;
    int as, ae, nx, qi;
    char q;
    int k, pos, endpos, rest;

    if (pi >= pe) return si;

    alt = find_alt(pat, pi, pe);
    if (alt >= 0) {
        rest = ere_match(pat, pi, alt, s, si, se, ign);
        if (rest >= 0) return rest;
        return ere_match(pat, alt + 1, pe, s, si, se, ign);
    }

    /* sequence of quantified atoms */
    while (pi < pe) {
        if (parse_atom(pat, pi, pe, &as, &ae, &nx) < 0) return -1;
        q = 0;
        qi = nx;
        if (qi < pe && (pat[qi] == '*' || pat[qi] == '+' || pat[qi] == '?')) {
            q = pat[qi];
            qi++;
        }
        /* remaining pattern after this quantified atom */
        /* greedy with backtrack */
        if (q == 0) {
            pos = ere_atom(pat, as, ae, s, si, se, ign);
            if (pos < 0) return -1;
            si = pos;
            pi = qi;
            continue;
        }
        if (q == '?') {
            /* try 1 then 0 (greedy) */
            pos = ere_atom(pat, as, ae, s, si, se, ign);
            if (pos >= 0) {
                rest = ere_match(pat, qi, pe, s, pos, se, ign);
                if (rest >= 0) return rest;
            }
            return ere_match(pat, qi, pe, s, si, se, ign);
        }
        /* * or + : greedy collect then backtrack */
        {
            int positions[64];
            int np = 0;
            if (q == '*') {
                positions[np++] = si; /* zero reps */
            } else {
                pos = ere_atom(pat, as, ae, s, si, se, ign);
                if (pos < 0) return -1;
                positions[np++] = pos;
            }
            while (np < 64) {
                endpos = ere_atom(pat, as, ae, s, positions[np - 1], se, ign);
                if (endpos < 0) break;
                if (endpos == positions[np - 1]) break; /* empty-match guard */
                positions[np++] = endpos;
            }
            for (k = np - 1; k >= 0; k--) {
                rest = ere_match(pat, qi, pe, s, positions[k], se, ign);
                if (rest >= 0) return rest;
            }
            return -1;
        }
    }
    return si;
}

static int ere_search(const char *hay, const char *pat, int ign) {
    int plen = (int)strlen(pat);
    int hlen = (int)strlen(hay);
    int i;

    if (plen == 0) return 1;
    /* Always try each start; ^ inside an alt fails when si!=0. Avoids
     * wrongly anchoring patterns like ^a|b (b may match mid-line). */
    for (i = 0; i <= hlen; i++) {
        if (ere_match(pat, 0, plen, hay, i, hlen, ign) >= 0) return 1;
    }
    return 0;
}

static int line_match(const char *line, const char *pat, int ign, int ere) {
    if (ere) return ere_search(line, pat, ign);
    return contains(line, pat, ign);
}

/* Returns 0 ok / 1 read error. *matched set if any hit. *count_out = hits. */
static int grep_fd(long fd, const char *pat, int ign, int invert, int number,
                   int count_mode, int ere, int show_name, const char *name,
                   int *matched, unsigned long *count_out) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;
    long lineno = 0;
    int any = 0;
    unsigned long hits = 0;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                int hit;
                line[li] = 0;
                lineno++;
                hit = line_match(line, pat, ign, ere);
                if (invert) hit = !hit;
                if (hit) {
                    any = 1;
                    hits++;
                    if (!count_mode) {
                        if (show_name) printf("%s:", name);
                        if (number) printf("%u:", (unsigned)lineno);
                        if (li > 0) u_write(1, line, (unsigned long)li);
                        u_write(1, "\n", 1);
                    }
                }
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        int hit;
        line[li] = 0;
        lineno++;
        hit = line_match(line, pat, ign, ere);
        if (invert) hit = !hit;
        if (hit) {
            any = 1;
            hits++;
            if (!count_mode) {
                if (show_name) printf("%s:", name);
                if (number) printf("%u:", (unsigned)lineno);
                u_write(1, line, (unsigned long)li);
                u_write(1, "\n", 1);
            }
        }
    }
    if (count_mode) {
        if (show_name) printf("%s:", name);
        printf("%u\n", (unsigned)hits);
    }
    if (any) *matched = 1;
    if (count_out) *count_out = hits;
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int ign = 0;
    int invert = 0;
    int number = 0;
    int count_mode = 0;
    int ere = 0;
    int fixed = 0;
    int rc = 0;
    int matched = 0;
    int nfiles = 0;
    int file_start;
    const char *pat;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        int j;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-e") == 0) {
            break;
        }
        for (j = 1; argv[i][j]; j++) {
            char o = argv[i][j];
            if (o == 'i') ign = 1;
            else if (o == 'v') invert = 1;
            else if (o == 'n') number = 1;
            else if (o == 'c') count_mode = 1;
            else if (o == 'E') { ere = 1; fixed = 0; }
            else if (o == 'F') { fixed = 1; ere = 0; }
            else {
                u_write(2, USAGE, 40);
                return 2;
            }
        }
        i++;
    }

    if (fixed) ere = 0;

    if (i < argc && strcmp(argv[i], "-e") == 0) {
        if (i + 1 >= argc) {
            u_write(2, USAGE, 40);
            return 2;
        }
        pat = argv[i + 1];
        i += 2;
    } else {
        if (i >= argc) {
            u_write(2, USAGE, 40);
            return 2;
        }
        pat = argv[i];
        i++;
    }

    file_start = i;
    nfiles = argc - file_start;
    if (nfiles <= 0) {
        unsigned long hits = 0;
        if (grep_fd(0, pat, ign, invert, number, count_mode, ere, 0, 0,
                    &matched, &hits) != 0) return 2;
        return matched ? 0 : 1;
    }
    for (; i < argc; i++) {
        long fd;
        int show_name = (nfiles > 1);
        unsigned long hits = 0;
        if (strcmp(argv[i], "-") == 0) {
            if (grep_fd(0, pat, ign, invert, number, count_mode, ere, show_name,
                        "(standard input)", &matched, &hits) != 0) rc = 2;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            printf("grep: %s: no such file\n", argv[i]);
            rc = 2;
            continue;
        }
        if (grep_fd(fd, pat, ign, invert, number, count_mode, ere, show_name,
                    argv[i], &matched, &hits) != 0)
            rc = 2;
        u_close(fd);
    }
    if (rc == 2) return 2;
    return matched ? 0 : 1;
}
