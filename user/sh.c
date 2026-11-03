/* fakeos userland - tiny interactive shell.
 * Builtins: echo ($? expansion), cd, pwd, mkdir, rmdir, exit [code], help.
 * Simple redirects (> >> <) apply to builtins and external commands via dup2.
 * Other commands fork/exec from /bin and are reaped via wait4. */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_dup2(long oldfd, long newfd);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern long u_chdir(const char *path);
extern long u_mkdir(const char *path);
extern long u_rmdir(const char *path);
extern long u_getcwd(char *buf, unsigned long len);
extern long u_getpid(void);

extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, unsigned long n);
extern long strtol_dec(const char *s);
extern char *strtok_ws(char **save);
extern void printf(const char *fmt, ...);
extern void puts(const char *s);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200,
    O_APPEND = 0x400,
    SAVE_FD  = 8
};

static int eq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

/* Pull < / > / >> out of argv. Returns 0 on success. */
static int extract_redirects(char **argv, char **in_path, char **out_path,
                             int *append) {
    int i = 0;
    int o = 0;
    *in_path = 0;
    *out_path = 0;
    *append = 0;
    while (argv[i] != 0) {
        if ((eq(argv[i], ">") || eq(argv[i], ">>") || eq(argv[i], "<"))
            && argv[i + 1] != 0) {
            if (eq(argv[i], "<")) {
                *in_path = argv[i + 1];
            } else {
                *out_path = argv[i + 1];
                *append = eq(argv[i], ">>") ? 1 : 0;
            }
            i += 2;
            continue;
        }
        argv[o++] = argv[i++];
    }
    argv[o] = 0;
    return 0;
}

static int push_redirects(const char *in_path, const char *out_path, int append,
                          int *saved_in, int *saved_out) {
    *saved_in = -1;
    *saved_out = -1;
    if (out_path != 0) {
        long flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        long fd = u_open(out_path, flags, 0x1A4);
        if (fd < 0) {
            printf("sh: cannot open %s for output\n", out_path);
            return -1;
        }
        if (u_dup2(1, SAVE_FD) < 0) {
            u_close(fd);
            return -1;
        }
        *saved_out = SAVE_FD;
        if (u_dup2(fd, 1) < 0) {
            u_dup2(SAVE_FD, 1);
            u_close(SAVE_FD);
            u_close(fd);
            *saved_out = -1;
            return -1;
        }
        u_close(fd);
    }
    if (in_path != 0) {
        long fd = u_open(in_path, O_RDONLY, 0);
        if (fd < 0) {
            printf("sh: cannot open %s for input\n", in_path);
            if (*saved_out >= 0) {
                u_dup2(*saved_out, 1);
                u_close(*saved_out);
                *saved_out = -1;
            }
            return -1;
        }
        if (u_dup2(0, SAVE_FD + 1) < 0) {
            u_close(fd);
            if (*saved_out >= 0) {
                u_dup2(*saved_out, 1);
                u_close(*saved_out);
                *saved_out = -1;
            }
            return -1;
        }
        *saved_in = SAVE_FD + 1;
        if (u_dup2(fd, 0) < 0) {
            u_dup2(*saved_in, 0);
            u_close(*saved_in);
            *saved_in = -1;
            u_close(fd);
            if (*saved_out >= 0) {
                u_dup2(*saved_out, 1);
                u_close(*saved_out);
                *saved_out = -1;
            }
            return -1;
        }
        u_close(fd);
    }
    return 0;
}

static void pop_redirects(int saved_in, int saved_out) {
    if (saved_out >= 0) {
        u_dup2(saved_out, 1);
        u_close(saved_out);
    }
    if (saved_in >= 0) {
        u_dup2(saved_in, 0);
        u_close(saved_in);
    }
}

int main(int argc, char **argv) {
    long rc = 0;
    char line[256];

    (void)argc;
    (void)argv;

    puts("fakeos shell - builtins: help, echo, cd, pwd, mkdir, rmdir, exit; redirects: > >> <");

    for (;;) {
        char *tokens[9];
        char *save;
        char *t;
        char *in_path;
        char *out_path;
        int append;
        int saved_in;
        int saved_out;
        int nt = 0;
        long n;
        int i;

        u_write(1, "fakeos:~$ ", 10);

        n = u_read(0, line, sizeof(line) - 1);
        if (n <= 0) {
            continue;
        }
        line[n] = 0;

        save = line;
        while (nt < 8 && (t = strtok_ws(&save)) != 0) {
            tokens[nt++] = t;
        }
        tokens[nt] = 0;
        if (nt == 0) {
            continue;
        }

        extract_redirects(tokens, &in_path, &out_path, &append);
        if (tokens[0] == 0) {
            continue;
        }

        if (eq(tokens[0], "exit")) {
            long code = tokens[1] != 0 ? strtol_dec(tokens[1]) : rc;
            u_exit(code);
        }
        if (eq(tokens[0], "cd")) {
            const char *target = tokens[1] != 0 ? tokens[1] : "/";
            if (u_chdir(target) < 0) {
                printf("sh: cd: %s: No such file or directory\n", target);
                rc = 1;
            } else {
                rc = 0;
            }
            continue;
        }
        if (eq(tokens[0], "mkdir")) {
            if (tokens[1] == 0) {
                puts("sh: mkdir: missing operand");
                rc = 1;
            } else if (u_mkdir(tokens[1]) < 0) {
                printf("sh: mkdir: %s: cannot create directory\n", tokens[1]);
                rc = 1;
            } else {
                rc = 0;
            }
            continue;
        }
        if (eq(tokens[0], "rmdir")) {
            if (tokens[1] == 0) {
                puts("sh: rmdir: missing operand");
                rc = 1;
            } else if (u_rmdir(tokens[1]) < 0) {
                printf("sh: rmdir: %s: cannot remove directory\n", tokens[1]);
                rc = 1;
            } else {
                rc = 0;
            }
            continue;
        }
        if (eq(tokens[0], "pwd")) {
            char buf[64];
            if (push_redirects(in_path, out_path, append, &saved_in, &saved_out) < 0) {
                rc = 1;
                continue;
            }
            if (u_getcwd(buf, sizeof(buf)) == -1) {
                puts("sh: pwd failed");
                rc = 1;
            } else {
                puts(buf);
                rc = 0;
            }
            pop_redirects(saved_in, saved_out);
            continue;
        }
        if (eq(tokens[0], "echo")) {
            if (push_redirects(in_path, out_path, append, &saved_in, &saved_out) < 0) {
                rc = 1;
                continue;
            }
            for (i = 1; tokens[i] != 0; i++) {
                if (i > 1) {
                    u_write(1, " ", 1);
                }
                if (eq(tokens[i], "$?")) {
                    printf("%d", (int)rc);
                } else {
                    u_write(1, tokens[i], strlen(tokens[i]));
                }
            }
            u_write(1, "\n", 1);
            pop_redirects(saved_in, saved_out);
            rc = 0;
            continue;
        }
        if (eq(tokens[0], "pid")) {
            if (push_redirects(in_path, out_path, append, &saved_in, &saved_out) < 0) {
                rc = 1;
                continue;
            }
            printf("%d\n", (int)u_getpid());
            pop_redirects(saved_in, saved_out);
            rc = 0;
            continue;
        }
        if (eq(tokens[0], "help")) {
            puts("builtins: echo [args|$?], cd, mkdir, rmdir, pwd, pid, exit, help");
            puts("redirects: cmd > file | cmd >> file | cmd < file");
            puts("programs: /bin/hello /bin/cat /bin/ls (PATH prefix auto)");
            rc = 0;
            continue;
        }

        {
            char path[96];
            long child;
            long status = 0;
            long reaped;
            const char *target;
            int has_slash = 0;
            int k;

            for (k = 0; tokens[0][k] != 0; k++) {
                if (tokens[0][k] == '/') {
                    has_slash = 1;
                    break;
                }
            }
            if (has_slash) {
                target = tokens[0];
            } else {
                strcpy(path, "/bin/");
                strncpy(path + 5, tokens[0], 90);
                path[95] = 0;
                target = path;
            }

            child = u_fork();
            if (child == 0) {
                if (push_redirects(in_path, out_path, append, &saved_in, &saved_out) < 0) {
                    u_exit(1);
                }
                if (u_execve(target, tokens, 0) < 0) {
                    printf("sh: %s: command not found\n", tokens[0]);
                    u_exit(127);
                }
                u_exit(127);
            }
            reaped = u_wait4(child, &status, 0, 0);
            if (reaped == child) {
                rc = status >> 8;
            } else {
                rc = -1;
            }
        }
    }
}
