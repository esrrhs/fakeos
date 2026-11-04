/* fakeos userland - grep: fixed-string line filter; thin -E ERE subset.
 * Options: -i -v -n -F -c -q -l -L -E -w -x -o. Glued short opts. -F forces fixed
 * (wins over -E). -w word-boundary match; -x whole-line match.
 * -o only matching: print each non-empty match on its own line (-c counts
 * matches; -v+-o yields no match text). -c suppresses line/match text output;
 * multi-FILE prints name:count per file.
 * -q quiet: no stdout (status only; -c count also suppressed). May stop early.
 * -l list matching file names only; -L list non-matching names (last of -l/-L wins).
 * ERE subset: | . * + ? () [] ^ $ and \ escapes. No [:class:], no {} bounds.
 * []: ranges, leading ^ negate, leading ]/- literal; \ escapes next inside [].
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

static const char *USAGE = "usage: grep [-ivncqlLFEwxo] PATTERN [FILE...]\n";

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

static int is_word_char(char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '_') return 1;
    return 0;
}

/* Fixed-string whole-line equality (optional -i). */
static int line_eq(const char *hay, const char *needle, int ign) {
    unsigned long i;
    for (i = 0; ; i++) {
        char a = hay[i];
        char b = needle[i];
        if (ign) {
            a = lower(a);
            b = lower(b);
        }
        if (a != b) return 0;
        if (a == 0) return 1;
    }
}

/* Fixed-string word match: needle at a word boundary both sides. */
static int contains_word(const char *hay, const char *needle, int ign) {
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
        if (!ok) continue;
        if (i > 0 && is_word_char(hay[i - 1])) continue;
        if (hay[i + nlen] != 0 && is_word_char(hay[i + nlen])) continue;
        return 1;
    }
    return 0;
}

/* Find closing ']' for '[' at pi. First ] after optional ^ is literal member.
 * Returns index of ']' or -1. */
static int find_rbrack(const char *pat, int pi, int pe) {
    int i;
    if (pi >= pe || pat[pi] != '[') return -1;
    i = pi + 1;
    if (i < pe && pat[i] == '^') i++;
    if (i < pe && pat[i] == ']') i++; /* ] as first member */
    for (; i < pe; i++) {
        if (pat[i] == '\\' && i + 1 < pe) { i++; continue; }
        if (pat[i] == ']') return i;
    }
    return -1;
}

/* Find top-level '|' in pat[pi..pe), respecting () and []. Returns index or -1. */
static int find_alt(const char *pat, int pi, int pe) {
    int depth = 0;
    int i;
    for (i = pi; i < pe; i++) {
        char c = pat[i];
        if (c == '\\' && i + 1 < pe) { i++; continue; }
        if (c == '[') {
            int rb = find_rbrack(pat, i, pe);
            if (rb < 0) return -1;
            i = rb;
            continue;
        }
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
        if (c == '[') {
            int rb = find_rbrack(pat, i, pe);
            if (rb < 0) return -1;
            i = rb;
            continue;
        }
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
    if (pat[pi] == '[') {
        int rb = find_rbrack(pat, pi, pe);
        if (rb < 0) return -1;
        *atom_e = rb + 1;
        *next = rb + 1;
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

/* Match one char against class pat[as..ae) where pat[as]=='[' and pat[ae-1]==']'.
 * Returns 1 match / 0 no. No [:class:] named classes. */
static int class_match(const char *pat, int as, int ae, char ch, int ign) {
    int i;
    int neg = 0;
    int hit = 0;
    if (as >= ae || pat[as] != '[' || pat[ae - 1] != ']') return 0;
    i = as + 1;
    if (i < ae - 1 && pat[i] == '^') { neg = 1; i++; }
    while (i < ae - 1) {
        char a, b;
        if (pat[i] == '\\' && i + 1 < ae - 1) {
            a = pat[i + 1];
            i += 2;
            if (eqc(ch, a, ign)) hit = 1;
            continue;
        }
        a = pat[i];
        /* range a-b when '-' is not the last member */
        if (i + 2 < ae - 1 && pat[i + 1] == '-') {
            b = pat[i + 2];
            if (b == '\\' && i + 3 < ae - 1) {
                b = pat[i + 3];
                i += 4;
            } else {
                i += 3;
            }
            if (ign) {
                char c = lower(ch), lo = lower(a), hi = lower(b);
                if (lo <= hi) {
                    if (c >= lo && c <= hi) hit = 1;
                } else {
                    if (c >= hi && c <= lo) hit = 1;
                }
            } else {
                if (a <= b) {
                    if (ch >= a && ch <= b) hit = 1;
                } else {
                    if (ch >= b && ch <= a) hit = 1;
                }
            }
            continue;
        }
        if (eqc(ch, a, ign)) hit = 1;
        i++;
    }
    return neg ? !hit : hit;
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
    if (c == '[') {
        if (ae < as + 2 || pat[ae - 1] != ']') return -1;
        if (si >= se) return -1;
        if (!class_match(pat, as, ae, s[si], ign)) return -1;
        return si + 1;
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

static int ere_search(const char *hay, const char *pat, int ign, int word, int whole) {
    int plen = (int)strlen(pat);
    int hlen = (int)strlen(hay);
    int i;

    if (plen == 0) {
        if (whole) return hlen == 0;
        return 1;
    }
    if (whole) {
        int end = ere_match(pat, 0, plen, hay, 0, hlen, ign);
        return end == hlen;
    }
    /* Always try each start; ^ inside an alt fails when si!=0. Avoids
     * wrongly anchoring patterns like ^a|b (b may match mid-line). */
    for (i = 0; i <= hlen; i++) {
        int end = ere_match(pat, 0, plen, hay, i, hlen, ign);
        if (end < 0) continue;
        if (word) {
            if (i > 0 && is_word_char(hay[i - 1])) continue;
            if (end < hlen && is_word_char(hay[end])) continue;
        }
        return 1;
    }
    return 0;
}

static int line_match(const char *line, const char *pat, int ign, int ere,
                      int word, int whole) {
    if (ere) return ere_search(line, pat, ign, word, whole);
    if (whole) return line_eq(line, pat, ign);
    if (word) return contains_word(line, pat, ign);
    return contains(line, pat, ign);
}

/* Next non-empty match in hay at/after from. Sets *ms,*me to [start,end).
 * Returns 1 if found, 0 if none. Empty matches skipped (no -o flood). */
static int find_next_match(const char *hay, const char *pat, int ign, int ere,
                           int word, int whole, int from, int *ms, int *me) {
    int hlen = (int)strlen(hay);
    int plen = (int)strlen(pat);
    int i;

    if (from < 0) from = 0;
    if (from > hlen) return 0;

    if (ere) {
        if (whole) {
            int end;
            if (from > 0) return 0;
            end = ere_match(pat, 0, plen, hay, 0, hlen, ign);
            if (end != hlen) return 0;
            if (end == 0) return 0; /* empty whole line */
            *ms = 0;
            *me = end;
            return 1;
        }
        for (i = from; i <= hlen; i++) {
            int end = ere_match(pat, 0, plen, hay, i, hlen, ign);
            if (end < 0) continue;
            if (end == i) continue; /* skip empty */
            if (word) {
                if (i > 0 && is_word_char(hay[i - 1])) continue;
                if (end < hlen && is_word_char(hay[end])) continue;
            }
            *ms = i;
            *me = end;
            return 1;
        }
        return 0;
    }

    /* Fixed string. */
    if (plen == 0) return 0; /* no empty -o matches */
    if (whole) {
        if (from > 0) return 0;
        if (!line_eq(hay, pat, ign)) return 0;
        *ms = 0;
        *me = hlen;
        return 1;
    }
    for (i = from; i + plen <= hlen; i++) {
        int j;
        int ok = 1;
        for (j = 0; j < plen; j++) {
            char a = hay[i + j];
            char b = pat[j];
            if (ign) {
                a = lower(a);
                b = lower(b);
            }
            if (a != b) { ok = 0; break; }
        }
        if (!ok) continue;
        if (word) {
            if (i > 0 && is_word_char(hay[i - 1])) continue;
            if (hay[i + plen] != 0 && is_word_char(hay[i + plen])) continue;
        }
        *ms = i;
        *me = i + plen;
        return 1;
    }
    return 0;
}

/* Emit -o matches for one line. Returns 1 if any match printed/counted. */
static int emit_only(const char *line, const char *pat, int ign, int ere,
                     int word, int whole, int number, int count_mode, int quiet,
                     int show_name, const char *name, long lineno,
                     unsigned long *hits) {
    int from = 0;
    int any = 0;
    int ms, me;
    while (find_next_match(line, pat, ign, ere, word, whole, from, &ms, &me)) {
        any = 1;
        (*hits)++;
        if (quiet) return 1;
        if (!count_mode) {
            if (show_name) printf("%s:", name);
            if (number) printf("%u:", (unsigned)lineno);
            if (me > ms) u_write(1, line + ms, (unsigned long)(me - ms));
            u_write(1, "\n", 1);
        }
        from = me;
        if (whole) break;
        if (from <= ms) from = ms + 1; /* safety */
    }
    return any;
}

/* Returns 0 ok / 1 read error. *matched set if any hit. *count_out = hits. */
static int grep_fd(long fd, const char *pat, int ign, int invert, int number,
                   int count_mode, int quiet, int ere, int word, int whole,
                   int only_matching, int show_name, const char *name,
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
                if (only_matching) {
                    /* -v selects non-matching lines; those have no match text. */
                    if (!invert) {
                        if (emit_only(line, pat, ign, ere, word, whole, number,
                                      count_mode, quiet, show_name, name,
                                      lineno, &hits)) {
                            any = 1;
                            if (quiet) {
                                *matched = 1;
                                if (count_out) *count_out = hits;
                                return 0;
                            }
                        }
                    }
                } else {
                    hit = line_match(line, pat, ign, ere, word, whole);
                    if (invert) hit = !hit;
                    if (hit) {
                        any = 1;
                        hits++;
                        if (quiet) {
                            *matched = 1;
                            if (count_out) *count_out = hits;
                            return 0; /* early exit */
                        }
                        if (!count_mode) {
                            if (show_name) printf("%s:", name);
                            if (number) printf("%u:", (unsigned)lineno);
                            if (li > 0) u_write(1, line, (unsigned long)li);
                            u_write(1, "\n", 1);
                        }
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
        if (only_matching) {
            if (!invert) {
                if (emit_only(line, pat, ign, ere, word, whole, number,
                              count_mode, quiet, show_name, name, lineno,
                              &hits)) {
                    any = 1;
                    if (quiet) {
                        *matched = 1;
                        if (count_out) *count_out = hits;
                        return 0;
                    }
                }
            }
        } else {
            hit = line_match(line, pat, ign, ere, word, whole);
            if (invert) hit = !hit;
            if (hit) {
                any = 1;
                hits++;
                if (quiet) {
                    *matched = 1;
                    if (count_out) *count_out = hits;
                    return 0;
                }
                if (!count_mode) {
                    if (show_name) printf("%s:", name);
                    if (number) printf("%u:", (unsigned)lineno);
                    u_write(1, line, (unsigned long)li);
                    u_write(1, "\n", 1);
                }
            }
        }
    }
    if (count_mode && !quiet) {
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
    int quiet = 0;
    int list_yes = 0; /* -l */
    int list_no = 0;  /* -L */
    int ere = 0;
    int fixed = 0;
    int word = 0;
    int whole = 0;
    int only_matching = 0;
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
            else if (o == 'q') quiet = 1;
            else if (o == 'l') { list_yes = 1; list_no = 0; }
            else if (o == 'L') { list_no = 1; list_yes = 0; }
            else if (o == 'E') { ere = 1; fixed = 0; }
            else if (o == 'F') { fixed = 1; ere = 0; }
            else if (o == 'w') word = 1;
            else if (o == 'x') whole = 1;
            else if (o == 'o') only_matching = 1;
            else {
                u_write(2, USAGE, 46);
                return 2;
            }
        }
        i++;
    }

    if (fixed) ere = 0;

    if (i < argc && strcmp(argv[i], "-e") == 0) {
        if (i + 1 >= argc) {
            u_write(2, USAGE, 46);
            return 2;
        }
        pat = argv[i + 1];
        i += 2;
    } else {
        if (i >= argc) {
            u_write(2, USAGE, 46);
            return 2;
        }
        pat = argv[i];
        i++;
    }

    file_start = i;
    nfiles = argc - file_start;
    /* -l/-L: scan quietly then print path; -l may stop early via quiet. */
    {
        int q = quiet;
        int cmode = count_mode;
        if (list_yes || list_no) {
            q = 1;
            cmode = 0;
        }
        if (nfiles <= 0) {
            unsigned long hits = 0;
            int file_matched = 0;
            if (grep_fd(0, pat, ign, invert, number, cmode, q, ere, word, whole,
                        only_matching, 0, 0,
                        &file_matched, &hits) != 0) return 2;
            if (list_yes) {
                if (file_matched) {
                    printf("(standard input)\n");
                    return 0;
                }
                return 1;
            }
            if (list_no) {
                if (!file_matched) {
                    printf("(standard input)\n");
                    return 0;
                }
                return 1;
            }
            return file_matched ? 0 : 1;
        }
        for (; i < argc; i++) {
            long fd;
            int show_name = (nfiles > 1) && !(list_yes || list_no);
            unsigned long hits = 0;
            int file_matched = 0;
            const char *name;
            if (strcmp(argv[i], "-") == 0) {
                name = "(standard input)";
                if (grep_fd(0, pat, ign, invert, number, cmode, q, ere, word, whole,
                            only_matching, show_name,
                            name, &file_matched, &hits) != 0) {
                    rc = 2;
                    continue;
                }
            } else {
                name = argv[i];
                fd = u_open(argv[i], 0, 0);
                if (fd < 0) {
                    printf("grep: %s: no such file\n", argv[i]);
                    rc = 2;
                    continue;
                }
                if (grep_fd(fd, pat, ign, invert, number, cmode, q, ere, word, whole,
                            only_matching, show_name,
                            name, &file_matched, &hits) != 0)
                    rc = 2;
                u_close(fd);
            }
            if (list_yes) {
                if (file_matched) {
                    printf("%s\n", name);
                    matched = 1;
                }
            } else if (list_no) {
                if (!file_matched) {
                    printf("%s\n", name);
                    matched = 1; /* listed at least one non-match */
                }
            } else if (file_matched) {
                matched = 1;
            }
        }
    }
    if (rc == 2) return 2;
    return matched ? 0 : 1;
}
