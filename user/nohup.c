/* fakeos userland - nohup: run COMMAND immune to hangups (thin POSIX).
 * Forms: nohup COMMAND [ARG...]
 * Ignores SIGHUP. If stdout is a TTY, append to ./nohup.out (create 0644);
 * if stderr is a TTY, dup it onto stdout. If stdin is a TTY, redirect from
 * /dev/null. COMMAND without '/' resolves as /bin/COMMAND (PATH-lite).
 * Compact argv for EXEC_MAX_ARGC=8. No background fork — use shell &.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_dup2(long oldfd, long newfd);
extern long u_ioctl(long fd, unsigned long req, unsigned long arg);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern void *signal(int sig, void *handler);
extern char *strcpy(char *dst, const char *src);

enum {
    ARG_SLOT = 128,
    SIGHUP = 1,
    SIG_IGN_V = 1,
    TCGETS = 0x5401,
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT = 0x40,
    O_APPEND = 0x400,
    MODE_0644 = 0x1A4
};

static int fd_is_tty(long fd) {
    char termios[64];
    int i;
    for (i = 0; i < 64; i++) termios[i] = 0;
    return u_ioctl(fd, (unsigned long)TCGETS, (unsigned long)termios) == 0;
}

static int resolve_cmd(const char *cmd, char *pathbuf) {
    unsigned long n;
    if (cmd == 0 || cmd[0] == 0) return -1;
    for (n = 0; cmd[n]; n++) {
        if (cmd[n] == '/') {
            if (n + 1 >= ARG_SLOT) return -1;
            strcpy(pathbuf, cmd);
            return 0;
        }
    }
    if (5 + n + 1 >= ARG_SLOT) return -1;
    strcpy(pathbuf, "/bin/");
    strcpy(pathbuf + 5, cmd);
    return 0;
}

int main(int argc, char **argv) {
    char pathbuf[ARG_SLOT];
    char *envp[1];
    long fd;
    long nfd;

    if (argc < 2) {
        u_write(2, "usage: nohup COMMAND [ARG...]\n", 30);
        return 1;
    }
    if (resolve_cmd(argv[1], pathbuf) != 0) {
        u_write(2, "nohup: command too long\n", 24);
        return 1;
    }

    signal(SIGHUP, (void *)(long)SIG_IGN_V);

    if (fd_is_tty(0)) {
        nfd = u_open("/dev/null", O_RDONLY, 0);
        if (nfd >= 0) {
            u_dup2(nfd, 0);
            if (nfd != 0) u_close(nfd);
        }
    }

    if (fd_is_tty(1)) {
        u_write(2, "nohup: appending output to nohup.out\n", 37);
        fd = u_open("nohup.out", O_WRONLY | O_CREAT | O_APPEND, MODE_0644);
        if (fd < 0) {
            u_write(2, "nohup: cannot open nohup.out\n", 29);
            return 1;
        }
        u_dup2(fd, 1);
        if (fd != 1) u_close(fd);
    }

    if (fd_is_tty(2)) {
        u_dup2(1, 2);
    }

    envp[0] = 0;
    if (u_execve(pathbuf, argv + 1, envp) < 0) {
        u_write(2, "nohup: exec failed\n", 19);
        u_exit(127);
    }
    return 127;
}
