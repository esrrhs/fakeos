/* fakeos userland - join: relational join on sorted text files (thin POSIX).
 * Forms: join [-t CHAR|-tCHAR] [-1 N|-1N] [-2 N|-2N] FILE1 FILE2
 * Default join field 1 on both; default field sep is blank runs; with -t the
 * delimiter is a single char (empty fields allowed). Output sep is space, or
 * the -t char when set. "-" is stdin (at most one). Files must be sorted on
 * the join field.
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

static void usage(void) {
    u_write(2, "usage: join [-t C] [-1 N] [-2 N] FILE1 FILE2\n", 45);
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
    unsigned long n = strlen(s);
    if (n > 0) u_write(1, s, n);
}

static void emit_sep(void) {
    if (have_t) u_write(1, &tchar, 1);
    else u_write(1, " ", 1);
}

static void emit_join(char **f1, int n1, int j1, char **f2, int n2, int j2) {
    int i;
    int first = 1;

    /* join field */
    emit_str(get_field(f1, n1, j1));
    first = 0;

    /* remaining fields from file1 (skip join field) */
    for (i = 1; i <= n1; i++) {
        if (i == j1) continue;
        if (!first) emit_sep();
        emit_str(get_field(f1, n1, i));
        first = 0;
    }
    /* remaining fields from file2 */
    for (i = 1; i <= n2; i++) {
        if (i == j2) continue;
        if (!first) emit_sep();
        emit_str(get_field(f2, n2, i));
        first = 0;
    }
    u_write(1, "\n", 1);
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
            if (load_line(fd1, buf1, f1, &n1, &have1) < 0) return 1;
        } else if (cmp > 0) {
            if (load_line(fd2, buf2, f2, &n2, &have2) < 0) return 1;
        } else {
            /* Match: for thin subset emit one pair then advance both.
             * (Full POSIX would cartesian-expand equal-key runs.) */
            emit_join(f1, n1, field1, f2, n2, field2);
            if (load_line(fd1, buf1, f1, &n1, &have1) < 0) return 1;
            if (load_line(fd2, buf2, f2, &n2, &have2) < 0) return 1;
        }
    }

    if (fd1 != 0) u_close(fd1);
    if (fd2 != 0) u_close(fd2);
    return 0;
}
