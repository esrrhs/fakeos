/* fakeos userland - minimal login/getty stub.
 * Accepts users from /etc/passwd (root / empty-or-hashed password), applies
 * uid/gid + supplementary groups from /etc/group, then execve /bin/sh.
 * /etc/passwd: name:password:uid:gid:... — empty hash => empty password;
 * $fake$salt$hash via libc crypt_check; legacy plaintext still accepted. */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_execve(const char *path, char **argv, char **envp);
extern long u_setuid(long uid);
extern long u_setgid(long gid);
extern long u_setgroups(long size, const unsigned int *list);
extern void u_exit(long code);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern char *strcpy(char *dst, const char *src);
extern void printf(const char *fmt, ...);
extern void puts(const char *s);
extern int crypt_check(const char *key, const char *setting);

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

static int parse_u32(const char *s, unsigned int *out) {
    unsigned int v = 0;
    int any = 0;
    if (s == 0) return 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned int)(*s - '0');
        s++;
        any = 1;
    }
    if (!any || *s != 0) return 0;
    *out = v;
    return 1;
}

/* Look up name in /etc/passwd. Fills password, uid, gid. Returns 1 if found. */
static int passwd_lookup(const char *name, char *pw_out, int cap,
                         unsigned int *uid_out, unsigned int *gid_out) {
    char buf[512];
    long fd;
    long n;
    long i;
    if (pw_out && cap > 0) pw_out[0] = 0;
    if (uid_out) *uid_out = 0;
    if (gid_out) *gid_out = 0;
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
        char *f[6];
        int nf = 0;
        char *p;
        int k;
        while (i < n && buf[i] != '\n') i++;
        if (i < n) {
            buf[i] = 0;
            i++;
        }
        if (line[0] == 0 || line[0] == '#') continue;
        p = line;
        f[0] = p;
        nf = 1;
        while (*p && nf < 6) {
            if (*p == ':') {
                *p = 0;
                p++;
                f[nf++] = p;
            } else {
                p++;
            }
        }
        if (nf < 4) continue;
        if (strcmp(f[0], name) != 0) continue;
        if (pw_out && cap > 0) {
            k = 0;
            while (f[1][k] != 0 && k < cap - 1) {
                pw_out[k] = f[1][k];
                k++;
            }
            pw_out[k] = 0;
        }
        if (uid_out) {
            if (!parse_u32(f[2], uid_out)) *uid_out = 0;
        }
        if (gid_out) {
            if (!parse_u32(f[3], gid_out)) *gid_out = 0;
        }
        return 1;
    }
    return 0;
}

/* Collect supplementary gids from /etc/group where member list contains name.
 * Primary gid is not duplicated. Returns count written into out[]. */
static int groups_for_user(const char *name, unsigned int primary,
                           unsigned int *out, int cap) {
    char buf[512];
    long fd;
    long n;
    long i;
    int ng = 0;
    fd = u_open("/etc/group", 0, 0);
    if (fd < 0) return 0;
    n = u_read(fd, buf, sizeof(buf) - 1);
    u_close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    i = 0;
    while (i < n && ng < cap) {
        char *line = buf + i;
        char *c1;
        char *c2;
        char *c3;
        char *members;
        unsigned int gid = 0;
        char *m;
        int match = 0;
        while (i < n && buf[i] != '\n') i++;
        if (i < n) {
            buf[i] = 0;
            i++;
        }
        if (line[0] == 0 || line[0] == '#') continue;
        c1 = line;
        while (*c1 && *c1 != ':') c1++;
        if (*c1 != ':') continue;
        *c1 = 0;
        c2 = c1 + 1;
        while (*c2 && *c2 != ':') c2++;
        if (*c2 != ':') continue;
        *c2 = 0;
        c3 = c2 + 1;
        while (*c3 && *c3 != ':') c3++;
        if (*c3 != ':') continue;
        *c3 = 0;
        members = c3 + 1;
        if (!parse_u32(c2 + 1, &gid)) continue;
        if (gid == primary) continue;
        m = members;
        while (*m) {
            char *end = m;
            char save;
            while (*end && *end != ',') end++;
            save = *end;
            *end = 0;
            if (strcmp(m, name) == 0) {
                match = 1;
            }
            *end = save;
            if (save == 0) break;
            m = end + 1;
            if (match) break;
        }
        if (match) {
            out[ng++] = gid;
        }
    }
    return ng;
}

static void apply_identity(const char *name, unsigned int uid, unsigned int gid) {
    unsigned int gids[8];
    int ng;
    ng = groups_for_user(name, gid, gids, 8);
    /* Root may clear; non-empty set requires CAP — login still runs as root. */
    if (u_setgroups((long)ng, ng > 0 ? gids : 0) != 0) {
        /* Ignore missing setgroups on older kernels; keep going. */
        (void)0;
    }
    if (u_setgid((long)gid) != 0) {
        puts("login: setgid failed");
    }
    if (u_setuid((long)uid) != 0) {
        puts("login: setuid failed");
    }
}

int main(int argc, char **argv) {
    char user[64];
    char pass[64];
    char *sh_argv[2];
    char env_user[80];
    char env_home[80];
    char *envp[4];
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
        {
            char pwhash[64];
            unsigned int uid = 0;
            unsigned int gid = 0;
            if (passwd_lookup(user, pwhash, sizeof(pwhash), &uid, &gid)
                && (crypt_check(pass, pwhash)
                    || (pwhash[0] == 0 && pass[0] == 0)
                    || (pwhash[0] != '$' && pwhash[0] != 0 && strcmp(pass, pwhash) == 0))) {
                printf("Welcome %s\n", user);
                apply_identity(user, uid, gid);
                /* Seed USER/HOME for the shell; PATH comes from shell defaults. */
                {
                    int i = 0;
                    env_user[0] = 'U'; env_user[1] = 'S'; env_user[2] = 'E';
                    env_user[3] = 'R'; env_user[4] = '=';
                    i = 5;
                    while (user[i - 5] && i < (int)sizeof(env_user) - 1) {
                        env_user[i] = user[i - 5];
                        i++;
                    }
                    env_user[i] = 0;
                }
                if (uid == 0) {
                    strcpy(env_home, "HOME=/home");
                } else {
                    strcpy(env_home, "HOME=/home");
                }
                envp[0] = env_user;
                envp[1] = env_home;
                envp[2] = "FAKECC_PKG=/src";
                envp[3] = 0;
                sh_argv[0] = "/bin/sh";
                sh_argv[1] = 0;
                if (u_execve("/bin/sh", sh_argv, envp) != 0) {
                    puts("login: exec /bin/sh failed");
                    u_exit(1);
                }
            }
        }
        puts("Login incorrect");
    }
    return 0;
}
