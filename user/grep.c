/* fakeos userland - grep: fixed-string line filter (thin; no BRE). */
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

static int grep_fd(long fd, const char *pat, int ign, int invert, int number,
                   int show_name, const char *name, int *matched) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;
    long lineno = 0;
    int any = 0;

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                int hit;
                line[li] = 0;
                lineno++;
                hit = contains(line, pat, ign);
                if (invert) hit = !hit;
                if (hit) {
                    any = 1;
                    if (show_name) printf("%s:", name);
                    if (number) printf("%u:", (unsigned)lineno);
                    if (li > 0) u_write(1, line, (unsigned long)li);
                    u_write(1, "\n", 1);
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
        hit = contains(line, pat, ign);
        if (invert) hit = !hit;
        if (hit) {
            any = 1;
            if (show_name) printf("%s:", name);
            if (number) printf("%u:", (unsigned)lineno);
            u_write(1, line, (unsigned long)li);
            u_write(1, "\n", 1);
        }
    }
    if (any) *matched = 1;
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int ign = 0;
    int invert = 0;
    int number = 0;
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
            else if (o == 'F') { /* fixed-string (always); accept */ }
            else {
                u_write(2, "usage: grep [-ivnF] PATTERN [FILE...]\n", 38);
                return 2;
            }
        }
        i++;
    }

    if (i < argc && strcmp(argv[i], "-e") == 0) {
        if (i + 1 >= argc) {
            u_write(2, "usage: grep [-ivnF] PATTERN [FILE...]\n", 38);
            return 2;
        }
        pat = argv[i + 1];
        i += 2;
    } else {
        if (i >= argc) {
            u_write(2, "usage: grep [-ivnF] PATTERN [FILE...]\n", 38);
            return 2;
        }
        pat = argv[i];
        i++;
    }

    file_start = i;
    nfiles = argc - file_start;
    if (nfiles <= 0) {
        if (grep_fd(0, pat, ign, invert, number, 0, 0, &matched) != 0) return 2;
        return matched ? 0 : 1;
    }
    for (; i < argc; i++) {
        long fd;
        int show_name = (nfiles > 1);
        if (strcmp(argv[i], "-") == 0) {
            if (grep_fd(0, pat, ign, invert, number, show_name, "(standard input)",
                        &matched) != 0) rc = 2;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            printf("grep: %s: no such file\n", argv[i]);
            rc = 2;
            continue;
        }
        if (grep_fd(fd, pat, ign, invert, number, show_name, argv[i], &matched) != 0)
            rc = 2;
        u_close(fd);
    }
    if (rc == 2) return 2;
    return matched ? 0 : 1;
}
