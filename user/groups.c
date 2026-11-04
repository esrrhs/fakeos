/* fakeos userland - groups: print current user's group names (thin).
 * Primary egid first, then getgroups() supplementary; names via /etc/group
 * with numeric fallback. No USERNAME operand (compact argv).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getegid(void);
extern long u_getgroups(long size, unsigned int *list);
extern void printf(const char *fmt, ...);
extern unsigned long strlen(const char *s);

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

/* Find name for gid in /etc/group. Returns 1 and fills name_out on hit. */
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

static void print_gid(unsigned int gid, int *first) {
    char name[64];
    if (!*first) {
        u_write(1, " ", 1);
    }
    *first = 0;
    if (group_name_for_gid(gid, name, 64) && name[0] != 0) {
        u_write(1, name, strlen(name));
    } else {
        printf("%u", gid);
    }
}

int main(int argc, char **argv) {
    unsigned int gids[8];
    unsigned int egid;
    long ng;
    long gi;
    int first = 1;
    int seen_egid = 0;

    if (argc > 1) {
        u_write(2, "usage: groups\n", 14);
        return 1;
    }
    egid = (unsigned int)u_getegid();
    print_gid(egid, &first);
    seen_egid = 1;
    ng = u_getgroups(8, gids);
    if (ng < 0) {
        ng = 0;
    }
    for (gi = 0; gi < ng; gi++) {
        if (seen_egid && gids[gi] == egid) continue;
        print_gid(gids[gi], &first);
    }
    u_write(1, "\n", 1);
    return 0;
}
