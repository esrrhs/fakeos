/* fakeos userland - tiny interactive shell.
 * Builtins: echo ($? expansion), cd, pwd, mkdir, rmdir, ln -s, readlink,
 * exit [code], help, jobs, fg, bg. Redirects (> >> <), pipelines (|),
 * && / || lists, and & jobs. */
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
extern long u_symlink(const char *target, const char *linkpath);
extern long u_readlink(const char *path, char *buf, unsigned long n);
extern long u_unlink(const char *path);
extern long u_getpid(void);
extern long u_getppid(void);
extern long u_uname(void *buf);
extern long u_nanosleep(const void *req, void *rem);
extern long u_clock_gettime(long clockid, void *tp);
extern long u_sync(void);
extern long u_kill(long pid, long sig);
extern long u_setpgid(long pid, long pgid);
extern long u_getpgid(long pid);
extern long u_tty_setfg(long pgid);
extern long u_tcsetpgrp(long fd, long pgid);
extern long u_tcgetpgrp(long fd);
extern long u_setsid(void);
extern long u_ioctl(long fd, unsigned long req, unsigned long arg);
extern void *signal(int sig, void *handler);

extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, unsigned long n);
extern long strtol_dec(const char *s);
extern char *strtok_ws(char **save);
extern char *strtok_sh(char **save);
extern void printf(const char *fmt, ...);
extern void puts(const char *s);

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200,
    O_APPEND = 0x400,
    SAVE_FD  = 8,
    HIST_MAX = 32,
    MAX_TOK  = 32,
    MAX_STAGE = 4,
    MAX_JOBS = 4,
    WNOHANG = 1,
    WUNTRACED = 2,
    WCONTINUED = 8,
    SIGCONT_N = 18,
    SIGTSTP_N = 20,
    SIGTTIN_N = 21,
    SIGTTOU_N = 22,
    SIG_IGN_V = 1
};

struct job {
    long pid;       /* leader / wait target */
    long pgid;      /* process group (pipeline unit) */
    int active;
    int stopped;
    char name[64];
};

static struct job jobs[MAX_JOBS];
static long shell_pgid;

static void shell_claim_tty(void) {
    /* Prefer ioctl TIOCSPGRP when available; fall back to tcsetpgrp. */
    long pg = shell_pgid;
    if (pg == 0) {
        pg = u_getpid();
    }
    {
        int pgid32 = (int)pg;
        if (u_ioctl(0, 0x5410UL, (unsigned long)&pgid32) != 0) {
            u_tcsetpgrp(0, pg);
        }
    }
}
static int job_seq;
static char hist[HIST_MAX][256];
static int hist_len;
static int hist_next;

static void hist_add(const char *line) {
    int i = 0;
    int n = 0;
    if (line == 0 || line[0] == 0) return;
    while (line[n] && n < 255) n++;
    /* Skip duplicate of most recent. */
    if (hist_len > 0) {
        int prev = hist_next == 0 ? HIST_MAX - 1 : hist_next - 1;
        int same = 1;
        for (i = 0; i <= n; i++) {
            if (hist[prev][i] != line[i]) { same = 0; break; }
        }
        if (same) return;
    }
    for (i = 0; i < n; i++) hist[hist_next][i] = line[i];
    hist[hist_next][n] = 0;
    hist_next = (hist_next + 1) % HIST_MAX;
    if (hist_len < HIST_MAX) hist_len++;
}

static void hist_print(void) {
    int i;
    int start = (hist_len < HIST_MAX) ? 0 : hist_next;
    for (i = 0; i < hist_len; i++) {
        int idx = (start + i) % HIST_MAX;
        printf(" %d  %s\n", i + 1, hist[idx]);
    }
}


static int eq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static int job_alloc(void) {
    int i;
    for (i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active) return i;
    }
    return -1;
}

static int job_find_pid(long pid) {
    int i;
    for (i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].pid == pid) return i;
    }
    return -1;
}

static int job_current(void) {
    int i;
    int best = -1;
    for (i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active) best = i;
    }
    return best;
}

static void job_set_name(int ji, char **stage) {
    int k;
    int p = 0;
    jobs[ji].name[0] = 0;
    for (k = 0; stage[k] != 0 && k < 3; k++) {
        int j = 0;
        if (k > 0 && p < 62) {
            jobs[ji].name[p++] = ' ';
            jobs[ji].name[p] = 0;
        }
        while (stage[k][j] != 0 && p < 62) {
            jobs[ji].name[p++] = stage[k][j++];
        }
        jobs[ji].name[p] = 0;
    }
}

static int status_stopped(long st) {
    return (st & 0xff) == 0x7f;
}

static int status_exit(long st) {
    return (int)((st >> 8) & 0xff);
}

/* Put child into a new process group (leader=pgid). Both parent and child
 * call this to close the fork race. */
static void job_set_pgrp(long child, long pgid) {
    if (child == 0) {
        u_setpgid(0, pgid == 0 ? 0 : pgid);
    } else {
        u_setpgid(child, pgid);
    }
}

static int job_note_stopped(long pid, const char *fallback_name) {
    int ji = job_find_pid(pid);
    int k;
    if (ji < 0) {
        ji = job_alloc();
        if (ji < 0) return -1;
        jobs[ji].pid = pid;
        jobs[ji].pgid = pid;
        jobs[ji].active = 1;
        jobs[ji].name[0] = 0;
        if (fallback_name) {
            for (k = 0; fallback_name[k] && k < 63; k++) {
                jobs[ji].name[k] = fallback_name[k];
            }
            jobs[ji].name[k] = 0;
        }
    }
    jobs[ji].stopped = 1;
    jobs[ji].active = 1;
    return ji;
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
           eq(cmd, "help") || eq(cmd, "pid") || eq(cmd, "ppid") ||
           eq(cmd, "uname") || eq(cmd, "sleep") || eq(cmd, "sync") ||
           eq(cmd, "clear") || eq(cmd, "history") || eq(cmd, "jobs") ||
           eq(cmd, "fg") || eq(cmd, "bg") || eq(cmd, "kill") ||
           eq(cmd, "ln") || eq(cmd, "readlink");
}

/* Default PATH directories (colon-separated). */
static const char *default_path = "/bin:/usr/bin:.";

/* Resolve argv[0] into path (may use buf). Tries PATH components. */
static const char *resolve_cmd(char **argv, char *buf) {
    int has_slash = 0;
    int k;
    const char *path = default_path;
    const char *p;
    for (k = 0; argv[0][k] != 0; k++) {
        if (argv[0][k] == '/') {
            has_slash = 1;
            break;
        }
    }
    if (has_slash) {
        return argv[0];
    }
    p = path;
    while (*p != 0) {
        int n = 0;
        long fd;
        while (p[n] != 0 && p[n] != ':') n++;
        if (n == 0) {
            /* empty component = "." */
            strcpy(buf, "./");
            strncpy(buf + 2, argv[0], 90);
            buf[95] = 0;
        } else if (n == 1 && p[0] == '.') {
            strcpy(buf, "./");
            strncpy(buf + 2, argv[0], 90);
            buf[95] = 0;
        } else {
            int i;
            for (i = 0; i < n && i < 80; i++) buf[i] = p[i];
            buf[n] = '/';
            buf[n + 1] = 0;
            strncpy(buf + n + 1, argv[0], 90);
            buf[95] = 0;
        }
        fd = u_open(buf, 0, 0);
        if (fd >= 0) {
            u_close(fd);
            return buf;
        }
        if (p[n] == ':') p += n + 1;
        else break;
    }
    /* Fall back to /bin/name even if missing (exec will fail). */
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
        const char *target = tokens[1] != 0 ? tokens[1] : "/home";
        if (u_chdir(target) < 0) {
            /* Fall back to / when Ext2 home is offline. */
            if (tokens[1] == 0 && u_chdir("/") == 0) {
                return 0;
            }
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
        puts("builtins: echo, cd, mkdir, rmdir, pwd, pid, ppid, uname, sleep, ln, readlink, sync, clear, history, jobs, fg, bg, kill, exit, help");
        puts("redirects: > >> <   pipelines: |   lists: && ||   jobs: &");
        puts("programs: hello cat ls cp rm touch mv ping nc ed login (PATH=/bin:.)");
        puts("signals: Ctrl-C/Z/TTIN/TTOU; setsid/fg pgid; ioctl TIOCSPGRP; pipelines share setpgid; fg/bg/jobs; kill [-]PGID; history");
        return 0;
    }
    if (eq(tokens[0], "history")) {
        hist_print();
        return 0;
    }
    if (eq(tokens[0], "jobs")) {
        int i;
        int cur = job_current();
        for (i = 0; i < MAX_JOBS; i++) {
            if (!jobs[i].active) continue;
            printf("[%d]%c %s %s\n", i + 1, (i == cur) ? '+' : '-',
                   jobs[i].stopped ? "Stopped" : "Running", jobs[i].name);
        }
        return 0;
    }
    if (eq(tokens[0], "fg")) {
        long status = 0;
        long reaped;
        int ji = job_current();
        long pid;
        if (tokens[1] != 0 && tokens[1][0] == '%') {
            long n = strtol_dec(tokens[1] + 1);
            if (n >= 1 && n <= MAX_JOBS && jobs[n - 1].active) ji = (int)(n - 1);
            else ji = -1;
        }
        if (ji < 0) {
            puts("sh: fg: no current job");
            return 1;
        }
        pid = jobs[ji].pid;
        if (jobs[ji].stopped) {
            long pg = jobs[ji].pgid != 0 ? jobs[ji].pgid : pid;
            u_kill(-pg, SIGCONT_N);
            jobs[ji].stopped = 0;
        }
        u_tcsetpgrp(0, jobs[ji].pgid != 0 ? jobs[ji].pgid : pid);
        reaped = u_wait4(pid, &status, WUNTRACED | WCONTINUED, 0);
        shell_claim_tty();
        if (reaped == pid && status_stopped(status)) {
            jobs[ji].stopped = 1;
            jobs[ji].active = 1;
            printf("[%d]+ Stopped %s\n", ji + 1, jobs[ji].name);
            return 148; /* 128+SIGTSTP */
        }
        jobs[ji].active = 0;
        jobs[ji].stopped = 0;
        if (reaped == pid) {
            return status_exit(status);
        }
        return 1;
    }
    if (eq(tokens[0], "bg")) {
        int ji = job_current();
        if (tokens[1] != 0 && tokens[1][0] == '%') {
            long n = strtol_dec(tokens[1] + 1);
            if (n >= 1 && n <= MAX_JOBS && jobs[n - 1].active) ji = (int)(n - 1);
            else ji = -1;
        }
        if (ji < 0) {
            puts("sh: bg: no current job");
            return 1;
        }
        if (jobs[ji].stopped) {
            long pg = jobs[ji].pgid != 0 ? jobs[ji].pgid : jobs[ji].pid;
            u_kill(-pg, SIGCONT_N);
            jobs[ji].stopped = 0;
        }
        printf("[%d]+ %s &\n", ji + 1, jobs[ji].name);
        return 0;
    }

    if (push_redirects(in_path, out_path, append, &saved_in, &saved_out) < 0) {
        return 1;
    }
    if (eq(tokens[0], "kill")) {
        long pid;
        long sig = 15; /* SIGTERM */
        int ai = 1;
        if (tokens[1] != 0 && tokens[1][0] == '-' && tokens[2] != 0) {
            sig = strtol_dec(tokens[1] + 1);
            ai = 2;
        }
        if (tokens[ai] == 0) {
            puts("sh: kill: usage: kill [-SIG] PID");
            return 1;
        }
        pid = strtol_dec(tokens[ai]);
        if (u_kill(pid, sig) != 0) {
            puts("sh: kill: failed");
            return 1;
        }
        return 0;
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
    } else if (eq(tokens[0], "ppid")) {
        printf("%d\n", (int)u_getppid());
        rc = 0;
    } else if (eq(tokens[0], "uname")) {
        char uts[390];
        long i;
        for (i = 0; i < 390; i++) uts[i] = 0;
        if (u_uname(uts) < 0) {
            puts("sh: uname failed");
            rc = 1;
        } else if (tokens[1] != 0 && eq(tokens[1], "-a")) {
            printf("%s %s %s %s %s\n", uts, uts + 65, uts + 130, uts + 195, uts + 260);
            rc = 0;
        } else {
            puts(uts);
            rc = 0;
        }
    } else if (eq(tokens[0], "sleep")) {
        long sec;
        long ts[2];
        if (tokens[1] == 0) {
            puts("sh: sleep: missing operand");
            rc = 1;
        } else {
            sec = strtol_dec(tokens[1]);
            if (sec < 0) sec = 0;
            ts[0] = sec;
            ts[1] = 0;
            if (u_nanosleep(ts, 0) < 0) {
                puts("sh: sleep interrupted");
                rc = 1;
            } else {
                rc = 0;
            }
        }
    } else if (eq(tokens[0], "ln")) {
        /* ln -s TARGET LINKPATH */
        if (tokens[1] == 0 || !eq(tokens[1], "-s") || tokens[2] == 0
            || tokens[3] == 0) {
            puts("sh: ln: usage: ln -s TARGET LINKPATH");
            rc = 1;
        } else if (u_symlink(tokens[2], tokens[3]) != 0) {
            puts("sh: ln: symlink failed");
            rc = 1;
        } else {
            rc = 0;
        }
    } else if (eq(tokens[0], "readlink")) {
        char buf[128];
        long n;
        if (tokens[1] == 0) {
            puts("sh: readlink: missing operand");
            rc = 1;
        } else {
            n = u_readlink(tokens[1], buf, sizeof(buf) - 1);
            if (n < 0) {
                puts("sh: readlink: failed");
                rc = 1;
            } else {
                buf[n] = 0;
                puts(buf);
                rc = 0;
            }
        }
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
                job_set_pgrp(0, 0); /* new pgrp = self */
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
            job_set_pgrp(child, child);
            u_tcsetpgrp(0, child);
            reaped = u_wait4(child, &status, WUNTRACED | WCONTINUED, 0);
            shell_claim_tty();
            if (reaped == child && status_stopped(status)) {
                int ji = job_note_stopped(child, stages[0][0]);
                if (ji >= 0) {
                    printf("[%d]+ Stopped %s\n", ji + 1, jobs[ji].name);
                }
                return 148;
            }
            if (reaped == child) {
                return status_exit(status);
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

    {
        long pipe_pgid = 0;
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
                job_set_pgrp(0, pipe_pgid == 0 ? 0 : pipe_pgid);
                child_run_stage(stages[si], in_fd, out_fd, rc_in);
            }
            if (pipe_pgid == 0) {
                pipe_pgid = child;
            }
            job_set_pgrp(child, pipe_pgid);
            pids[si] = child;
        }

        for (si = 0; si < npipes; si++) {
            u_close(pipes[si][0]);
            u_close(pipes[si][1]);
        }

        if (pipe_pgid != 0) {
            u_tcsetpgrp(0, pipe_pgid);
        }
        for (si = 0; si < nstage; si++) {
            long reaped = u_wait4(pids[si], &status, WUNTRACED | WCONTINUED, 0);
            if (reaped == pids[si] && status_stopped(status)) {
                int ji = job_note_stopped(pipe_pgid, stages[0][0]);
                if (ji >= 0) {
                    jobs[ji].pgid = pipe_pgid;
                    jobs[ji].pid = pipe_pgid;
                    printf("[%d]+ Stopped %s\n", ji + 1, jobs[ji].name);
                }
                rc = 148;
            } else if (reaped == pids[si]) {
                rc = status_exit(status);
            } else {
                rc = -1;
            }
        }
    }
    shell_claim_tty();
    return rc;
}

/* Run tokens as && / || list of pipelines; optional trailing &. */
static long run_list(char **tokens, long rc_in) {
    int i = 0;
    long rc = rc_in;
    int background = 0;
    int end;

    /* Detect trailing & for job control (single trailing amp). */
    end = 0;
    while (tokens[end] != 0) end++;
    if (end > 0 && eq(tokens[end - 1], "&")) {
        background = 1;
        tokens[end - 1] = 0;
        end--;
        if (end == 0) {
            puts("sh: syntax error near &");
            return 1;
        }
    }

    while (tokens[i] != 0) {
        char *stage[MAX_TOK];
        int ns = 0;
        int op = 0; /* 0 none, 1 &&, 2 || */
        long child;
        long status = 0;

        while (tokens[i] != 0 && !eq(tokens[i], "&&") && !eq(tokens[i], "||")) {
            if (ns < MAX_TOK - 1) {
                stage[ns++] = tokens[i];
            }
            i++;
        }
        stage[ns] = 0;
        if (tokens[i] != 0 && eq(tokens[i], "&&")) {
            op = 1;
            i++;
        } else if (tokens[i] != 0 && eq(tokens[i], "||")) {
            op = 2;
            i++;
        }
        if (ns == 0) {
            puts("sh: empty command in list");
            return 1;
        }

        if (background && op == 0 && tokens[i] == 0) {
            int ji = job_alloc();
            if (ji < 0) {
                puts("sh: too many background jobs");
                return 1;
            }
            child = u_fork();
            if (child < 0) {
                puts("sh: fork failed");
                return 1;
            }
            if (child == 0) {
                long st = run_pipeline(stage, rc);
                u_exit(st);
            }
            jobs[ji].pid = child;
            jobs[ji].pgid = child;
            jobs[ji].active = 1;
            jobs[ji].stopped = 0;
            job_set_name(ji, stage);
            job_seq++;
            printf("[%d] %d\n", ji + 1, (int)child);
            return 0;
        }

        rc = run_pipeline(stage, rc);
        if (op == 1 && rc != 0) {
            /* skip until next || or end — simple: stop list */
            return rc;
        }
        if (op == 2 && rc == 0) {
            return rc;
        }
        (void)status;
    }
    return rc;
}

static void reap_background(void) {
    int i;
    for (i = 0; i < MAX_JOBS; i++) {
        long status = 0;
        long reaped;
        if (!jobs[i].active) continue;
        reaped = u_wait4(jobs[i].pid, &status, WNOHANG | WUNTRACED, 0);
        if (reaped == jobs[i].pid && status_stopped(status)) {
            jobs[i].stopped = 1;
            printf("[%d]+ Stopped %s\n", i + 1, jobs[i].name);
        } else if (reaped == jobs[i].pid) {
            printf("[%d]+ Done %s\n", i + 1, jobs[i].name);
            jobs[i].active = 0;
            jobs[i].stopped = 0;
        }
    }
}

int main(int argc, char **argv) {
    long rc = 0;
    char line[256];

    (void)argc;
    (void)argv;

    /* Job-control shells ignore TTIN/TTOU so tcsetpgrp can reclaim the TTY
     * after a foreground job without stopping the shell (POSIX). */
    signal(SIGTTIN_N, (void *)SIG_IGN_V);
    signal(SIGTTOU_N, (void *)SIG_IGN_V);

    /* Session leader + own process group as foreground (not pgid 0). */
    {
        long sid = u_setsid();
        shell_pgid = u_getpid();
        if (sid < 0) {
            /* Already a leader/member: still own a pgrp for job control. */
            u_setpgid(0, shell_pgid);
        }
        shell_claim_tty();
    }

    puts("fakeos shell - session/fg, pipes/redirects, &&/||, jobs/fg/bg/Ctrl-Z/ioctl/tcsetpgrp, history/!!/readline, PATH/quotes, kill; persist under /home");

    for (;;) {
        char *tokens[MAX_TOK];
        char *save;
        char *t;
        int nt = 0;
        long n;

        reap_background();

        {
            char cwd[64];
            if (u_getcwd(cwd, sizeof(cwd)) != -1
                && (eq(cwd, "/home") || eq(cwd, "/mnt/home"))) {
                u_write(1, "fakeos:~$ ", 10);
            } else {
                u_write(1, "fakeos:$ ", 9);
            }
        }

        shell_claim_tty();
        n = u_read(0, line, sizeof(line) - 1);
        if (n == -4) {
            /* Ctrl-C at prompt: new line already echoed. */
            continue;
        }
        if (n <= 0) {
            continue;
        }
        line[n] = 0;
        /* Strip trailing newline already handled by read; expand !!. */
        if (line[0] == '!' && line[1] == '!' && (line[2] == 0 || line[2] == ' '
            || line[2] == '\t' || line[2] == '\n' || line[2] == '\r')) {
            if (hist_len == 0) {
                puts("sh: !!: event not found");
                continue;
            }
            {
                int prev = hist_next == 0 ? HIST_MAX - 1 : hist_next - 1;
                int i = 0;
                while (hist[prev][i] && i < 255) {
                    line[i] = hist[prev][i];
                    i++;
                }
                line[i] = 0;
                puts(line);
            }
        }

        hist_add(line);

        save = line;
        while (nt < MAX_TOK - 1 && (t = strtok_sh(&save)) != 0) {
            tokens[nt++] = t;
        }
        tokens[nt] = 0;
        if (nt == 0) {
            continue;
        }

        rc = run_list(tokens, rc);
    }
}
