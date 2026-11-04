/* fakeos userland - wc: count lines, words, bytes, and max line length.
 *   wc [-lwcL] [FILE...]
 * Default (no flags): lines, words, bytes. -L is max line length (bytes
 * since last newline; unterminated final line counts). Glued flags ok.
 * Compact argv for EXEC_MAX_ARGC=8. stdin if no FILE.
 */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

struct counts {
    u64 lines;
    u64 words;
    u64 bytes;
    u64 maxlen;
};

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int wc_fd(long fd, struct counts *out) {
    char buf[512];
    long n;
    int in_word = 0;
    u64 cur = 0;

    out->lines = 0;
    out->words = 0;
    out->bytes = 0;
    out->maxlen = 0;
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        long i;
        out->bytes += (u64)n;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                out->lines++;
                if (cur > out->maxlen) out->maxlen = cur;
                cur = 0;
            } else {
                cur++;
            }
            if (is_space(c)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                out->words++;
            }
        }
    }
    if (cur > out->maxlen) out->maxlen = cur;
    return n < 0 ? 1 : 0;
}

static void print_counts(int show_l, int show_w, int show_c, int show_L,
                         struct counts *c, const char *name) {
    if (show_l) printf(" %u", (unsigned)c->lines);
    if (show_w) printf(" %u", (unsigned)c->words);
    if (show_c) printf(" %u", (unsigned)c->bytes);
    if (show_L) printf(" %u", (unsigned)c->maxlen);
    if (name != 0 && name[0] != 0) printf(" %s", name);
    printf("\n");
}

int main(int argc, char **argv) {
    int i = 1;
    int show_l = 0;
    int show_w = 0;
    int show_c = 0;
    int show_L = 0;
    int any_flag = 0;
    int rc = 0;
    int nfiles = 0;
    struct counts total;
    struct counts one;

    total.lines = 0;
    total.words = 0;
    total.bytes = 0;
    total.maxlen = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        int j;
        if (strcmp(argv[i], "-") == 0) break;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        for (j = 1; argv[i][j]; j++) {
            char o = argv[i][j];
            if (o == 'l') { show_l = 1; any_flag = 1; }
            else if (o == 'w') { show_w = 1; any_flag = 1; }
            else if (o == 'c') { show_c = 1; any_flag = 1; }
            else if (o == 'L') { show_L = 1; any_flag = 1; }
            else {
                u_write(2, "usage: wc [-lwcL] [FILE...]\n", 28);
                return 1;
            }
        }
        i++;
    }
    if (!any_flag) {
        show_l = 1;
        show_w = 1;
        show_c = 1;
    }

    if (i >= argc) {
        if (wc_fd(0, &one) != 0) return 1;
        print_counts(show_l, show_w, show_c, show_L, &one, 0);
        return 0;
    }

    for (; i < argc; i++) {
        long fd;
        const char *name = argv[i];
        if (strcmp(argv[i], "-") == 0) {
            fd = 0;
            name = "";
        } else {
            fd = u_open(argv[i], 0, 0);
            if (fd < 0) {
                printf("wc: %s: no such file\n", argv[i]);
                rc = 1;
                continue;
            }
        }
        if (wc_fd(fd, &one) != 0) {
            rc = 1;
        } else {
            print_counts(show_l, show_w, show_c, show_L, &one, name);
            total.lines += one.lines;
            total.words += one.words;
            total.bytes += one.bytes;
            if (one.maxlen > total.maxlen) total.maxlen = one.maxlen;
            nfiles++;
        }
        if (fd > 2) u_close(fd);
    }
    if (nfiles > 1) {
        print_counts(show_l, show_w, show_c, show_L, &total, "total");
    }
    return rc;
}
