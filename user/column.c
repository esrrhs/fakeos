/* fakeos userland - column: table + fill layout (thin).
 * Forms:
 *   column -t [-s SEP|-sSEP] [-o OSEP|-oOSEP] [FILE...]
 *   column [-x] [-c WIDTH|-cWIDTH] [FILE...]
 * -t: size columns from input and pad fields into a table.
 * Default field split is runs of spaces/tabs. -s SEP treats each character
 * in SEP as a separator (empty fields kept). -o OSEP sets the output
 * column separator (default two spaces).
 * Without -t: fill mode — whitespace words into a grid that fits -c WIDTH
 * (default 80); two spaces between columns. Default fills columns first
 * (column-major); -x fillrows uses row-major order.
 * Glued -sSEP/-oOSEP/-toOSEP/-cN/-xcN/-cxN keep argv under EXEC_MAX_ARGC=8.
 * stdin if no FILE. Bounded: MAX_LINES x MAX_COLS / MAX_WORDS.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);

enum {
    LINE_MAX = 256,
    MAX_LINES = 128,
    MAX_COLS = 16,
    FIELD_MAX = 64,
    MAX_WORDS = 256
};

static int table_mode;
static const char *sep = 0; /* NULL => whitespace runs */
static const char *out_sep = "  "; /* between columns on output */
static int out_width = 80; /* fill-mode output width (-c) */
static int fillrows; /* -x: row-major fill */

static char lines[MAX_LINES][LINE_MAX];
static int nlines;
static char fields[MAX_LINES][MAX_COLS][FIELD_MAX];
static int ncols_row[MAX_LINES];
static int col_width[MAX_COLS];
static int max_cols;

static char words[MAX_WORDS][FIELD_MAX];
static int nwords;

static void usage(void) {
    u_write(2, "usage: column [-t [-s SEP] [-o OSEP] | [-x] [-c WIDTH]] [FILE...]\n", 66);
}

static int is_ws(char c) {
    return c == ' ' || c == '\t';
}

static int is_sep_char(char c) {
    int i;
    if (!sep) return 0;
    for (i = 0; sep[i]; i++) {
        if (sep[i] == c) return 1;
    }
    return 0;
}

static int parse_pos(const char *s, int *out) {
    int v = 0;
    int i = 0;
    if (!s || !s[0]) return 1;
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 1;
        v = v * 10 + (s[i] - '0');
        if (v > 4096) return 1;
    }
    if (v < 1) return 1;
    *out = v;
    return 0;
}

static void split_line(int row) {
    const char *s = lines[row];
    int col = 0;
    int fi = 0;
    int i = 0;

    fields[row][0][0] = 0;
    ncols_row[row] = 0;

    if (!sep) {
        /* whitespace-run fields; leading/trailing ws ignored */
        while (s[i]) {
            while (is_ws(s[i])) i++;
            if (!s[i]) break;
            fi = 0;
            while (s[i] && !is_ws(s[i])) {
                if (fi < FIELD_MAX - 1) fields[row][col][fi++] = s[i];
                i++;
            }
            fields[row][col][fi] = 0;
            col++;
            if (col >= MAX_COLS) break;
        }
    } else {
        /* each sep char splits; empty fields kept */
        fi = 0;
        while (1) {
            if (!s[i] || is_sep_char(s[i])) {
                fields[row][col][fi] = 0;
                col++;
                if (!s[i] || col >= MAX_COLS) break;
                i++;
                fi = 0;
                continue;
            }
            if (fi < FIELD_MAX - 1) fields[row][col][fi++] = s[i];
            i++;
        }
    }
    ncols_row[row] = col;
    if (col > max_cols) max_cols = col;
}

static int load_fd(long fd) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int i;
    int li = 0;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (nlines >= MAX_LINES) return 1;
                if (li >= LINE_MAX) li = LINE_MAX - 1;
                line[li] = 0;
                {
                    int k;
                    for (k = 0; k <= li; k++) lines[nlines][k] = line[k];
                }
                nlines++;
                li = 0;
            } else {
                if (li < LINE_MAX - 1) line[li++] = c;
                else if (li < LINE_MAX) li = LINE_MAX; /* truncate mark */
            }
        }
    }
    if (li > 0 && nlines < MAX_LINES) {
        if (li >= LINE_MAX) li = LINE_MAX - 1;
        line[li] = 0;
        {
            int k;
            for (k = 0; k <= li; k++) lines[nlines][k] = line[k];
        }
        nlines++;
    }
    return n < 0 ? 1 : 0;
}

static void collect_words(void) {
    int r, i, fi;
    nwords = 0;
    for (r = 0; r < nlines; r++) {
        const char *s = lines[r];
        i = 0;
        while (s[i]) {
            while (is_ws(s[i])) i++;
            if (!s[i]) break;
            if (nwords >= MAX_WORDS) return;
            fi = 0;
            while (s[i] && !is_ws(s[i])) {
                if (fi < FIELD_MAX - 1) words[nwords][fi++] = s[i];
                i++;
            }
            words[nwords][fi] = 0;
            nwords++;
        }
    }
}

static void emit_table(void) {
    int r, c, k;
    int w;

    for (c = 0; c < MAX_COLS; c++) col_width[c] = 0;
    max_cols = 0;
    for (r = 0; r < nlines; r++) {
        split_line(r);
        for (c = 0; c < ncols_row[r]; c++) {
            w = (int)strlen(fields[r][c]);
            if (w > col_width[c]) col_width[c] = w;
        }
    }
    for (r = 0; r < nlines; r++) {
        for (c = 0; c < max_cols; c++) {
            const char *f = (c < ncols_row[r]) ? fields[r][c] : "";
            int fl = (int)strlen(f);
            if (fl > 0) u_write(1, f, (unsigned long)fl);
            if (c + 1 < max_cols) {
                int pad = col_width[c] - fl;
                if (pad < 0) pad = 0;
                for (k = 0; k < pad; k++) u_write(1, " ", 1);
                {
                    unsigned long ol = strlen(out_sep);
                    if (ol > 0) u_write(1, out_sep, ol);
                }
            }
        }
        u_write(1, "\n", 1);
    }
}

/* Fill grid. Default column-major; -x fillrows is row-major. */
static void emit_fill(void) {
    int maxlen = 0;
    int ncols, nrows;
    int r, c, k, idx;
    int w;

    collect_words();
    if (nwords == 0) return;
    for (k = 0; k < nwords; k++) {
        w = (int)strlen(words[k]);
        if (w > maxlen) maxlen = w;
    }
    if (maxlen < 1) maxlen = 1;
    /* cell = word + two inter-column spaces; last col needs no trailing gap */
    ncols = (out_width + 2) / (maxlen + 2);
    if (ncols < 1) ncols = 1;
    if (ncols > nwords) ncols = nwords;
    nrows = (nwords + ncols - 1) / ncols;

    for (r = 0; r < nrows; r++) {
        for (c = 0; c < ncols; c++) {
            if (fillrows) idx = r * ncols + c;
            else idx = c * nrows + r;
            if (idx >= nwords) {
                break;
            }
            w = (int)strlen(words[idx]);
            if (w > 0) u_write(1, words[idx], (unsigned long)w);
            {
                int more;
                if (fillrows) more = (c + 1 < ncols) && (idx + 1 < nwords);
                else more = (c + 1 < ncols) && ((c + 1) * nrows + r < nwords);
                if (more) {
                    int pad = maxlen - w;
                    if (pad < 0) pad = 0;
                    for (k = 0; k < pad; k++) u_write(1, " ", 1);
                    u_write(1, "  ", 2);
                }
            }
        }
        u_write(1, "\n", 1);
    }
}

static int column_fd(long fd) {
    nlines = 0;
    max_cols = 0;
    nwords = 0;
    if (load_fd(fd) != 0) return 1;
    if (table_mode) emit_table();
    else emit_fill();
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-") == 0) break;
        if (strcmp(argv[i], "-t") == 0) {
            table_mode = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-x") == 0) {
            fillrows = 1;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            if (parse_pos(argv[i + 1], &out_width) != 0) { usage(); return 1; }
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            sep = argv[i + 1];
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            out_sep = argv[i + 1];
            i += 2;
            continue;
        }
        /* glued fill flags: -cN / -x / -xcN / -cxN */
        if (argv[i][1] == 'c' || argv[i][1] == 'x') {
            int j = 1;
            int saw = 0;
            while (argv[i][j]) {
                if (argv[i][j] == 'x') {
                    fillrows = 1;
                    saw = 1;
                    j++;
                    continue;
                }
                if (argv[i][j] == 'c') {
                    if (argv[i][j + 1] != 0) {
                        if (parse_pos(argv[i] + j + 1, &out_width) != 0) {
                            usage();
                            return 1;
                        }
                        saw = 1;
                        i++;
                        goto next_arg;
                    }
                    if (i + 1 >= argc) { usage(); return 1; }
                    if (parse_pos(argv[i + 1], &out_width) != 0) {
                        usage();
                        return 1;
                    }
                    saw = 1;
                    i += 2;
                    goto next_arg;
                }
                usage();
                return 1;
            }
            if (!saw) { usage(); return 1; }
            i++;
            continue;
        }
        if (argv[i][1] == 's' && argv[i][2] != 0) {
            sep = argv[i] + 2;
            i++;
            continue;
        }
        if (argv[i][1] == 'o' && argv[i][2] != 0) {
            out_sep = argv[i] + 2;
            i++;
            continue;
        }
        /* combined -ts / -tsSEP / -to / -toOSEP */
        if (argv[i][1] == 't') {
            int j = 2;
            table_mode = 1;
            while (argv[i][j]) {
                if (argv[i][j] == 's') {
                    if (argv[i][j + 1] != 0) {
                        sep = argv[i] + j + 1;
                        i++;
                        goto next_arg;
                    }
                    if (i + 1 >= argc) { usage(); return 1; }
                    sep = argv[i + 1];
                    i += 2;
                    goto next_arg;
                }
                if (argv[i][j] == 'o') {
                    if (argv[i][j + 1] != 0) {
                        out_sep = argv[i] + j + 1;
                        i++;
                        goto next_arg;
                    }
                    if (i + 1 >= argc) { usage(); return 1; }
                    out_sep = argv[i + 1];
                    i += 2;
                    goto next_arg;
                }
                usage();
                return 1;
            }
            i++;
            continue;
        }
        usage();
        return 1;
    next_arg:
        continue;
    }

    /* fill mode is default; -t selects table. -s/-o without -t still ok (ignored). */

    if (i >= argc) {
        return column_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (column_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "column: open failed\n", 20);
            rc = 1;
            continue;
        }
        if (column_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
