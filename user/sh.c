/* fakeos userland - tiny interactive shell.
 * Builtins: echo ($? expansion), cd, pwd, mkdir, rmdir, exit [code], help.
 * Redirects (> >> <) and pipelines (|) via pipe/dup2/fork. */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_dup2(long oldfd, long newfd);
extern long u_pipe(int *pipefd);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern long u_chdir(const char *path);
extern long u_mkdir(const char *path);
extern long u_rmdir(const char *path);
extern long u_getcwd(char *buf, unsigned long len);
extern long u_getpid(void);
extern long u_sync(void);

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
    SAVE_FD  = 8,
    MAX_TOK  = 24,
    MAX_STAGE = 4
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

static int is_builtin(const char *cmd) {
    return eq(cmd, "echo") || eq(cmd, "cd") || eq(cmd, "pwd") ||
           eq(cmd, "mkdir") || eq(cmd, "rmdir") || eq(cmd, "exit") ||
           eq(cmd, "help") || eq(cmd, "pid") || eq(cmd, "sync") ||
           eq(cmd, "clear");
}

/* Resolve argv[0] into path (may use buf). */
static const char *resolve_cmd(char **argv, char *buf) {
    int has_slash = 0;
    int k;
    for (k = 0; argv[0][k] != 0; k++) {
        if (argv[0][k] == '/') {
            has_slash = 1;
            break;
        }
    }
    if (has_slash) {
        return argv[0];
    }
    strcpy(buf, "/bin/");
    strncpy(buf + 5, argv[0], 90);
    buf[95] = 0;
    return buf;
}

/* Run a builtin in the current process. Returns exit status. */
static long run_builtin(char **tokens, long rc_in, char *in_path, char *out_path,
                        int append) {
    int saved_in;
    int saved_out;
    int i;
    long rc = rc_in;

    if (eq(tokens[0], "exit")) {
        long code = tokens[1] != 0 ? strtol_dec(tokens[1]) : rc_in;
        u_exit(code);
    }
    if (eq(tokens[0], "cd")) {
        const char *target = tokens[1] != 0 ? tokens[1] : "/";
        if (u_chdir(target) < 0) {
            printf("sh: cd: %s: No such file or directory\n", target);
            return 1;
        }
        return 0;
    }
    if (eq(tokens[0], "mkdir")) {
        if (tokens[1] == 0) {
            puts("sh: mkdir: missing operand");
            return 1;
        }
        if (u_mkdir(tokens[1]) < 0) {
            printf("sh: mkdir: %s: cannot create directory\n", tokens[1]);
            return 1;
        }
        return 0;
    }
    if (eq(tokens[0], "rmdir")) {
        if (tokens[1] == 0) {
            puts("sh: rmdir: missing operand");
            return 1;
        }
        if (u_rmdir(tokens[1]) < 0) {
            printf("sh: rmdir: %s: cannot remove directory\n", tokens[1]);
            return 1;
        }
        return 0;
    }
    if (eq(tokens[0], "sync")) {
        u_sync();
        return 0;
    }
    if (eq(tokens[0], "clear")) {
        /* ANSI clear for serial; VGA users still see history (no userspace VGA). */
        u_write(1, "\033[2J\033[H", 11);
        return 0;
    }
    if (eq(tokens[0], "help")) {
        puts("builtins: echo, cd, mkdir, rmdir, pwd, pid, sync, clear, exit, help");
        puts("redirects: > >> <   pipelines: |");
        puts("programs: hello cat ls cp rm touch mv ping (auto /bin)");
        return 0;
    }

    if (push_redirects(in_path, out_path, append, &saved_in, &saved_out) < 0) {
        return 1;
    }
    if (eq(tokens[0], "pwd")) {
        char buf[64];
        if (u_getcwd(buf, sizeof(buf)) == -1) {
            puts("sh: pwd failed");
            rc = 1;
        } else {
            puts(buf);
            rc = 0;
        }
    } else if (eq(tokens[0], "echo")) {
        for (i = 1; tokens[i] != 0; i++) {
            if (i > 1) {
                u_write(1, " ", 1);
            }
            if (eq(tokens[i], "$?")) {
                printf("%d", (int)rc_in);
            } else {
                u_write(1, tokens[i], strlen(tokens[i]));
            }
        }
        u_write(1, "\n", 1);
        rc = 0;
    } else if (eq(tokens[0], "pid")) {
        printf("%d\n", (int)u_getpid());
        rc = 0;
    }
    pop_redirects(saved_in, saved_out);
    return rc;
}

/* Child path: apply pipe ends + redirects, then exec or run builtin+exit. */
static void child_run_stage(char **argv, int stdin_fd, int stdout_fd,
                            long rc_in) {
    char *in_path;
    char *out_path;
    int append;
    char path[96];
    const char *target;
    int saved_in;
    int saved_out;

    extract_redirects(argv, &in_path, &out_path, &append);
    if (argv[0] == 0) {
        u_exit(1);
    }

    if (stdin_fd >= 0) {
        if (u_dup2(stdin_fd, 0) < 0) {
            u_exit(1);
        }
    }
    if (stdout_fd >= 0) {
        if (u_dup2(stdout_fd, 1) < 0) {
            u_exit(1);
        }
    }
    /* Close inherited pipe fds above stderr so they do not leak into exec. */
    {
        int fd;
        for (fd = 3; fd < 16; fd++) {
            u_close(fd);
        }
    }

    if (is_builtin(argv[0])) {
        /* cd/exit in a pipeline child only affect the child. */
        long st = run_builtin(argv, rc_in, in_path, out_path, append);
        u_exit(st);
    }

    if (push_redirects(in_path, out_path, append, &saved_in, &saved_out) < 0) {
        u_exit(1);
    }
    target = resolve_cmd(argv, path);
    if (u_execve(target, argv, 0) < 0) {
        printf("sh: %s: command not found\n", argv[0]);
        u_exit(127);
    }
    u_exit(127);
}

/* tokens contain '|' separators. Run as a pipeline. */
static long run_pipeline(char **tokens, long rc_in) {
    char *stages[MAX_STAGE][MAX_TOK];
    int nstage = 0;
    int i = 0;
    int si;
    int pipes[MAX_STAGE - 1][2];
    long pids[MAX_STAGE];
    long status = 0;
    long rc = 0;
    int npipes = 0;

    /* Split on '|'. */
    while (tokens[i] != 0 && nstage < MAX_STAGE) {
        int nt = 0;
        while (tokens[i] != 0 && !eq(tokens[i], "|")) {
            if (nt < MAX_TOK - 1) {
                stages[nstage][nt++] = tokens[i];
            }
            i++;
        }
        stages[nstage][nt] = 0;
        if (nt == 0) {
            puts("sh: empty pipeline stage");
            return 1;
        }
        nstage++;
        if (tokens[i] != 0 && eq(tokens[i], "|")) {
            i++;
            if (tokens[i] == 0) {
                puts("sh: trailing pipe");
                return 1;
            }
        }
    }
    if (tokens[i] != 0) {
        puts("sh: too many pipeline stages");
        return 1;
    }
    if (nstage == 0) {
        return 0;
    }

    /* Single stage, no pipe: keep builtins in the shell process. */
    if (nstage == 1) {
        char *in_path;
        char *out_path;
        int append;
        extract_redirects(stages[0], &in_path, &out_path, &append);
        if (stages[0][0] == 0) {
            return 0;
        }
        if (is_builtin(stages[0][0])) {
            return run_builtin(stages[0], rc_in, in_path, out_path, append);
        }
        {
            long child;
            long reaped;
            char path[96];
            const char *target = resolve_cmd(stages[0], path);
            int saved_in;
            int saved_out;
            child = u_fork();
            if (child == 0) {
                if (push_redirects(in_path, out_path, append, &saved_in,
                                   &saved_out) < 0) {
                    u_exit(1);
                }
                if (u_execve(target, stages[0], 0) < 0) {
                    printf("sh: %s: command not found\n", stages[0][0]);
                    u_exit(127);
                }
                u_exit(127);
            }
            reaped = u_wait4(child, &status, 0, 0);
            if (reaped == child) {
                return status >> 8;
            }
            return -1;
        }
    }

    for (si = 0; si < nstage - 1; si++) {
        if (u_pipe(pipes[si]) < 0) {
            puts("sh: pipe failed");
            while (npipes > 0) {
                npipes--;
                u_close(pipes[npipes][0]);
                u_close(pipes[npipes][1]);
            }
            return 1;
        }
        npipes++;
    }

    for (si = 0; si < nstage; si++) {
        int in_fd = -1;
        int out_fd = -1;
        long child;
        if (si > 0) {
            in_fd = pipes[si - 1][0];
        }
        if (si < nstage - 1) {
            out_fd = pipes[si][1];
        }
        child = u_fork();
        if (child < 0) {
            puts("sh: fork failed");
            rc = 1;
            break;
        }
        if (child == 0) {
            child_run_stage(stages[si], in_fd, out_fd, rc_in);
        }
        pids[si] = child;
    }

    /* Parent closes all pipe ends so writers can see EOF. */
    for (si = 0; si < npipes; si++) {
        u_close(pipes[si][0]);
        u_close(pipes[si][1]);
    }

    /* Reap every stage; overall status is the last one's. */
    for (si = 0; si < nstage; si++) {
        long reaped = u_wait4(pids[si], &status, 0, 0);
        if (reaped == pids[si]) {
            rc = status >> 8;
        } else {
            rc = -1;
        }
    }
    return rc;
}

int main(int argc, char **argv) {
    long rc = 0;
    char line[256];

    (void)argc;
    (void)argv;

    puts("fakeos shell - help | sync | clear | pipes/redirects; persist under /mnt/home");

    for (;;) {
        char *tokens[MAX_TOK];
        char *save;
        char *t;
        int nt = 0;
        long n;

        u_write(1, "fakeos:~$ ", 10);

        n = u_read(0, line, sizeof(line) - 1);
        if (n <= 0) {
            continue;
        }
        line[n] = 0;

        save = line;
        while (nt < MAX_TOK - 1 && (t = strtok_ws(&save)) != 0) {
            tokens[nt++] = t;
        }
        tokens[nt] = 0;
        if (nt == 0) {
            continue;
        }

        rc = run_pipeline(tokens, rc);
    }
}
