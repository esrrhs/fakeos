/* fakeos userland - reset: restore sane termios + clear screen (thin).
 * Same sane defaults as /bin/stty sane, then ANSI clear (serial-friendly).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_ioctl(long fd, unsigned long req, unsigned long arg);

enum {
    TCSETS = 0x5402,
    F_ISIG = 0x00000001,
    F_ICANON = 0x00000002,
    F_ECHO = 0x00000008,
    F_ICRNL = 0x00000100,
    F_OPOST = 0x00000001,
    F_ONLCR = 0x00000004,
    F_CS8 = 0x00000030,
    F_CREAD = 0x00000080
};

static void put_u32(char *b, int off, unsigned int v) {
    b[off] = (char)(v & 0xFF);
    b[off + 1] = (char)((v >> 8) & 0xFF);
    b[off + 2] = (char)((v >> 16) & 0xFF);
    b[off + 3] = (char)((v >> 24) & 0xFF);
}

int main(int argc, char **argv) {
    char tio[64];
    int i;
    (void)argc;
    (void)argv;
    for (i = 0; i < 64; i++) tio[i] = 0;
    put_u32(tio, 0, F_ICRNL);
    put_u32(tio, 4, F_OPOST | F_ONLCR);
    put_u32(tio, 8, F_CS8 | F_CREAD);
    put_u32(tio, 12, F_ISIG | F_ICANON | F_ECHO);
    tio[17 + 0] = 0x03;
    tio[17 + 1] = 0x1C;
    tio[17 + 2] = 0x7F;
    tio[17 + 3] = 0x15;
    tio[17 + 4] = 0x04;
    tio[17 + 6] = 1;
    tio[17 + 10] = 0x1A;
    /* Best-effort: ignore TCSETS failure (e.g. redirected stdin). */
    u_ioctl(0, (unsigned long)TCSETS, (unsigned long)tio);
    u_write(1, "\033[2J\033[H", 7);
    return 0;
}
