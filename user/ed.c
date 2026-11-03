/* fakeos userland - tiny line editor (subset of ed).
 * Commands (at * prompt):
 *   p            print buffer
 *   n            print numbered
 *   a            append lines until a lone "."
 *   c            change: clear then append
 *   d            delete all lines
 *   w [file]     write buffer to file (default: path from argv)
 *   e [file]     edit/load file into buffer
 *   q            quit
 */
package user;

extern long u_read(long fd, void *buf, unsigned long n);
extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_lseek(long fd, long off, long whence);
extern void printf(const char *fmt, ...);
extern void puts(const char *s);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern int strcmp(const char *a, const char *b);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT = 0x40,
    O_TRUNC = 0x200,
    MAX_LINES = 64,
    LINE_LEN = 120
};

static char lines[MAX_LINES][LINE_LEN];
static int nlines = 0;
static char curfile[96];

static int read_line(char *buf, int cap) {
    int n = 0;
    while (n < cap - 1) {
        char c;
        long r = u_read(0, &c, 1);
        if (r <= 0) {
            if (n == 0) return -1;
            break;
        }
        if (c == '\n' || c == '\r') break;
        buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

static void cmd_print(int numbered) {
    int i;
    for (i = 0; i < nlines; i++) {
        if (numbered) {
            printf("%d\t%s\n", i + 1, lines[i]);
        } else {
            puts(lines[i]);
        }
    }
}

static void cmd_append(void) {
    char buf[LINE_LEN];
    for (;;) {
        if (read_line(buf, LINE_LEN) < 0) return;
        if (strcmp(buf, ".") == 0) return;
        if (nlines >= MAX_LINES) {
            puts("ed: buffer full");
            return;
        }
        strcpy(lines[nlines], buf);
        nlines++;
    }
}

static int load_file(const char *path) {
    long fd;
    char buf[LINE_LEN];
    int n = 0;
    int i;
    nlines = 0;
    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    for (;;) {
        char c;
        long r = u_read(fd, &c, 1);
        if (r <= 0) break;
        if (c == '\n') {
            buf[n] = 0;
            if (nlines < MAX_LINES) {
                strcpy(lines[nlines], buf);
                nlines++;
            }
            n = 0;
            continue;
        }
        if (n < LINE_LEN - 1) {
            buf[n++] = c;
        }
    }
    if (n > 0 && nlines < MAX_LINES) {
        buf[n] = 0;
        strcpy(lines[nlines], buf);
        nlines++;
    }
    u_close(fd);
    strcpy(curfile, path);
    (void)i;
    return 0;
}

static int save_file(const char *path) {
    long fd;
    int i;
    unsigned long total = 0;
    fd = u_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0x1A4);
    if (fd < 0) {
        puts("ed: cannot write file");
        return -1;
    }
    for (i = 0; i < nlines; i++) {
        unsigned long L = strlen(lines[i]);
        u_write(fd, lines[i], L);
        u_write(fd, "\n", 1);
        total += L + 1;
    }
    u_close(fd);
    strcpy(curfile, path);
    printf("%u\n", (unsigned int)total);
    return 0;
}

static void usage(void) {
    puts("ed commands: p n a c d w [file] e [file] q");
}

int main(int argc, char **argv) {
    char cmd[LINE_LEN];
    curfile[0] = 0;
    nlines = 0;
    if (argc >= 2) {
        if (load_file(argv[1]) < 0) {
            /* start empty; remember name for w */
            strcpy(curfile, argv[1]);
        }
    }
    for (;;) {
        u_write(1, "* ", 2);
        if (read_line(cmd, LINE_LEN) < 0) break;
        if (cmd[0] == 0) continue;
        if (strcmp(cmd, "q") == 0) return 0;
        if (strcmp(cmd, "p") == 0) {
            cmd_print(0);
            continue;
        }
        if (strcmp(cmd, "n") == 0) {
            cmd_print(1);
            continue;
        }
        if (strcmp(cmd, "a") == 0) {
            cmd_append();
            continue;
        }
        if (strcmp(cmd, "c") == 0) {
            nlines = 0;
            cmd_append();
            continue;
        }
        if (strcmp(cmd, "d") == 0) {
            nlines = 0;
            continue;
        }
        if (cmd[0] == 'w' && (cmd[1] == 0 || cmd[1] == ' ')) {
            const char *path = curfile;
            if (cmd[1] == ' ' && cmd[2] != 0) path = cmd + 2;
            if (path[0] == 0) {
                puts("ed: no file name");
                continue;
            }
            save_file(path);
            continue;
        }
        if (cmd[0] == 'e' && (cmd[1] == 0 || cmd[1] == ' ')) {
            const char *path = curfile;
            if (cmd[1] == ' ' && cmd[2] != 0) path = cmd + 2;
            if (path[0] == 0) {
                puts("ed: no file name");
                continue;
            }
            if (load_file(path) < 0) {
                puts("ed: cannot load file");
            }
            continue;
        }
        if (strcmp(cmd, "h") == 0 || strcmp(cmd, "?") == 0) {
            usage();
            continue;
        }
        puts("?");
    }
    return 0;
}
