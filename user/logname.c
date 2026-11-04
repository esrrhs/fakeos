/* fakeos userland - logname: print login name (thin POSIX).
 * Prefer LOGNAME then USER from /proc/self/environ; else /etc/passwd for
 * getuid(); else numeric uid. No utmp/getlogin(3) yet. Compact argv.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getuid(void);
extern void printf(const char *fmt, ...);
extern unsigned long strlen(const char *s);

enum { NAME_CAP = 64, ENV_CAP = 512 };

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

static int env_name(const char *key, char *out, int cap) {
    char env[ENV_CAP];
    long fd;
    long n;
    long i;
    long start;
    int klen;
    int j;

    if (key == 0 || out == 0 || cap <= 0) return 0;
    out[0] = 0;
    klen = 0;
    while (key[klen]) klen++;
    fd = u_open("/proc/self/environ", 0, 0);
    if (fd < 0) return 0;
    n = u_read(fd, env, sizeof(env) - 1);
    u_close(fd);
    if (n < 0) n = 0;
    env[n] = 0;
    start = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || env[i] == 0) {
            if (i - start > klen && env[start + klen] == '=') {
                int match = 1;
                for (j = 0; j < klen; j++) {
                    if (env[start + j] != key[j]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    int k = 0;
                    long p = start + klen + 1;
                    while (p < i && k < cap - 1) {
                        out[k++] = env[p++];
                    }
                    out[k] = 0;
                    return k > 0;
                }
            }
            start = i + 1;
        }
    }
    return 0;
}

static int passwd_name_for_uid(unsigned int uid, char *name_out, int cap) {
    char buf[512];
    long fd;
    long n;
    long i;

    if (name_out == 0 || cap <= 0) return 0;
    name_out[0] = 0;
    fd = u_open("/etc/passwd", 0, 0);
    if (fd < 0) return 0;
    n = u_read(fd, buf, sizeof(buf) - 1);
    u_close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    i = 0;
    while (i < n) {
        char *line = buf + i;
        char *f[4];
        int nf = 0;
        char *p;
        unsigned int u;
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
        while (*p && nf < 4) {
            if (*p == ':') {
                *p = 0;
                p++;
                f[nf++] = p;
            } else {
                p++;
            }
        }
        if (nf < 3) continue;
        if (!parse_u32(f[2], &u)) continue;
        if (u != uid) continue;
        k = 0;
        while (f[0][k] != 0 && k < cap - 1) {
            name_out[k] = f[0][k];
            k++;
        }
        name_out[k] = 0;
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    char name[NAME_CAP];
    unsigned int uid;
    unsigned long n;

    if (argc > 1) {
        u_write(2, "usage: logname\n", 15);
        return 1;
    }
    if (env_name("LOGNAME", name, NAME_CAP) || env_name("USER", name, NAME_CAP)) {
        n = strlen(name);
        u_write(1, name, n);
        u_write(1, "\n", 1);
        return 0;
    }
    uid = (unsigned int)u_getuid();
    if (passwd_name_for_uid(uid, name, NAME_CAP) && name[0] != 0) {
        n = strlen(name);
        u_write(1, name, n);
        u_write(1, "\n", 1);
        return 0;
    }
    printf("%u\n", uid);
    return 0;
}
