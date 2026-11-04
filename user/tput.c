/* fakeos userland - tput: thin terminfo-ish helpers (no terminfo DB).
 * Forms:
 *   tput cols | columns   print columns (TIOCGWINSZ; fixed 80)
 *   tput lines            print rows (fixed 25)
 *   tput clear            ANSI clear screen (same as /bin/clear)
 *   tput cup ROW COL      CSI CUP (1-based args like ncurses tput)
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_ioctl(long fd, unsigned long req, unsigned long arg);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum { TIOCGWINSZ = 0x5413 };

static int parse_u(const char *s, unsigned int *out) {
    unsigned int v = 0;
    int i = 0;
    if (s == 0 || s[0] == 0) return 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10u + (unsigned int)(s[i] - '0');
        i++;
        if (i > 5) return 0;
    }
    if (s[i] != 0) return 0;
    *out = v;
    return 1;
}

static int get_winsize(unsigned int *row, unsigned int *col) {
    char ws[8];
    int i;
    for (i = 0; i < 8; i++) ws[i] = 0;
    if (u_ioctl(0, (unsigned long)TIOCGWINSZ, (unsigned long)ws) != 0) {
        /* fall back to VGA defaults when stdin is not a tty */
        *row = 25;
        *col = 80;
        return 0;
    }
    *row = (unsigned int)(unsigned char)ws[0] | ((unsigned int)(unsigned char)ws[1] << 8);
    *col = (unsigned int)(unsigned char)ws[2] | ((unsigned int)(unsigned char)ws[3] << 8);
    return 1;
}

static void usage(void) {
    u_write(2, "usage: tput cols|lines|clear|cup ROW COL\n", 41);
}

int main(int argc, char **argv) {
    unsigned int row;
    unsigned int col;
    unsigned int r;
    unsigned int c;

    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "cols") == 0 || strcmp(argv[1], "columns") == 0) {
        get_winsize(&row, &col);
        printf("%u\n", col);
        return 0;
    }
    if (strcmp(argv[1], "lines") == 0) {
        get_winsize(&row, &col);
        printf("%u\n", row);
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0) {
        u_write(1, "\033[2J\033[H", 7);
        return 0;
    }
    if (strcmp(argv[1], "cup") == 0) {
        if (argc < 4 || !parse_u(argv[2], &r) || !parse_u(argv[3], &c)) {
            usage();
            return 1;
        }
        /* CSI row;col H — 1-based like ncurses tput */
        printf("\033[%u;%uH", r, c);
        return 0;
    }
    usage();
    return 1;
}
