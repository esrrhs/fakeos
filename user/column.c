/* fakeos userland - column: table layout (thin).
 * Forms: column -t [-s SEP|-sSEP] [FILE...]
 * -t required: determine column widths and pad fields into a table.
 * Default field split is runs of spaces/tabs. -s SEP treats each character
 * in SEP as a separator (empty fields kept). Columns separated by two
 * spaces on output. Glued -sSEP keeps argv under EXEC_MAX_ARGC=8.
 * stdin if no FILE. Bounded: MAX_LINES x MAX_COLS.
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
    FIELD_MAX = 64
};

static int table_mode;
static const char *sep = 0; /* NULL => whitespace runs */

static char lines[MAX_LINES][LINE_MAX];
static int nlines;
static char fields[MAX_LINES][MAX_COLS][FIELD_MAX];
static int ncols_row[MAX_LINES];
static int col_width[MAX_COLS];
static int max_cols;

static void usage(void) {
    u_write(2, "usage: column -t [-s SEP] [FILE...]\n", 36);
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
                u_write(1, "  ", 2);
            }
        }
        u_write(1, "\n", 1);
    }
}

static int column_fd(long fd) {
    nlines = 0;
    max_cols = 0;
    if (load_fd(fd) != 0) return 1;
    emit_table();
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
        if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            sep = argv[i + 1];
            i += 2;
            continue;
        }
        if (argv[i][1] == 's' && argv[i][2] != 0) {
            sep = argv[i] + 2;
            i++;
            continue;
        }
        /* allow combined -ts / -ts: */
        if (argv[i][1] == 't' && argv[i][2] == 's') {
            table_mode = 1;
            if (argv[i][3] != 0) {
                sep = argv[i] + 3;
                i++;
            } else {
                if (i + 1 >= argc) { usage(); return 1; }
                sep = argv[i + 1];
                i += 2;
            }
            continue;
        }
        if (argv[i][1] == 't' && argv[i][2] == 0) {
            /* already handled -t */
        }
        usage();
        return 1;
    }

    if (!table_mode) {
        usage();
        return 1;
    }

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
