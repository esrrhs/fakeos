/* fakeos userland - join: relational join on sorted text files (thin POSIX).
 * Forms: join [-a N|-aN] [-v N|-vN] [-e STR|-eSTR] [-t CHAR|-tCHAR]
 *        [-1 N|-1N] [-2 N|-2N] FILE1 FILE2
 * Default join field 1 on both; default field sep is blank runs; with -t the
 * delimiter is a single char (empty fields allowed). -e STR replaces empty
 * output fields with STR. Output sep is space, or the -t char when set.
 * -a N also prints unpairable lines from file N (1 or 2). -v N like -a N but
 * suppress pairable (joined) lines. "-" is stdin (at most one). Files must be
 * sorted on the join field. Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern long strtol_dec(const char *s);

enum { LINE_MAX = 512, MAX_FIELDS = 32 };

static int field1 = 1;
static int field2 = 1;
static int have_t;
static char tchar;
static int opt_a1;
static int opt_a2;
static int opt_v1;
static int opt_v2;
static const char *empty_repl; /* -e: replace empty fields */

static void usage(void) {
    u_write(2, "usage: join [-a N] [-v N] [-e STR] [-t C] [-1 N] [-2 N] FILE1 FILE2\n", 68);
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

static int read_line(long fd, char *out, int *len_out) {
    char c;
    long n;
    int li = 0;
    while (1) {
        n = u_read(fd, &c, 1);
        if (n < 0) return -1;
        if (n == 0) {
            *len_out = li;
            return li > 0 ? 1 : 0;
        }
        if (c == '\n') {
            out[li] = 0;
            *len_out = li;
            return 1;
        }
        if (li < LINE_MAX - 1) out[li++] = c;
    }
}

/* Split line into fields. Returns field count. Pointers into `line` (mutated). */
static int split_fields(char *line, char **fields, int maxf) {
    int n = 0;
    char *p = line;

    if (have_t) {
        while (n < maxf) {
            fields[n++] = p;
            while (*p && *p != tchar) p++;
            if (*p == 0) break;
            *p++ = 0;
        }
        return n;
    }

    while (*p && n < maxf) {
        while (is_blank(*p)) p++;
        if (*p == 0) break;
        fields[n++] = p;
        while (*p && !is_blank(*p)) p++;
        if (*p) {
            *p++ = 0;
        }
    }
    return n;
}

static const char *get_field(char **fields, int nf, int idx) {
    if (idx < 1 || idx > nf) return "";
    return fields[idx - 1];
}

static void emit_str(const char *s) {
    unsigned long n;
    if (s == 0 || s[0] == 0) {
        if (empty_repl) s = empty_repl;
        else return;
    }
    n = strlen(s);
    if (n > 0) u_write(1, s, n);
}

static void emit_sep(void) {
    if (have_t) u_write(1, &tchar, 1);
    else u_write(1, " ", 1);
}

static void emit_join(char **fa, int na, int ja, char **fb, int nb, int jb) {
    int i;
    int first = 1;

    /* join field */
    emit_str(get_field(fa, na, ja));
    first = 0;

    /* remaining fields from file A (skip join field) */
    for (i = 1; i <= na; i++) {
        if (i == ja) continue;
        if (!first) emit_sep();
        emit_str(get_field(fa, na, i));
        first = 0;
    }
    /* remaining fields from file B */
    for (i = 1; i <= nb; i++) {
        if (i == jb) continue;
        if (!first) emit_sep();
        emit_str(get_field(fb, nb, i));
        first = 0;
    }
    u_write(1, "\n", 1);
}

/* Unpairable line: join field + remaining fields from that file only. */
static void emit_unpaired(char **f, int n, int j) {
    char *empty[1];
    empty[0] = "";
    emit_join(f, n, j, empty, 0, 1);
}

static int open_in(const char *path, long *fd_out) {
    long fd;
    if (strcmp(path, "-") == 0) {
        *fd_out = 0;
        return 0;
    }
    fd = u_open(path, 0, 0);
    if (fd < 0) {
        u_write(2, "join: open failed\n", 18);
        return 1;
    }
    *fd_out = fd;
    return 0;
}

static int load_line(long fd, char *buf, char **fields, int *nf, int *have) {
    int len = 0;
    int r = read_line(fd, buf, &len);
    if (r < 0) return -1;
    if (r == 0) {
        *have = 0;
        *nf = 0;
        return 0;
    }
    *have = 1;
    *nf = split_fields(buf, fields, MAX_FIELDS);
    return 0;
}

static int parse_a_fileno(const char *s) {
    if (s == 0 || !is_digits(s)) return 0;
    if (s[0] == '1' && s[1] == 0) return 1;
    if (s[0] == '2' && s[1] == 0) return 2;
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    long fd1;
    long fd2;
    char buf1[LINE_MAX];
    char buf2[LINE_MAX];
    char *f1[MAX_FIELDS];
    char *f2[MAX_FIELDS];
    int n1 = 0;
    int n2 = 0;
    int have1 = 0;
    int have2 = 0;
    int stdin_used = 0;

    empty_repl = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-t") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == 0) { usage(); return 1; }
            have_t = 1;
            tchar = argv[i + 1][0];
            i += 2;
        } else if (a[1] == 't' && a[2] != 0) {
            have_t = 1;
            tchar = a[2];
            i++;
        } else if (strcmp(a, "-e") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            empty_repl = argv[i + 1];
            i += 2;
        } else if (a[1] == 'e' && a[2] != 0) {
            empty_repl = a + 2;
            i++;
        } else if (strcmp(a, "-a") == 0) {
            int which;
            if (i + 1 >= argc) { usage(); return 1; }
            which = parse_a_fileno(argv[i + 1]);
            if (which == 0) { usage(); return 1; }
            if (which == 1) opt_a1 = 1;
            else opt_a2 = 1;
            i += 2;
        } else if (a[1] == 'a' && a[2] != 0) {
            int which = parse_a_fileno(a + 2);
            if (which == 0) { usage(); return 1; }
            if (which == 1) opt_a1 = 1;
            else opt_a2 = 1;
            i++;
        } else if (strcmp(a, "-v") == 0) {
            int which;
            if (i + 1 >= argc) { usage(); return 1; }
            which = parse_a_fileno(argv[i + 1]);
            if (which == 0) { usage(); return 1; }
            if (which == 1) { opt_v1 = 1; opt_a1 = 1; }
            else { opt_v2 = 1; opt_a2 = 1; }
            i += 2;
        } else if (a[1] == 'v' && a[2] != 0) {
            int which = parse_a_fileno(a + 2);
            if (which == 0) { usage(); return 1; }
            if (which == 1) { opt_v1 = 1; opt_a1 = 1; }
            else { opt_v2 = 1; opt_a2 = 1; }
            i++;
        } else if (strcmp(a, "-1") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            field1 = (int)strtol_dec(argv[i + 1]);
            if (field1 < 1) field1 = 1;
            i += 2;
        } else if (a[1] == '1' && a[2] != 0 && is_digits(a + 2)) {
            field1 = (int)strtol_dec(a + 2);
            if (field1 < 1) field1 = 1;
            i++;
        } else if (strcmp(a, "-2") == 0) {
            if (i + 1 >= argc || !is_digits(argv[i + 1])) { usage(); return 1; }
            field2 = (int)strtol_dec(argv[i + 1]);
            if (field2 < 1) field2 = 1;
            i += 2;
        } else if (a[1] == '2' && a[2] != 0 && is_digits(a + 2)) {
            field2 = (int)strtol_dec(a + 2);
            if (field2 < 1) field2 = 1;
            i++;
        } else {
            usage();
            return 1;
        }
    }
    if (i + 2 != argc) {
        usage();
        return 1;
    }
    if (strcmp(argv[i], "-") == 0) stdin_used = 1;
    if (strcmp(argv[i + 1], "-") == 0 && stdin_used) {
        u_write(2, "join: only one stdin\n", 21);
        return 1;
    }
    if (open_in(argv[i], &fd1) != 0) return 1;
    if (open_in(argv[i + 1], &fd2) != 0) {
        if (fd1 != 0) u_close(fd1);
        return 1;
    }

    if (load_line(fd1, buf1, f1, &n1, &have1) < 0) return 1;
    if (load_line(fd2, buf2, f2, &n2, &have2) < 0) return 1;

    while (have1 && have2) {
        const char *k1 = get_field(f1, n1, field1);
        const char *k2 = get_field(f2, n2, field2);
        int cmp = strcmp(k1, k2);
        if (cmp < 0) {
            if (opt_a1) emit_unpaired(f1, n1, field1);
            if (load_line(fd1, buf1, f1, &n1, &have1) < 0) return 1;
        } else if (cmp > 0) {
            if (opt_a2) emit_unpaired(f2, n2, field2);
            if (load_line(fd2, buf2, f2, &n2, &have2) < 0) return 1;
        } else {
            /* Match: for thin subset emit one pair then advance both.
             * (Full POSIX would cartesian-expand equal-key runs.)
             * -v suppresses pairable output. */
            if (!opt_v1 && !opt_v2)
                emit_join(f1, n1, field1, f2, n2, field2);
            if (load_line(fd1, buf1, f1, &n1, &have1) < 0) return 1;
            if (load_line(fd2, buf2, f2, &n2, &have2) < 0) return 1;
        }
    }

    /* Drain unpairable leftovers. */
    while (have1) {
        if (opt_a1) emit_unpaired(f1, n1, field1);
        if (load_line(fd1, buf1, f1, &n1, &have1) < 0) return 1;
    }
    while (have2) {
        if (opt_a2) emit_unpaired(f2, n2, field2);
        if (load_line(fd2, buf2, f2, &n2, &have2) < 0) return 1;
    }

    if (fd1 != 0) u_close(fd1);
    if (fd2 != 0) u_close(fd2);
    return 0;
}
