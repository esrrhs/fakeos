/* fakeos userland - yes: repeatedly output a line (default "y") until write fails. */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);

enum { LINE_CAP = 512 };

static int emit_line(const char *line, unsigned long len) {
    long w;
    if (len == 0) {
        w = u_write(1, "\n", 1);
        return w == 1 ? 0 : 1;
    }
    w = u_write(1, line, len);
    if (w < 0 || (unsigned long)w != len) return 1;
    w = u_write(1, "\n", 1);
    return w == 1 ? 0 : 1;
}

int main(int argc, char **argv) {
    char line[LINE_CAP];
    unsigned long n = 0;
    int i;

    if (argc <= 1) {
        line[0] = 'y';
        line[1] = 0;
        n = 1;
    } else {
        for (i = 1; i < argc; i++) {
            unsigned long L = strlen(argv[i]);
            unsigned long j;
            if (i > 1) {
                if (n + 1 >= LINE_CAP) break;
                line[n++] = ' ';
            }
            for (j = 0; j < L && n + 1 < LINE_CAP; j++) {
                line[n++] = argv[i][j];
            }
        }
        line[n] = 0;
    }

    for (;;) {
        if (emit_line(line, n) != 0) return 1;
    }
}
