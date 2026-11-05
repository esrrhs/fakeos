/* fakeos userland - id: print uid/euid/gid/egid (+ supplementary groups).
 * Thin shell-mirror. Forms:
 *   id
 *   id -u|-g|-G [-nr]  (glued -un/-Gn/-Gr/-nu/...; numeric or /etc name)
 * Default: uid= euid= gid= egid= [groups=]. -u/-g print one id (euid/egid
 * by default; -r selects real uid/gid). -G prints all group IDs
 * space-separated (primary + getgroups; GNU). -n prints names when possible.
 * No USER operand (compact argv / EXEC_MAX_ARGC=8).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getuid(void);
extern long u_geteuid(void);
extern long u_getgid(void);
extern long u_getegid(void);
extern long u_getgroups(long size, unsigned int *list);
extern void printf(const char *fmt, ...);
extern unsigned long strlen(const char *s);

enum { MAX_GIDS = 8 };

static void usage(void) {
    u_write(2, "usage: id [-u|-g|-G] [-nr]\n", 27);
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

/* name:passwd:uid:... — fields 0 and 2. */
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

/* name:x:gid:... — fields 0 and 2. */
static int group_name_for_gid(unsigned int gid, char *name_out, int cap) {
    char buf[512];
    long fd;
    long n;
    long i;

    if (name_out == 0 || cap <= 0) return 0;
    name_out[0] = 0;
    fd = u_open("/etc/group", 0, 0);
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
        unsigned int g;
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
        if (!parse_u32(f[2], &g)) continue;
        if (g != gid) continue;
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

static void emit_id_or_name(unsigned int id, int want_name, int is_user,
                            int trailing_nl) {
    char name[64];
    int ok = 0;
    if (want_name) {
        if (is_user) {
            ok = passwd_name_for_uid(id, name, 64);
        } else {
            ok = group_name_for_gid(id, name, 64);
        }
    }
    if (ok && name[0] != 0) {
        u_write(1, name, strlen(name));
    } else {
        printf("%u", id);
    }
    if (trailing_nl) u_write(1, "\n", 1);
}

/* Collect primary + getgroups into out[], deduped. Returns count. */
static int collect_groups(unsigned int primary, unsigned int *out, int cap) {
    unsigned int gids[MAX_GIDS];
    long ng;
    long gi;
    int n = 0;
    int j;
    if (cap <= 0) return 0;
    out[n++] = primary;
    ng = u_getgroups(MAX_GIDS, gids);
    if (ng < 0) ng = 0;
    for (gi = 0; gi < ng && n < cap; gi++) {
        int seen = 0;
        for (j = 0; j < n; j++) {
            if (out[j] == gids[gi]) {
                seen = 1;
                break;
            }
        }
        if (!seen) out[n++] = gids[gi];
    }
    return n;
}

int main(int argc, char **argv) {
    unsigned int gids[MAX_GIDS];
    long ng;
    long gi;
    int ai = 1;
    int only_u = 0;
    int only_g = 0;
    int only_G = 0;
    int want_name = 0;
    int want_real = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *o = argv[ai] + 1;
        int j;
        if (o[0] == '-' && o[1] == 0) break;
        for (j = 0; o[j] != 0; j++) {
            if (o[j] == 'u') {
                only_u = 1;
            } else if (o[j] == 'g') {
                only_g = 1;
            } else if (o[j] == 'G') {
                only_G = 1;
            } else if (o[j] == 'n') {
                want_name = 1;
            } else if (o[j] == 'r') {
                want_real = 1;
            } else {
                usage();
                return 1;
            }
        }
        ai++;
    }
    if (ai != argc) {
        usage();
        return 1;
    }
    if ((only_u + only_g + only_G) > 1) {
        usage();
        return 1;
    }
    if (want_name && !only_u && !only_g && !only_G) {
        usage();
        return 1;
    }

    if (only_u) {
        unsigned int idv = want_real ? (unsigned int)u_getuid()
                                     : (unsigned int)u_geteuid();
        emit_id_or_name(idv, want_name, 1, 1);
        return 0;
    }
    if (only_g) {
        unsigned int idv = want_real ? (unsigned int)u_getgid()
                                     : (unsigned int)u_getegid();
        emit_id_or_name(idv, want_name, 0, 1);
        return 0;
    }
    if (only_G) {
        unsigned int primary = want_real ? (unsigned int)u_getgid()
                                         : (unsigned int)u_getegid();
        unsigned int all[MAX_GIDS + 1];
        int n;
        int k;
        n = collect_groups(primary, all, MAX_GIDS + 1);
        for (k = 0; k < n; k++) {
            if (k) u_write(1, " ", 1);
            emit_id_or_name(all[k], want_name, 0, 0);
        }
        u_write(1, "\n", 1);
        return 0;
    }

    printf("uid=%d euid=%d gid=%d egid=%d",
           (int)u_getuid(), (int)u_geteuid(),
           (int)u_getgid(), (int)u_getegid());
    ng = u_getgroups(MAX_GIDS, gids);
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
    return 0;
}
