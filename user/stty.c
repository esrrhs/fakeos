/* fakeos userland - stty: thin termios / winsize control.
 * Forms:
 *   stty size              print "ROWS COLS" (TIOCGWINSZ; fixed 80x25)
 *   stty sane              restore ICANON/ECHO/ISIG/ICRNL defaults
 *   stty echo | -echo
 *   stty raw | -raw        raw clears ICANON/ECHO/ISIG; -raw restores them
 *   stty [-a]              dump a short settings line
 * No baud/speed, no per-char c_cc edits beyond sane defaults.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_ioctl(long fd, unsigned long req, unsigned long arg);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum {
    TCGETS = 0x5401,
    TCSETS = 0x5402,
    TIOCGWINSZ = 0x5413,
    /* Match kernel/tty.c termios flag bits. */
    F_ISIG = 0x00000001,
    F_ICANON = 0x00000002,
    F_ECHO = 0x00000008,
    F_ICRNL = 0x00000100,
    F_OPOST = 0x00000001,
    F_ONLCR = 0x00000004,
    F_CS8 = 0x00000030,
    F_CREAD = 0x00000080
};

static unsigned int get_u32(const char *b, int off) {
    return (unsigned int)(unsigned char)b[off]
         | ((unsigned int)(unsigned char)b[off + 1] << 8)
         | ((unsigned int)(unsigned char)b[off + 2] << 16)
         | ((unsigned int)(unsigned char)b[off + 3] << 24);
}

static void put_u32(char *b, int off, unsigned int v) {
    b[off] = (char)(v & 0xFF);
    b[off + 1] = (char)((v >> 8) & 0xFF);
    b[off + 2] = (char)((v >> 16) & 0xFF);
    b[off + 3] = (char)((v >> 24) & 0xFF);
}

static int tio_get(char *tio) {
    int i;
    for (i = 0; i < 64; i++) tio[i] = 0;
    return u_ioctl(0, (unsigned long)TCGETS, (unsigned long)tio) == 0;
}

static int tio_set(char *tio) {
    return u_ioctl(0, (unsigned long)TCSETS, (unsigned long)tio) == 0;
}

static void apply_sane(char *tio) {
    int i;
    for (i = 0; i < 64; i++) tio[i] = 0;
    put_u32(tio, 0, F_ICRNL);
    put_u32(tio, 4, F_OPOST | F_ONLCR);
    put_u32(tio, 8, F_CS8 | F_CREAD);
    put_u32(tio, 12, F_ISIG | F_ICANON | F_ECHO);
    /* c_cc: VINTR=^C VQUIT=^\ VERASE=DEL VKILL=^U VEOF=^D VMIN=1 VSUSP=^Z */
    tio[17 + 0] = 0x03;
    tio[17 + 1] = 0x1C;
    tio[17 + 2] = 0x7F;
    tio[17 + 3] = 0x15;
    tio[17 + 4] = 0x04;
    tio[17 + 6] = 1;
    tio[17 + 10] = 0x1A;
}

static int do_size(void) {
    char ws[8];
    int i;
    unsigned int row;
    unsigned int col;
    for (i = 0; i < 8; i++) ws[i] = 0;
    if (u_ioctl(0, (unsigned long)TIOCGWINSZ, (unsigned long)ws) != 0) {
        u_write(2, "stty: TIOCGWINSZ failed\n", 24);
        return 1;
    }
    row = (unsigned int)(unsigned char)ws[0] | ((unsigned int)(unsigned char)ws[1] << 8);
    col = (unsigned int)(unsigned char)ws[2] | ((unsigned int)(unsigned char)ws[3] << 8);
    printf("%u %u\n", row, col);
    return 0;
}

static int do_dump(void) {
    char tio[64];
    unsigned int lflag;
    unsigned int iflag;
    if (!tio_get(tio)) {
        u_write(2, "stty: not a tty\n", 16);
        return 1;
    }
    iflag = get_u32(tio, 0);
    lflag = get_u32(tio, 12);
    printf("speed 9600 baud; rows 25; columns 80; "
           "%sicanon %secho %sisig %sicrnl\n",
           (lflag & F_ICANON) ? "" : "-",
           (lflag & F_ECHO) ? "" : "-",
           (lflag & F_ISIG) ? "" : "-",
           (iflag & F_ICRNL) ? "" : "-");
    return 0;
}

static void usage(void) {
    u_write(2, "usage: stty [size|sane|echo|-echo|raw|-raw|-a]\n", 47);
}

int main(int argc, char **argv) {
    char tio[64];
    unsigned int lflag;
    int i;

    if (argc <= 1) {
        return do_dump();
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "size") == 0) {
            return do_size();
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "a") == 0) {
            return do_dump();
        } else if (strcmp(argv[i], "sane") == 0) {
            apply_sane(tio);
            if (!tio_set(tio)) {
                u_write(2, "stty: TCSETS failed\n", 20);
                return 1;
            }
        } else if (strcmp(argv[i], "echo") == 0 || strcmp(argv[i], "-echo") == 0) {
            if (!tio_get(tio)) {
                u_write(2, "stty: not a tty\n", 16);
                return 1;
            }
            lflag = get_u32(tio, 12);
            if (argv[i][0] == '-') lflag &= ~((unsigned int)F_ECHO);
            else lflag |= (unsigned int)F_ECHO;
            put_u32(tio, 12, lflag);
            if (!tio_set(tio)) {
                u_write(2, "stty: TCSETS failed\n", 20);
                return 1;
            }
        } else if (strcmp(argv[i], "raw") == 0 || strcmp(argv[i], "-raw") == 0) {
            if (!tio_get(tio)) {
                u_write(2, "stty: not a tty\n", 16);
                return 1;
            }
            if (argv[i][0] != '-') {
                /* raw: clear ICANON/ECHO/ISIG */
                lflag = get_u32(tio, 12);
                lflag &= ~((unsigned int)(F_ICANON | F_ECHO | F_ISIG));
                put_u32(tio, 12, lflag);
            } else {
                /* cooked-ish: restore ICANON/ECHO/ISIG */
                lflag = get_u32(tio, 12);
                lflag |= (unsigned int)(F_ICANON | F_ECHO | F_ISIG);
                put_u32(tio, 12, lflag);
            }
            if (!tio_set(tio)) {
                u_write(2, "stty: TCSETS failed\n", 20);
                return 1;
            }
        } else {
            usage();
            return 1;
        }
    }
    return 0;
}
