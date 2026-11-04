/* fakeos userland - tail: print the last N lines of files (default 10). */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern long strtol_dec(const char *s);
extern void printf(const char *fmt, ...);

enum { LINE_MAX = 512, RING_MAX = 256 };

static char ring[RING_MAX][LINE_MAX];
static int ring_len;
static int ring_next;
static int ring_cap;

static void ring_reset(int cap) {
    ring_len = 0;
    ring_next = 0;
    ring_cap = cap;
    if (ring_cap < 0) ring_cap = 0;
    if (ring_cap > RING_MAX) ring_cap = RING_MAX;
}

static void ring_push_line(const char *line, int n) {
    int i;
    if (ring_cap <= 0) return;
    if (n >= LINE_MAX) n = LINE_MAX - 1;
    for (i = 0; i < n; i++) ring[ring_next][i] = line[i];
    ring[ring_next][n] = 0;
    ring_next = (ring_next + 1) % ring_cap;
    if (ring_len < ring_cap) ring_len++;
}

static void ring_flush(void) {
    int i;
    int start = (ring_len < ring_cap) ? 0 : ring_next;
    for (i = 0; i < ring_len; i++) {
        int idx = (start + i) % ring_cap;
        int n = 0;
        while (ring[idx][n]) n++;
        if (n > 0) u_write(1, ring[idx], (unsigned long)n);
        u_write(1, "\n", 1);
    }
}

static int tail_fd(long fd, long limit) {
    char buf[512];
    char line[LINE_MAX];
    long n;
    int li = 0;
    int i;

    ring_reset((int)limit);
    if (limit <= 0) {
        /* Drain stdin/file but print nothing. */
        while ((n = u_read(fd, buf, sizeof(buf))) > 0) { }
        return n < 0 ? 1 : 0;
    }

    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[li] = 0;
                ring_push_line(line, li);
                li = 0;
                continue;
            }
            if (li < LINE_MAX - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        ring_push_line(line, li);
    }
    ring_flush();
    return n < 0 ? 1 : 0;
}

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    long limit = 10;
    int i = 1;
    int rc = 0;
    int files = 0;

    if (i < argc && strcmp(argv[i], "-n") == 0) {
        if (i + 1 >= argc || !is_digits(argv[i + 1])) {
            u_write(2, "usage: tail [-n N] [FILE...]\n", 29);
            return 1;
        }
        limit = strtol_dec(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && is_digits(argv[i] + 1)) {
        limit = strtol_dec(argv[i] + 1);
        i++;
    }

    if (i >= argc) {
        return tail_fd(0, limit);
    }
    for (; i < argc; i++) {
        long fd;
        if (strcmp(argv[i], "-") == 0) {
            if (files > 0 || i + 1 < argc) {
                if (files > 0) u_write(1, "\n", 1);
                printf("==> standard input <==\n");
            }
            if (tail_fd(0, limit) != 0) rc = 1;
            files++;
            continue;
        }
        fd = u_open(argv[i], 0, 0);
        if (fd < 0) {
            printf("tail: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (files > 0 || i + 1 < argc) {
            if (files > 0) u_write(1, "\n", 1);
            printf("==> %s <==\n", argv[i]);
        }
        if (tail_fd(fd, limit) != 0) rc = 1;
        u_close(fd);
        files++;
    }
    return rc;
}
