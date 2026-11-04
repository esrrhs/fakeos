/* fakeos userland - which: locate command in PATH (thin; shell-mirror).
 * Forms: which [-a] CMD...
 * Searches PATH from /proc/self/environ (default /bin:/usr/bin:.). Prints
 * the first matching path per CMD; -a prints every match along PATH.
 * Does not know shell builtins (use the shell builtin which for that).
 * Exit 0 iff every CMD resolves at least once. External form for
 * xargs/scripts that exec which without the shell builtin.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum { PATH_CAP = 192, BUF_CAP = 96, ENV_CAP = 512 };

static char path_buf[PATH_CAP];
static int path_ready;

static void set_default_path(void) {
    const char *d = "/bin:/usr/bin:.";
    int i;
    for (i = 0; d[i] && i < PATH_CAP - 1; i++) path_buf[i] = d[i];
    path_buf[i] = 0;
    path_ready = 1;
}

static void load_path(void) {
    char env[ENV_CAP];
    long fd;
    long n;
    long i;
    long start;
    if (path_ready) return;
    fd = u_open("/proc/self/environ", 0, 0);
    if (fd < 0) {
        set_default_path();
        return;
    }
    n = u_read(fd, env, sizeof(env) - 1);
    u_close(fd);
    if (n < 0) n = 0;
    env[n] = 0;
    start = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || env[i] == 0) {
            if (i - start >= 5 &&
                env[start] == 'P' && env[start + 1] == 'A' &&
                env[start + 2] == 'T' && env[start + 3] == 'H' &&
                env[start + 4] == '=') {
                long j;
                long k = 0;
                for (j = start + 5; j < i && k < PATH_CAP - 1; j++) {
                    path_buf[k++] = env[j];
                }
                path_buf[k] = 0;
                path_ready = 1;
                return;
            }
            start = i + 1;
        }
    }
    set_default_path();
}

static int has_slash(const char *s) {
    int i;
    for (i = 0; s[i]; i++) {
        if (s[i] == '/') return 1;
    }
    return 0;
}

static int path_exists(const char *p) {
    long fd = u_open(p, 0, 0);
    if (fd < 0) return 0;
    u_close(fd);
    return 1;
}

/* Fill out with candidate path for PATH component [p, p+n). */
static void build_candidate(const char *p, int n, const char *cmd, char *out) {
    int i;
    if (n == 0 || (n == 1 && p[0] == '.')) {
        out[0] = '.';
        out[1] = '/';
        for (i = 0; cmd[i] && i < BUF_CAP - 3; i++) out[2 + i] = cmd[i];
        out[2 + i] = 0;
    } else {
        for (i = 0; i < n && i < 80; i++) out[i] = p[i];
        out[n] = '/';
        out[n + 1] = 0;
        for (i = 0; cmd[i] && n + 1 + i < BUF_CAP - 1; i++) {
            out[n + 1 + i] = cmd[i];
        }
        out[n + 1 + i] = 0;
    }
}

/* Print matches for cmd. Returns number printed (0 = not found). */
static int lookup_print(const char *cmd, int all) {
    const char *p;
    int n;
    int i;
    int count = 0;
    char out[BUF_CAP];

    if (cmd == 0 || cmd[0] == 0) return 0;
    if (has_slash(cmd)) {
        if (!path_exists(cmd)) return 0;
        for (i = 0; cmd[i] && i < BUF_CAP - 1; i++) out[i] = cmd[i];
        out[i] = 0;
        printf("%s\n", out);
        return 1;
    }
    load_path();
    p = path_buf;
    while (*p != 0) {
        n = 0;
        while (p[n] != 0 && p[n] != ':') n++;
        build_candidate(p, n, cmd, out);
        if (path_exists(out)) {
            printf("%s\n", out);
            count++;
            if (!all) return count;
        }
        if (p[n] == ':') p += n + 1;
        else break;
    }
    return count;
}

static void usage(void) {
    u_write(2, "usage: which [-a] CMD...\n", 25);
}

int main(int argc, char **argv) {
    int i = 1;
    int rc = 0;
    int all = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        int j;
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        for (j = 1; argv[i][j]; j++) {
            if (argv[i][j] == 'a') all = 1;
            else {
                usage();
                return 1;
            }
        }
        i++;
    }

    if (i >= argc) {
        usage();
        return 1;
    }
    for (; i < argc; i++) {
        if (lookup_print(argv[i], all) == 0) {
            printf("%s: not found\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
