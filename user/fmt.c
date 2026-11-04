/* fakeos userland - fmt: simple paragraph fill (thin POSIX subset).
 * Forms: fmt [-su] [-w WIDTH|-wWIDTH] [FILE...]
 * Default WIDTH 75. Joins whitespace-separated words into lines of at most
 * WIDTH columns; a single newline is just a word separator, blank lines
 * separate paragraphs. -s split-only: do not refill across newlines (each
 * input line wraps alone). -u uniform spacing: one space between words,
 * two after .?! sentence ends. Words longer than WIDTH stand alone on a
 * line. Glued -wN/-su/-swN keep argv under EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);

enum { WIDTH_DEFAULT = 75, WORD_MAX = 256, OUT_MAX = 512 };

static int width = WIDTH_DEFAULT;
static int opt_s; /* -s: split-only, no refill */
static int opt_u; /* -u: uniform spacing */
static int prev_sentence; /* last emitted word ended .?! */

static void usage(void) {
    u_write(2, "usage: fmt [-su] [-w WIDTH] [FILE...]\n", 38);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int sentence_end(const char *word, int wlen) {
    char c;
    if (wlen <= 0) return 0;
    c = word[wlen - 1];
    return c == '.' || c == '?' || c == '!';
}

static void emit_word(const char *word, int wlen, int *col) {
    int gap;
    int g;
    if (wlen <= 0) return;
    if (*col == 0) {
        u_write(1, word, (unsigned long)wlen);
        *col = wlen;
        prev_sentence = opt_u && sentence_end(word, wlen);
        return;
    }
    gap = 1;
    if (opt_u && prev_sentence) gap = 2;
    if (*col + gap + wlen <= width) {
        for (g = 0; g < gap; g++) u_write(1, " ", 1);
        u_write(1, word, (unsigned long)wlen);
        *col += gap + wlen;
        prev_sentence = opt_u && sentence_end(word, wlen);
        return;
    }
    u_write(1, "\n", 1);
    u_write(1, word, (unsigned long)wlen);
    *col = wlen;
    prev_sentence = opt_u && sentence_end(word, wlen);
}

static int fmt_fd(long fd) {
    char buf[512];
    char word[WORD_MAX];
    long n;
    int i;
    int wlen = 0;
    int col = 0;
    int last_was_nl = 0;

    prev_sentence = 0;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (wlen > 0) {
                    emit_word(word, wlen, &col);
                    wlen = 0;
                }
                if (last_was_nl) {
                    /* blank line → paragraph break (or extra blank) */
                    if (col > 0) {
                        u_write(1, "\n", 1);
                        col = 0;
                    }
                    u_write(1, "\n", 1);
                    prev_sentence = 0;
                } else if (opt_s) {
                    /* split-only: end the output line; do not join next */
                    if (col > 0) {
                        u_write(1, "\n", 1);
                        col = 0;
                    }
                    prev_sentence = 0;
                }
                last_was_nl = 1;
                continue;
            }
            if (is_space(c)) {
                if (wlen > 0) {
                    emit_word(word, wlen, &col);
                    wlen = 0;
                }
                last_was_nl = 0;
                continue;
            }
            last_was_nl = 0;
            if (wlen < WORD_MAX - 1) {
                word[wlen++] = c;
            }
        }
    }
    if (wlen > 0) {
        emit_word(word, wlen, &col);
    }
    if (col > 0) {
        u_write(1, "\n", 1);
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;

    width = WIDTH_DEFAULT;
    opt_s = 0;
    opt_u = 0;
    prev_sentence = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-s") == 0) {
            opt_s = 1;
            i++;
        } else if (strcmp(argv[i], "-u") == 0) {
            opt_u = 1;
            i++;
        } else if (strcmp(argv[i], "-w") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            width = (int)strtol_dec(argv[i + 1]);
            if (width < 1) width = 1;
            if (width > OUT_MAX - 1) width = OUT_MAX - 1;
            i += 2;
        } else if (argv[i][1] == 'w' && argv[i][2] != 0 && is_digits(argv[i] + 2)) {
            width = (int)strtol_dec(argv[i] + 2);
            if (width < 1) width = 1;
            if (width > OUT_MAX - 1) width = OUT_MAX - 1;
            i++;
        } else if (argv[i][1] == 's' && argv[i][2] == 'w' &&
                   argv[i][3] != 0 && is_digits(argv[i] + 3)) {
            opt_s = 1;
            width = (int)strtol_dec(argv[i] + 3);
            if (width < 1) width = 1;
            if (width > OUT_MAX - 1) width = OUT_MAX - 1;
            i++;
        } else if ((argv[i][1] == 's' || argv[i][1] == 'u') && argv[i][2] != 0) {
            /* glued -su / -us / -suwN / -uswN / -uwN */
            int j;
            int saw = 0;
            for (j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 's') { opt_s = 1; saw = 1; }
                else if (argv[i][j] == 'u') { opt_u = 1; saw = 1; }
                else if (argv[i][j] == 'w' && argv[i][j + 1] != 0 &&
                         is_digits(argv[i] + j + 1)) {
                    width = (int)strtol_dec(argv[i] + j + 1);
                    if (width < 1) width = 1;
                    if (width > OUT_MAX - 1) width = OUT_MAX - 1;
                    saw = 1;
                    break;
                } else {
                    usage();
                    return 1;
                }
            }
            if (!saw) { usage(); return 1; }
            i++;
        } else if (is_digits(argv[i] + 1)) {
            /* historic fmt -WIDTH */
            width = (int)strtol_dec(argv[i] + 1);
            if (width < 1) width = 1;
            if (width > OUT_MAX - 1) width = OUT_MAX - 1;
            i++;
        } else {
            usage();
            return 1;
        }
    }

    if (i >= argc) {
        return fmt_fd(0);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (fmt_fd(0) != 0) rc = 1;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            u_write(2, "fmt: open failed\n", 17);
            rc = 1;
            continue;
        }
        if (fmt_fd(fd) != 0) rc = 1;
        u_close(fd);
    }
    return rc;
}
