/* fakeos userland - minimal login/getty stub.
 * Accepts user "root" with empty password (or "root"), then execve /bin/sh.
 * /etc/passwd is consulted for a name match only (no crypt yet). */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);
extern void puts(const char *s);

static int read_line(char *buf, int cap) {
    long n = u_read(0, buf, (unsigned long)(cap - 1));
    int i;
    if (n <= 0) return 0;
    buf[n] = 0;
    for (i = 0; i < (int)n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            buf[i] = 0;
            break;
        }
    }
    return 1;
}

static int user_known(const char *name) {
    char buf[256];
    long fd;
    long n;
    long i;
    fd = u_open("/etc/passwd", 0, 0);
    if (fd < 0) {
        return strcmp(name, "root") == 0;
    }
    n = u_read(fd, buf, sizeof(buf) - 1);
    u_close(fd);
    if (n <= 0) return strcmp(name, "root") == 0;
    buf[n] = 0;
    i = 0;
    while (i < n) {
        char *line = buf + i;
        char *colon;
        while (i < n && buf[i] != '\n') i++;
        if (i < n) {
            buf[i] = 0;
            i++;
        }
        if (line[0] == 0 || line[0] == '#') continue;
        colon = line;
        while (*colon != 0 && *colon != ':') colon++;
        if (*colon == ':') {
            *colon = 0;
            if (strcmp(line, name) == 0) return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    char user[64];
    char pass[64];
    char *sh_argv[2];
    (void)argc;
    (void)argv;

    for (;;) {
        u_write(1, "fakeos login: ", 14);
        if (!read_line(user, sizeof(user))) {
            continue;
        }
        if (user[0] == 0) {
            continue;
        }
        u_write(1, "Password: ", 10);
        if (!read_line(pass, sizeof(pass))) {
            continue;
        }
        /* Minimal auth: root with empty or "root" password. */
        if (user_known(user) && (pass[0] == 0 || strcmp(pass, "root") == 0
                                 || strcmp(pass, user) == 0)) {
            printf("Welcome %s\n", user);
            sh_argv[0] = "/bin/sh";
            sh_argv[1] = 0;
            if (u_execve("/bin/sh", sh_argv, 0) != 0) {
                puts("login: exec /bin/sh failed");
                u_exit(1);
            }
        }
        puts("Login incorrect");
    }
    return 0;
}
