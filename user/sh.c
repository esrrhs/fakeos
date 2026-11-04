/* fakeos userland - tiny interactive shell.
 * Builtins: echo [-neE] ($?/$VAR), cd, pwd[-LP], mkdir[-p], rmdir[-p], ln[-s], readlink,
 * realpath, env/export/unset/which, exit [code], help, jobs, fg, bg.
 * Prompt shows cwd (~ for home). Shell env table drives PATH lookup and
 * is passed through execve envp (live /proc/self/environ). Redirects,
 * pipelines, &&/||, & jobs. */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_chmod(const char *path, long mode);
extern long u_umask(long mask);
extern long u_chown(const char *path, long uid, long gid);
extern long u_getuid(void);
extern long u_geteuid(void);
extern long u_getgid(void);
extern long u_getegid(void);
extern long u_getgroups(long size, unsigned int *list);
extern long u_setuid(long uid);
extern long u_setgid(long gid);
extern long u_setreuid(long ruid, long euid);
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
extern long u_stat(const char *path, void *statbuf);
extern long u_getcwd(char *buf, unsigned long len);
extern long u_symlink(const char *target, const char *linkpath);
extern long u_link(const char *oldpath, const char *newpath);
extern long u_readlink(const char *path, char *buf, unsigned long n);
extern long u_unlink(const char *path);
extern long u_getpid(void);
extern long u_getppid(void);
extern long u_uname(void *buf);
extern long u_nanosleep(const void *req, void *rem);
extern long u_clock_gettime(long clockid, void *tp);
extern long u_sync(void);
extern long u_fsync(long fd);
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
    MAX_ENV  = 16,
    ENV_NAME = 32,
    ENV_VAL  = 96,
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

static const char *hist_path(void) {
    /* Persist under Ext2 home so history survives reboot. */
    return "/home/.sh_history";
}

static void hist_save(void) {
    long fd;
    int i;
    int start;
    const char *path = hist_path();
    fd = u_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0x1A4);
    if (fd < 0) return;
    start = (hist_len < HIST_MAX) ? 0 : hist_next;
    for (i = 0; i < hist_len; i++) {
        int idx = (start + i) % HIST_MAX;
        int n = 0;
        while (hist[idx][n] && n < 255) n++;
        if (n > 0) {
            u_write(fd, hist[idx], (unsigned long)n);
            u_write(fd, "\n", 1);
        }
    }
    u_fsync(fd);
    u_close(fd);
}

static void hist_add_internal(const char *line, int do_save) {
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
    if (do_save) hist_save();
}

static void hist_add(const char *line) {
    hist_add_internal(line, 1);
}

static void hist_load(void) {
    char buf[4096];
    char line[256];
    long fd;
    long n;
    long off = 0;
    int li = 0;
    const char *path = hist_path();
    fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) return;
    while ((n = u_read(fd, buf + off, (unsigned long)(sizeof(buf) - (unsigned long)off))) > 0) {
        long end = off + n;
        long i = 0;
        while (i < end) {
            char c = buf[i++];
            if (c == '\n' || c == '\r') {
                if (li > 0) {
                    line[li] = 0;
                    hist_add_internal(line, 0);
                    li = 0;
                }
                continue;
            }
            if (li < 255) line[li++] = c;
        }
        off = 0;
    }
    if (li > 0) {
        line[li] = 0;
        hist_add_internal(line, 0);
    }
    u_close(fd);
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

enum {
    SH_ST_MODE = 24,
    SH_ST_BYTES = 144,
    SH_S_IFMT = 0xF000,
    SH_S_IFDIR = 0x4000,
    SH_PATH_CAP = 128
};

static unsigned int sh_load_u32(const char *p) {
    return (unsigned int)(unsigned char)p[0]
         | ((unsigned int)(unsigned char)p[1] << 8)
         | ((unsigned int)(unsigned char)p[2] << 16)
         | ((unsigned int)(unsigned char)p[3] << 24);
}

static int sh_is_dir(const char *path) {
    char st[SH_ST_BYTES];
    unsigned int mode;
    if (u_stat(path, st) != 0) return 0;
    mode = sh_load_u32(st + SH_ST_MODE);
    return (mode & SH_S_IFMT) == SH_S_IFDIR;
}

/* Create one directory; with parents, create each missing component.
 * Existing directories are OK under -p (no errno; re-stat after mkdir). */
static int sh_mkdir_one(const char *path, int parents) {
    char buf[SH_PATH_CAP];
    unsigned long n;
    unsigned long i;
    if (path == 0 || path[0] == 0) return -1;
    if (!parents) {
        return u_mkdir(path) < 0 ? -1 : 0;
    }
    n = strlen(path);
    if (n >= SH_PATH_CAP) return -1;
    strcpy(buf, path);
    i = 0;
    if (buf[0] == '/') i = 1;
    while (i <= n) {
        char saved;
        if (i < n && buf[i] != '/' && buf[i] != 0) {
            i++;
            continue;
        }
        saved = buf[i];
        buf[i] = 0;
        if (buf[0] != 0 && !(buf[0] == '/' && buf[1] == 0)) {
            if (!sh_is_dir(buf)) {
                if (u_mkdir(buf) < 0) {
                    if (!sh_is_dir(buf)) {
                        buf[i] = saved;
                        return -1;
                    }
                }
            }
        }
        buf[i] = saved;
        if (i >= n) break;
        while (i < n && buf[i] == '/') i++;
        if (i >= n) {
            /* trailing slash: final component already handled */
            break;
        }
    }
    return 0;
}

/* Remove one empty directory; with -p, also strip empty parents
 * (inverse of mkdir -p). Stops before removing "/". */
static int sh_rmdir_one(const char *path, int parents) {
    char buf[SH_PATH_CAP];
    unsigned long n;
    unsigned long i;
    if (path == 0 || path[0] == 0) return -1;
    if (!parents) {
        return u_rmdir(path) < 0 ? -1 : 0;
    }
    n = strlen(path);
    if (n >= SH_PATH_CAP) return -1;
    strcpy(buf, path);
    for (;;) {
        if (u_rmdir(buf) < 0) return -1;
        n = strlen(buf);
        while (n > 1 && buf[n - 1] == '/') {
            buf[n - 1] = 0;
            n--;
        }
        if (n == 0 || (n == 1 && buf[0] == '/')) return 0;
        i = n;
        while (i > 0 && buf[i - 1] != '/') i--;
        if (i == 0) return 0; /* bare component */
        if (i == 1 && buf[0] == '/') return 0; /* parent is "/" */
        buf[i - 1] = 0;
        if (buf[0] == 0) return 0;
    }
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


/* True if `s` starts with `pfx`. */
static int startswith(const char *s, const char *pfx) {
    unsigned long i = 0;
    while (pfx[i] != 0) {
        if (s[i] != pfx[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

/* Lexical join of absolute `base` + relative `rel` into out (cap bytes). */
static int path_join(char *out, unsigned long cap, const char *base,
                     const char *rel) {
    unsigned long i = 0;
    unsigned long j = 0;
    if (cap < 2) {
        return 0;
    }
    if (rel[0] == '/') {
        while (rel[j] != 0 && i + 1 < cap) {
            out[i++] = rel[j++];
        }
        out[i] = 0;
        return 1;
    }
    while (base[i] != 0 && i + 1 < cap) {
        out[i] = base[i];
        i++;
    }
    if (i == 1 && out[0] == '/') {
        /* root */
    } else if (i > 0 && out[i - 1] != '/' && i + 1 < cap) {
        out[i++] = '/';
    }
    while (rel[j] != 0 && i + 1 < cap) {
        out[i++] = rel[j++];
    }
    out[i] = 0;
    return 1;
}

/* Userland physical resolve via readlink (mirrors kernel path_physical). */
static int urealpath(const char *in, char *out, unsigned long cap) {
    char work[128];
    char prefix[128];
    char tgt[128];
    char rebuilt[128];
    unsigned long loop;
    unsigned long i;
    unsigned long nlen;

    if (in == 0 || out == 0 || cap < 2) {
        return 0;
    }
    if (in[0] == '/') {
        nlen = strlen(in);
        if (nlen + 1 > sizeof(work)) {
            return 0;
        }
        for (i = 0; i <= nlen; i++) {
            work[i] = in[i];
        }
    } else {
        char cwd[128];
        if (u_getcwd(cwd, sizeof(cwd)) == -1) {
            return 0;
        }
        if (!path_join(work, sizeof(work), cwd, in)) {
            return 0;
        }
    }

    for (loop = 0; loop < 8; loop++) {
        int found = 0;
        unsigned long wi = 0;
        prefix[0] = '/';
        prefix[1] = 0;
        while (work[wi] != 0) {
            unsigned long start;
            unsigned long n;
            long rn;
            while (work[wi] == '/') {
                wi++;
            }
            if (work[wi] == 0) {
                break;
            }
            start = wi;
            n = 0;
            while (work[wi + n] != 0 && work[wi + n] != '/') {
                n++;
            }
            if (n == 1 && work[start] == '.') {
                wi += n;
                continue;
            }
            if (n == 2 && work[start] == '.' && work[start + 1] == '.') {
                /* pop last component of prefix */
                unsigned long L = strlen(prefix);
                if (L > 1) {
                    L--;
                    while (L > 0 && prefix[L] != '/') {
                        L--;
                    }
                    if (L == 0) {
                        prefix[0] = '/';
                        prefix[1] = 0;
                    } else {
                        prefix[L] = 0;
                    }
                }
                wi += n;
                continue;
            }
            {
                char comp[64];
                char joined[128];
                unsigned long c;
                if (n >= sizeof(comp)) {
                    return 0;
                }
                for (c = 0; c < n; c++) {
                    comp[c] = work[start + c];
                }
                comp[n] = 0;
                if (!path_join(joined, sizeof(joined), prefix, comp)) {
                    return 0;
                }
                for (c = 0; joined[c] != 0; c++) {
                    prefix[c] = joined[c];
                }
                prefix[c] = 0;
            }
            wi += n;
            rn = u_readlink(prefix, tgt, sizeof(tgt) - 1);
            if (rn >= 0) {
                const char *rest;
                tgt[rn] = 0;
                if (tgt[0] == '/') {
                    nlen = strlen(tgt);
                    if (nlen + 1 > sizeof(rebuilt)) {
                        return 0;
                    }
                    for (i = 0; i <= nlen; i++) {
                        rebuilt[i] = tgt[i];
                    }
                } else {
                    /* parent of prefix + relative tgt */
                    unsigned long L = strlen(prefix);
                    char parent[128];
                    if (L > 1) {
                        L--;
                        while (L > 0 && prefix[L] != '/') {
                            L--;
                        }
                        if (L == 0) {
                            parent[0] = '/';
                            parent[1] = 0;
                        } else {
                            unsigned long p;
                            for (p = 0; p < L; p++) {
                                parent[p] = prefix[p];
                            }
                            parent[L] = 0;
                        }
                    } else {
                        parent[0] = '/';
                        parent[1] = 0;
                    }
                    if (!path_join(rebuilt, sizeof(rebuilt), parent, tgt)) {
                        return 0;
                    }
                }
                rest = &work[wi];
                while (rest[0] == '/') {
                    rest++;
                }
                if (rest[0] != 0) {
                    char combined[128];
                    if (!path_join(combined, sizeof(combined), rebuilt, rest)) {
                        return 0;
                    }
                    nlen = strlen(combined);
                    for (i = 0; i <= nlen; i++) {
                        rebuilt[i] = combined[i];
                    }
                }
                nlen = strlen(rebuilt);
                if (nlen + 1 > sizeof(work)) {
                    return 0;
                }
                for (i = 0; i <= nlen; i++) {
                    work[i] = rebuilt[i];
                }
                found = 1;
                break;
            }
        }
        if (!found) {
            nlen = strlen(work);
            if (nlen + 1 > cap) {
                return 0;
            }
            for (i = 0; i <= nlen; i++) {
                out[i] = work[i];
            }
            return 1;
        }
    }
    return 0;
}

static void print_prompt(void) {
    char cwd[128];
    if (u_getcwd(cwd, sizeof(cwd)) == -1) {
        u_write(1, "fakeos:$ ", 9);
        return;
    }
    if (eq(cwd, "/home") || eq(cwd, "/mnt/home")) {
        u_write(1, "fakeos:~$ ", 10);
        return;
    }
    if (startswith(cwd, "/home/")) {
        printf("fakeos:~/%s$ ", cwd + 6);
        return;
    }
    if (startswith(cwd, "/mnt/home/")) {
        printf("fakeos:~/%s$ ", cwd + 10);
        return;
    }
    printf("fakeos:%s$ ", cwd);
}

/* Shell environment: builtins + passed through execve envp (cap 8). */
struct sh_env {
    int used;
    char name[ENV_NAME];
    char val[ENV_VAL];
};

static struct sh_env sh_envs[MAX_ENV];
static char sh_env_flat[8][128];
static char *sh_envp[9];

static int env_name_ok(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')
          || s[0] == '_')) {
        return 0;
    }
    for (i = 1; s[i] != 0; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
        if (i + 1 >= ENV_NAME) return 0;
    }
    return 1;
}

static const char *sh_getenv(const char *name) {
    int i;
    if (name == 0) return 0;
    for (i = 0; i < MAX_ENV; i++) {
        if (sh_envs[i].used && eq(sh_envs[i].name, name)) {
            return sh_envs[i].val;
        }
    }
    return 0;
}

static int sh_setenv(const char *name, const char *val) {
    int i;
    int free_slot = -1;
    int nlen;
    int vlen;
    if (!env_name_ok(name) || val == 0) return -1;
    nlen = 0;
    while (name[nlen] != 0) nlen++;
    vlen = 0;
    while (val[vlen] != 0) vlen++;
    if (nlen >= ENV_NAME || vlen >= ENV_VAL) return -1;
    for (i = 0; i < MAX_ENV; i++) {
        if (sh_envs[i].used && eq(sh_envs[i].name, name)) {
            int k;
            for (k = 0; k < vlen; k++) sh_envs[i].val[k] = val[k];
            sh_envs[i].val[vlen] = 0;
            return 0;
        }
        if (!sh_envs[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return -1;
    {
        int k;
        for (k = 0; k < nlen; k++) sh_envs[free_slot].name[k] = name[k];
        sh_envs[free_slot].name[nlen] = 0;
        for (k = 0; k < vlen; k++) sh_envs[free_slot].val[k] = val[k];
        sh_envs[free_slot].val[vlen] = 0;
        sh_envs[free_slot].used = 1;
    }
    return 0;
}

static int sh_unsetenv(const char *name) {
    int i;
    if (name == 0) return -1;
    for (i = 0; i < MAX_ENV; i++) {
        if (sh_envs[i].used && eq(sh_envs[i].name, name)) {
            sh_envs[i].used = 0;
            sh_envs[i].name[0] = 0;
            sh_envs[i].val[0] = 0;
            return 0;
        }
    }
    return -1;
}

static void sh_env_init(void) {
    int i;
    for (i = 0; i < MAX_ENV; i++) {
        sh_envs[i].used = 0;
        sh_envs[i].name[0] = 0;
        sh_envs[i].val[0] = 0;
    }
    sh_setenv("PATH", "/bin:/usr/bin:.");
    sh_setenv("HOME", "/home");
    sh_setenv("USER", "root");
    sh_setenv("LOGNAME", "root");
    sh_setenv("FAKECC_PKG", "/src");
}

/* Build a NULL-terminated envp from the table (at most 8 entries). */
static char **sh_build_envp(void) {
    int n = 0;
    int i;
    for (i = 0; i < MAX_ENV && n < 8; i++) {
        int k;
        int nl;
        int vl;
        if (!sh_envs[i].used) continue;
        nl = 0;
        while (sh_envs[i].name[nl] != 0) nl++;
        vl = 0;
        while (sh_envs[i].val[vl] != 0) vl++;
        if (nl + 1 + vl >= 128) continue;
        for (k = 0; k < nl; k++) sh_env_flat[n][k] = sh_envs[i].name[k];
        sh_env_flat[n][nl] = '=';
        for (k = 0; k < vl; k++) sh_env_flat[n][nl + 1 + k] = sh_envs[i].val[k];
        sh_env_flat[n][nl + 1 + vl] = 0;
        sh_envp[n] = sh_env_flat[n];
        n++;
    }
    sh_envp[n] = 0;
    return sh_envp;
}

/* Expand whole-token $? / $NAME (no ${} / mid-string). Writes into out. */
static void expand_token(const char *in, char *out, int outsz, long rc_in) {
    const char *val;
    int i;
    int n;
    if (outsz <= 0) return;
    if (in == 0) {
        out[0] = 0;
        return;
    }
    if (in[0] == '$' && in[1] == '?' && in[2] == 0) {
        /* decimal of last status into out */
        long v = rc_in;
        char tmp[16];
        int t = 0;
        int neg = 0;
        if (v < 0) {
            neg = 1;
            v = -v;
        }
        if (v == 0) {
            tmp[t++] = '0';
        } else {
            while (v > 0 && t < 15) {
                tmp[t++] = (char)('0' + (v % 10));
                v = v / 10;
            }
        }
        i = 0;
        if (neg && i + 1 < outsz) out[i++] = '-';
        while (t > 0 && i + 1 < outsz) {
            t--;
            out[i++] = tmp[t];
        }
        out[i] = 0;
        return;
    }
    if (in[0] == '$' && in[1] != 0 && env_name_ok(in + 1)) {
        val = sh_getenv(in + 1);
        if (val == 0) val = "";
        n = 0;
        while (val[n] != 0 && n + 1 < outsz) {
            out[n] = val[n];
            n++;
        }
        out[n] = 0;
        return;
    }
    n = 0;
    while (in[n] != 0 && n + 1 < outsz) {
        out[n] = in[n];
        n++;
    }
    out[n] = 0;
}

static void expand_argv(char **tokens, long rc_in) {
    static char exp_bufs[MAX_TOK][128];
    int i;
    for (i = 0; tokens[i] != 0 && i < MAX_TOK; i++) {
        expand_token(tokens[i], exp_bufs[i], 128, rc_in);
        tokens[i] = exp_bufs[i];
    }
}

static int is_builtin(const char *cmd) {
    return eq(cmd, "echo") || eq(cmd, "cd") || eq(cmd, "pwd") ||
           eq(cmd, "mkdir") || eq(cmd, "rmdir") || eq(cmd, "exit") ||
           eq(cmd, "help") || eq(cmd, "pid") || eq(cmd, "ppid") ||
           eq(cmd, "uname") || eq(cmd, "sleep") || eq(cmd, "sync") ||
           eq(cmd, "clear") || eq(cmd, "history") || eq(cmd, "jobs") ||
           eq(cmd, "fg") || eq(cmd, "bg") || eq(cmd, "kill") ||
           eq(cmd, "ln") || eq(cmd, "readlink") || eq(cmd, "realpath") ||
           eq(cmd, "env") || eq(cmd, "export") || eq(cmd, "unset") ||
           eq(cmd, "which") || eq(cmd, "chmod") || eq(cmd, "umask")
        || eq(cmd, "chown") || eq(cmd, "id") || eq(cmd, "setuid") || eq(cmd, "seteuid");
}

/* Resolve argv[0] into path (may use buf). Tries PATH components. */
static const char *resolve_cmd(char **argv, char *buf) {
    int has_slash = 0;
    int k;
    const char *path;
    const char *p;
    path = sh_getenv("PATH");
    if (path == 0 || path[0] == 0) {
        path = "/bin:/usr/bin:.";
    }
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
        const char *target;
        if (tokens[1] != 0) {
            target = tokens[1];
        } else {
            target = sh_getenv("HOME");
            if (target == 0 || target[0] == 0) target = "/home";
        }
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
        int parents = 0;
        int ai = 1;
        int rc = 0;
        int saw = 0;
        while (tokens[ai] != 0 && tokens[ai][0] == '-' && tokens[ai][1] != 0) {
            const char *o = tokens[ai] + 1;
            int j;
            if (o[0] == '-' && o[1] == 0) break;
            for (j = 0; o[j] != 0; j++) {
                if (o[j] == 'p') {
                    parents = 1;
                } else {
                    puts("usage: mkdir [-p] DIR...");
                    return 1;
                }
            }
            ai++;
        }
        if (tokens[ai] == 0) {
            puts("sh: mkdir: missing operand");
            return 1;
        }
        while (tokens[ai] != 0) {
            saw = 1;
            if (sh_mkdir_one(tokens[ai], parents) < 0) {
                printf("sh: mkdir: %s: cannot create directory\n", tokens[ai]);
                rc = 1;
            }
            ai++;
        }
        if (!saw) {
            puts("sh: mkdir: missing operand");
            return 1;
        }
        return rc;
    }
    if (eq(tokens[0], "rmdir")) {
        int parents = 0;
        int ai = 1;
        int rc = 0;
        int saw = 0;
        while (tokens[ai] != 0 && tokens[ai][0] == '-' && tokens[ai][1] != 0) {
            const char *o = tokens[ai] + 1;
            int j;
            if (o[0] == '-' && o[1] == 0) break;
            for (j = 0; o[j] != 0; j++) {
                if (o[j] == 'p') {
                    parents = 1;
                } else {
                    puts("usage: rmdir [-p] DIR...");
                    return 1;
                }
            }
            ai++;
        }
        if (tokens[ai] == 0) {
            puts("sh: rmdir: missing operand");
            return 1;
        }
        while (tokens[ai] != 0) {
            saw = 1;
            if (sh_rmdir_one(tokens[ai], parents) < 0) {
                printf("sh: rmdir: %s: cannot remove directory\n", tokens[ai]);
                rc = 1;
            }
            ai++;
        }
        if (!saw) {
            puts("sh: rmdir: missing operand");
            return 1;
        }
        return rc;
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
        puts("builtins: echo[-neE], cd, mkdir[-p], rmdir[-p], pwd[-LP], pid, ppid, uname, sleep, ln[-s], readlink, realpath, env, export, unset, which, chmod, umask, chown, id, setuid, seteuid, sync, clear, history, jobs, fg, bg, kill, exit, help");
        puts("redirects: > >> <   pipelines: |   lists: && ||   jobs: &");
        puts("env: export NAME=val / unset NAME / which CMD; chmod MODE FILE; umask [MODE]; chown UID[:GID] FILE; id; setuid/seteuid; echo[-neE] $NAME / $?; envp -> children");
        puts("programs: hello cat ls cp rm touch mv ping nc ed login head wc getent tail grep printf tee basename dirname true false seq cut tr test/[ uniq sort yes od xargs cmp dd expr hostname echo sleep truncate mkdir pwd ln chmod uname env sync (default PATH=/bin:/usr/bin:.)");
        puts("signals: Ctrl-C/Z/TTIN/TTOU; setsid/fg pgid; ioctl TIOCSPGRP; pipelines share setpgid; fg/bg/jobs; kill [-]PGID; history (/home/.sh_history)");
        return 0;
    }
    if (eq(tokens[0], "history")) {
        hist_print();
        return 0;
    }
    if (eq(tokens[0], "export")) {
        /* export           -> list
         * export NAME=val  -> set
         * export NAME      -> no-op if already set / error if missing value form */
        if (tokens[1] == 0) {
            int i;
            for (i = 0; i < MAX_ENV; i++) {
                if (!sh_envs[i].used) continue;
                printf("export %s=%s\n", sh_envs[i].name, sh_envs[i].val);
            }
            return 0;
        }
        {
            int ai;
            for (ai = 1; tokens[ai] != 0; ai++) {
                char name[ENV_NAME];
                const char *eqp = 0;
                int n = 0;
                int k;
                for (k = 0; tokens[ai][k] != 0; k++) {
                    if (tokens[ai][k] == '=') {
                        eqp = tokens[ai] + k;
                        break;
                    }
                }
                if (eqp == 0) {
                    /* export NAME with no '=': require existing binding */
                    if (sh_getenv(tokens[ai]) == 0) {
                        printf("sh: export: %s: not set\n", tokens[ai]);
                        return 1;
                    }
                    continue;
                }
                n = (int)(eqp - tokens[ai]);
                if (n <= 0 || n >= ENV_NAME) {
                    puts("sh: export: bad name");
                    return 1;
                }
                for (k = 0; k < n; k++) name[k] = tokens[ai][k];
                name[n] = 0;
                if (sh_setenv(name, eqp + 1) != 0) {
                    printf("sh: export: %s: failed\n", name);
                    return 1;
                }
            }
            return 0;
        }
    }
    if (eq(tokens[0], "unset")) {
        int ai;
        if (tokens[1] == 0) {
            puts("sh: unset: missing name");
            return 1;
        }
        for (ai = 1; tokens[ai] != 0; ai++) {
            if (sh_unsetenv(tokens[ai]) != 0) {
                /* POSIX: unset of missing name is ok */
            }
        }
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
    if (eq(tokens[0], "env")) {
        int i;
        for (i = 0; i < MAX_ENV; i++) {
            if (!sh_envs[i].used) continue;
            printf("%s=%s\n", sh_envs[i].name, sh_envs[i].val);
        }
        rc = 0;
    } else if (eq(tokens[0], "which")) {
        char path[96];
        const char *target;
        long fd;
        if (tokens[1] == 0) {
            puts("sh: which: missing command");
            rc = 1;
        } else if (is_builtin(tokens[1])) {
            printf("%s: shell builtin\n", tokens[1]);
            rc = 0;
        } else {
            char *av[2];
            av[0] = tokens[1];
            av[1] = 0;
            target = resolve_cmd(av, path);
            fd = u_open(target, 0, 0);
            if (fd >= 0) {
                u_close(fd);
                puts(target);
                rc = 0;
            } else {
                printf("%s: not found\n", tokens[1]);
                rc = 1;
            }
        }
    } else if (eq(tokens[0], "umask")) {
        if (tokens[1] == 0) {
            long curm = u_umask(0);
            char b[4];
            unsigned int v;
            u_umask(curm);
            v = (unsigned int)(curm & 0x1FF);
            b[0] = (char)('0' + ((v >> 6) & 7));
            b[1] = (char)('0' + ((v >> 3) & 7));
            b[2] = (char)('0' + (v & 7));
            b[3] = 0;
            puts(b);
            rc = 0;
        } else {
            unsigned int mode = 0;
            const char *s = tokens[1];
            int j;
            int bad = 0;
            for (j = 0; s[j] != 0; j++) {
                if (s[j] < '0' || s[j] > '7') {
                    bad = 1;
                    break;
                }
                mode = (mode << 3) | (unsigned int)(s[j] - '0');
            }
            if (bad) {
                puts("sh: umask: bad mode");
                rc = 1;
            } else {
                u_umask((long)mode);
                rc = 0;
            }
        }
    } else if (eq(tokens[0], "chmod")) {
        unsigned int mode = 0;
        const char *s;
        int j;
        int ai;
        int bad = 0;
        if (tokens[1] == 0 || tokens[2] == 0) {
            puts("usage: chmod MODE FILE...");
            rc = 1;
        } else {
            s = tokens[1];
            for (j = 0; s[j] != 0; j++) {
                if (s[j] < '0' || s[j] > '7') {
                    bad = 1;
                    break;
                }
                mode = (mode << 3) | (unsigned int)(s[j] - '0');
            }
            if (bad) {
                puts("sh: chmod: bad mode");
                rc = 1;
            } else {
                rc = 0;
                for (ai = 2; tokens[ai] != 0; ai++) {
                    if (u_chmod(tokens[ai], (long)mode) != 0) {
                        printf("sh: chmod: %s: failed\n", tokens[ai]);
                        rc = 1;
                        break;
                    }
                }
            }
        }
    } else if (eq(tokens[0], "id")) {
        unsigned int gids[8];
        long ng;
        long gi;
        printf("uid=%d euid=%d gid=%d egid=%d",
               (int)u_getuid(), (int)u_geteuid(),
               (int)u_getgid(), (int)u_getegid());
        ng = u_getgroups(8, gids);
        if (ng < 0) {
            ng = 0;
        }
        if (ng > 0) {
            printf(" groups=");
            for (gi = 0; gi < ng; gi++) {
                if (gi) printf(",");
                printf("%d", (int)gids[gi]);
            }
        }
        printf("\n");
        rc = 0;
    } else if (eq(tokens[0], "seteuid")) {
        if (tokens[1] == 0) {
            puts("usage: seteuid UID");
            rc = 1;
        } else {
            unsigned long v = 0;
            const char *s = tokens[1];
            while (*s >= '0' && *s <= '9') {
                v = v * 10UL + (unsigned long)(*s - '0');
                s++;
            }
            if (*s != 0 || u_setreuid((long)(-1), (long)v) != 0) {
                puts("sh: seteuid failed");
                rc = 1;
            } else {
                rc = 0;
            }
        }
    } else if (eq(tokens[0], "setuid")) {
        if (tokens[1] == 0) {
            puts("usage: setuid UID");
            rc = 1;
        } else {
            unsigned long v = 0;
            const char *s = tokens[1];
            while (*s >= '0' && *s <= '9') {
                v = v * 10UL + (unsigned long)(*s - '0');
                s++;
            }
            if (*s != 0 || u_setuid((long)v) != 0) {
                puts("sh: setuid failed");
                rc = 1;
            } else {
                rc = 0;
            }
        }
    } else if (eq(tokens[0], "chown")) {
        /* chown UID[:GID] FILE...  (decimal; missing GID keeps old) */
        if (tokens[1] == 0 || tokens[2] == 0) {
            puts("usage: chown UID[:GID] FILE...");
            rc = 1;
        } else {
            unsigned long uid = 0;
            unsigned long gid = 0xFFFFFFFFUL; /* unchanged */
            const char *spec = tokens[1];
            long ai;
            long i = 0;
            int bad = 0;
            while (spec[i] >= '0' && spec[i] <= '9') {
                uid = uid * 10 + (unsigned long)(spec[i] - '0');
                i++;
            }
            if (i == 0) {
                puts("sh: chown: bad owner");
                bad = 1;
            } else if (spec[i] == ':') {
                long j = i + 1;
                gid = 0;
                if (spec[j] == 0) {
                    puts("sh: chown: bad group");
                    bad = 1;
                } else {
                    while (spec[j] >= '0' && spec[j] <= '9') {
                        gid = gid * 10 + (unsigned long)(spec[j] - '0');
                        j++;
                    }
                    if (spec[j] != 0) {
                        puts("sh: chown: bad group");
                        bad = 1;
                    }
                }
            } else if (spec[i] != 0) {
                puts("sh: chown: bad owner");
                bad = 1;
            }
            if (bad) {
                rc = 1;
            } else {
                rc = 0;
                for (ai = 2; tokens[ai] != 0; ai++) {
                    if (u_chown(tokens[ai], (long)uid, (long)gid) != 0) {
                        printf("sh: chown: %s: failed\n", tokens[ai]);
                        rc = 1;
                        break;
                    }
                }
            }
        }
    } else if (eq(tokens[0], "pwd")) {
        /* pwd [-L|-P]: kernel cwd is already physical; both modes getcwd. */
        char buf[128];
        int ai = 1;
        int bad = 0;
        while (tokens[ai] != 0) {
            if (eq(tokens[ai], "-L") || eq(tokens[ai], "-P")) {
                ai++;
                continue;
            }
            bad = 1;
            break;
        }
        if (bad) {
            puts("sh: pwd: usage: pwd [-L|-P]");
            rc = 1;
        } else if (u_getcwd(buf, sizeof(buf)) == -1) {
            puts("sh: pwd failed");
            rc = 1;
        } else {
            puts(buf);
            rc = 0;
        }
    } else if (eq(tokens[0], "echo")) {
        /* echo [-neE] [ARGS...] — -n no newline, -e escapes, -E literal (default). */
        int nonew = 0;
        int do_esc = 0;
        int ai2 = 1;
        int first = 1;
        int stop = 0;
        while (tokens[ai2] != 0 && tokens[ai2][0] == '-' && tokens[ai2][1] != 0) {
            const char *p = tokens[ai2] + 1;
            int ok = 1;
            int j;
            for (j = 0; p[j] != 0; j++) {
                if (p[j] == 'n') nonew = 1;
                else if (p[j] == 'e') do_esc = 1;
                else if (p[j] == 'E') do_esc = 0;
                else {
                    ok = 0;
                    break;
                }
            }
            if (!ok) break;
            ai2++;
        }
        for (; tokens[ai2] != 0 && !stop; ai2++) {
            if (!first) u_write(1, " ", 1);
            first = 0;
            if (!do_esc) {
                u_write(1, tokens[ai2], strlen(tokens[ai2]));
            } else {
                const char *s = tokens[ai2];
                char outb[256];
                int on = 0;
                while (*s != 0 && on + 1 < 256) {
                    if (*s != '\\') {
                        outb[on++] = *s++;
                        continue;
                    }
                    s++;
                    if (*s == 0) {
                        outb[on++] = '\\';
                        break;
                    }
                    if (*s == 'c') {
                        stop = 1;
                        nonew = 1;
                        s++;
                        break;
                    } else if (*s == 'n') { outb[on++] = '\n'; s++; }
                    else if (*s == 't') { outb[on++] = '\t'; s++; }
                    else if (*s == 'r') { outb[on++] = '\r'; s++; }
                    else if (*s == 'a') { outb[on++] = '\a'; s++; }
                    else if (*s == 'b') { outb[on++] = '\b'; s++; }
                    else if (*s == 'f') { outb[on++] = '\f'; s++; }
                    else if (*s == 'v') { outb[on++] = '\v'; s++; }
                    else if (*s == '\\') { outb[on++] = '\\'; s++; }
                    else if (*s >= '0' && *s <= '7') {
                        int v = 0;
                        int k = 0;
                        while (k < 3 && s[k] >= '0' && s[k] <= '7') {
                            v = (v << 3) + (s[k] - '0');
                            k++;
                        }
                        outb[on++] = (char)v;
                        s += k;
                    } else {
                        outb[on++] = *s++;
                    }
                }
                if (on > 0) u_write(1, outb, (unsigned long)on);
            }
        }
        if (!nonew) u_write(1, "\n", 1);
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
        /* ln [-s] TARGET LINKPATH */
        if (tokens[1] != 0 && eq(tokens[1], "-s")) {
            if (tokens[2] == 0 || tokens[3] == 0 || tokens[4] != 0) {
                puts("sh: ln: usage: ln [-s] TARGET LINKPATH");
                rc = 1;
            } else if (u_symlink(tokens[2], tokens[3]) != 0) {
                puts("sh: ln: symlink failed");
                rc = 1;
            } else {
                rc = 0;
            }
        } else if (tokens[1] == 0 || tokens[2] == 0 || tokens[3] != 0) {
            puts("sh: ln: usage: ln [-s] TARGET LINKPATH");
            rc = 1;
        } else if (u_link(tokens[1], tokens[2]) != 0) {
            puts("sh: ln: link failed");
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
    } else if (eq(tokens[0], "realpath")) {
        char buf[128];
        if (tokens[1] == 0) {
            puts("sh: realpath: missing operand");
            rc = 1;
        } else if (!urealpath(tokens[1], buf, sizeof(buf))) {
            puts("sh: realpath: failed");
            rc = 1;
        } else {
            puts(buf);
            rc = 0;
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
    if (u_execve(target, argv, sh_build_envp()) < 0) {
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
                if (u_execve(target, stages[0], sh_build_envp()) < 0) {
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

    sh_env_init();
    hist_load();
    puts("fakeos shell - session/fg, pipes/redirects, &&/||, jobs/fg/bg/Ctrl-Z/ioctl/tcsetpgrp, history/!!/readline/Tab/Double-Tab/Ctrl-W/K/V/L + /home/.sh_history, env/export/envp, chmod/umask/chown/id, PATH/quotes, kill; persist under /home");

    for (;;) {
        char *tokens[MAX_TOK];
        char *save;
        char *t;
        int nt = 0;
        long n;

        reap_background();

        print_prompt();

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

        expand_argv(tokens, rc);
        rc = run_list(tokens, rc);
    }
}
