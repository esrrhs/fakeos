/* fakeos userland - file: thin magic type guess.
 * Forms: file FILE...
 * Uses lstat for non-regular types; reads a small prefix for regular files.
 * Recognizes empty, ELF, #! scripts, gzip, PNG, ASCII text, otherwise data.
 * Compact argv (no flags) for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_lstat(const char *path, void *statbuf);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum {
    O_RDONLY = 0,
    ST_MODE = 24,
    ST_SIZE = 48,
    ST_SIZE_BYTES = 144,
    S_IFMT = 0xF000,
    S_IFDIR = 0x4000,
    S_IFCHR = 0x2000,
    S_IFIFO = 0x1000,
    S_IFSOCK = 0xC000,
    S_IFLNK = 0xA000,
    S_IFREG = 0x8000,
    PREFIX = 256
};

static void usage(void) {
    u_write(2, "usage: file FILE...\n", 20);
}

static unsigned int load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
}

static unsigned long load_u64(const char *p) {
    unsigned long v = 0;
    unsigned long i;
    for (i = 0; i < 8; i++) {
        v |= ((unsigned long)(unsigned char)p[i]) << (8 * i);
    }
    return v;
}

static int is_ascii_buf(const unsigned char *buf, long n) {
    long i;
    if (n <= 0) return 1;
    for (i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == 0) return 0;
        if (c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v') continue;
        if (c < 32 || c > 126) return 0;
    }
    return 1;
}

static void emit_result(const char *path, const char *desc) {
    printf("%s: %s\n", path, desc);
}

static int file_one(const char *path) {
    char st[ST_SIZE_BYTES];
    unsigned int mode;
    unsigned long size;
    long fd;
    unsigned char buf[PREFIX];
    long n;
    unsigned int t;

    if (u_lstat(path, st) != 0) {
        emit_result(path, "cannot open");
        return 1;
    }
    mode = load_u32(st + ST_MODE);
    size = load_u64(st + ST_SIZE);
    t = mode & S_IFMT;

    if (t == S_IFDIR) {
        emit_result(path, "directory");
        return 0;
    }
    if (t == S_IFLNK) {
        emit_result(path, "symbolic link");
        return 0;
    }
    if (t == S_IFCHR) {
        emit_result(path, "character special");
        return 0;
    }
    if (t == S_IFIFO) {
        emit_result(path, "fifo");
        return 0;
    }
    if (t == S_IFSOCK) {
        emit_result(path, "socket");
        return 0;
    }
    if (t != S_IFREG) {
        emit_result(path, "data");
        return 0;
    }
    if (size == 0) {
        emit_result(path, "empty");
        return 0;
    }

    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) {
        emit_result(path, "cannot open");
        return 1;
    }
    n = u_read(fd, buf, PREFIX);
    u_close(fd);
    if (n < 0) {
        emit_result(path, "cannot open");
        return 1;
    }
    if (n == 0) {
        emit_result(path, "empty");
        return 0;
    }

    if (n >= 4 && buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' &&
        buf[3] == 'F') {
        if (n >= 5 && buf[4] == 2) emit_result(path, "ELF 64-bit LSB executable");
        else if (n >= 5 && buf[4] == 1) emit_result(path, "ELF 32-bit LSB executable");
        else emit_result(path, "ELF");
        return 0;
    }
    if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
        emit_result(path, "script text");
        return 0;
    }
    if (n >= 2 && buf[0] == 0x1f && buf[1] == 0x8b) {
        emit_result(path, "gzip compressed data");
        return 0;
    }
    if (n >= 4 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' &&
        buf[3] == 'G') {
        emit_result(path, "PNG image data");
        return 0;
    }
    if (is_ascii_buf(buf, n)) {
        emit_result(path, "ASCII text");
        return 0;
    }
    emit_result(path, "data");
    return 0;
}

int main(int argc, char **argv) {
    int i;
    int rc = 0;
    if (argc < 2) {
        usage();
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) continue;
        if (file_one(argv[i]) != 0) rc = 1;
    }
    return rc;
}
