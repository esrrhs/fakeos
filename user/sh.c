/* fakeos userland - tiny interactive shell.
 * Builtins: echo ($? expansion), cd, pwd, exit [code], help.
 * Other commands fork/exec from /bin and are reaped via wait4. */
package user;

typedef unsigned long u64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern long u_chdir(const char *path);
extern long u_mkdir(const char *path);
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

static int eq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

int main(int argc, char **argv) {
    long rc = 0;
    char line[256];

    (void)argc;
    (void)argv;

    puts("fakeos shell - builtins: help, echo, cd, pwd, exit; programs in /bin");

    for (;;) {
        char *tokens[9];
        char *save;
        char *t;
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

        if (eq(tokens[0], "exit")) {
            long code = nt > 1 ? strtol_dec(tokens[1]) : rc;
            u_exit(code);
        }
        if (eq(tokens[0], "cd")) {
            const char *target = nt > 1 ? tokens[1] : "/";
            if (u_chdir(target) < 0) {
                printf("sh: cd: %s: No such file or directory\n", target);
                rc = 1;
            } else {
                rc = 0;
            }
            continue;
        }
        if (eq(tokens[0], "mkdir")) {
            if (nt < 2) {
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
        if (eq(tokens[0], "pwd")) {
            char buf[64];
            if (u_getcwd(buf, sizeof(buf)) == -1) {
                puts("sh: pwd failed");
                rc = 1;
            } else {
                puts(buf);
                rc = 0;
            }
            continue;
        }
        if (eq(tokens[0], "echo")) {
            for (i = 1; i < nt; i++) {
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
            rc = 0;
            continue;
        }
        if (eq(tokens[0], "pid")) {
            printf("%d\n", (int)u_getpid());
            rc = 0;
            continue;
        }
        if (eq(tokens[0], "help")) {
            puts("builtins: echo [args|$?], cd [dir], mkdir [dir], pwd, pid, exit [code], help");
            puts("programs: /bin/hello /bin/cat /bin/ls (PATH prefix auto)");
            rc = 0;
            continue;
        }

        /* External command: a slash in the name means run that exact path
         * (e.g. /tmp/ping, ./tool); otherwise try /bin/<name>. */
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
                if (u_execve(target, tokens, 0) < 0) {
                    printf("sh: %s: command not found\n", tokens[0]);
                    u_exit(127);
                }
                u_exit(127);                /* execve must not return */
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
