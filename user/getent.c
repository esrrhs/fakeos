/* fakeos userland - getent: look up passwd/group/hosts.
 *   getent passwd [name|uid ...]
 *   getent group  [name|gid ...]
 *   getent hosts  [name|ip ...]
 * With no keys, dump the database. hosts keys also try DNS via resolve_host.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);
extern int resolve_host(const char *name, unsigned int *out);

static int is_digits(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static int parse_ipv4(const char *s, unsigned int *out) {
    unsigned int b[4];
    int i = 0;
    int n = 0;
    unsigned int cur = 0;
    int digits = 0;
    while (1) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (unsigned int)(c - '0');
            if (cur > 255) return 0;
            digits++;
            i++;
            continue;
        }
        if (digits == 0) return 0;
        b[n++] = cur;
        cur = 0;
        digits = 0;
        if (c == '.') {
            if (n >= 4) return 0;
            i++;
            continue;
        }
        if (c == 0) break;
        return 0;
    }
    if (n != 4) return 0;
    *out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return 1;
}

static void print_ipv4(unsigned int ip) {
    printf("%u.%u.%u.%u",
           (ip >> 24) & 0xFF,
           (ip >> 16) & 0xFF,
           (ip >> 8) & 0xFF,
           ip & 0xFF);
}

static void usage(void) {
    u_write(2, "usage: getent passwd|group|hosts [key...]\n", 42);
}

/* Return pointer to field N (0-based) in a colon-separated line; mutates. */
static char *colon_field(char *line, int want) {
    int f = 0;
    char *p = line;
    if (want == 0) return line;
    while (*p) {
        if (*p == ':') {
            f++;
            p++;
            if (f == want) return p;
            continue;
        }
        p++;
    }
    return 0;
}

static int field0_eq(char *line, const char *key) {
    char *p = line;
    while (*p && *p != ':') p++;
    if (*p == ':') {
        char saved = *p;
        *p = 0;
        if (strcmp(line, key) == 0) {
            *p = saved;
            return 1;
        }
        *p = saved;
        return 0;
    }
    return strcmp(line, key) == 0;
}

static int field_num_eq(char *line, int field, const char *key) {
    char *p;
    char *end;
    char saved;
    p = colon_field(line, field);
    if (p == 0) return 0;
    end = p;
    while (*end && *end != ':') end++;
    saved = *end;
    *end = 0;
    if (strcmp(p, key) == 0) {
        *end = saved;
        return 1;
    }
    *end = saved;
    return 0;
}

static int match_passwd_or_group(char *line, const char *key, int is_group) {
    int id_field = is_group ? 2 : 2; /* both: name:x:id:... */
    if (field0_eq(line, key)) return 1;
    if (is_digits(key) && field_num_eq(line, id_field, key)) return 1;
    return 0;
}

/* Read path into buf (NUL-terminated). Returns length or -1. */
static long slurp(const char *path, char *buf, long cap) {
    long fd;
    long n;
    fd = u_open(path, 0, 0);
    if (fd < 0) return -1;
    n = u_read(fd, buf, (unsigned long)(cap - 1));
    u_close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

static int dump_or_filter_colon(const char *path, char **keys, int nkeys, int is_group) {
    char buf[2048];
    long n;
    long i;
    int found = 0;
    int any = 0;

    n = slurp(path, buf, (long)sizeof(buf));
    if (n < 0) {
        u_write(2, "getent: cannot open database\n", 29);
        return 1;
    }
    i = 0;
    while (i < n) {
        char *line = buf + i;
        long start = i;
        while (i < n && buf[i] != '\n') i++;
        if (i < n) {
            buf[i] = 0;
            i++;
        }
        if (line[0] == '#' || line[0] == 0) continue;
        if (nkeys == 0) {
            printf("%s\n", line);
            any = 1;
            continue;
        }
        {
            int k;
            for (k = 0; k < nkeys; k++) {
                /* restore line each attempt: mutate carefully on a copy window */
                if (match_passwd_or_group(line, keys[k], is_group)) {
                    printf("%s\n", line);
                    found = 1;
                    any = 1;
                    break;
                }
            }
        }
        (void)start;
    }
    if (nkeys > 0 && !found) return 2;
    (void)any;
    return 0;
}

static int hosts_line_has_ip(char *line, unsigned int want) {
    char *ip;
    char *rest;
    unsigned int addr;
    char saved;
    ip = line;
    while (*line && *line != ' ' && *line != '\t') line++;
    if (*line == 0) return 0;
    saved = *line;
    *line = 0;
    rest = line + 1;
    if (!parse_ipv4(ip, &addr) || addr != want) {
        *line = saved;
        return 0;
    }
    *line = saved;
    (void)rest;
    return 1;
}

static int hosts_line_has_name(char *line, const char *name) {
    char *p = line;
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    while (*p) {
        char *host = p;
        char saved;
        while (*p && *p != ' ' && *p != '\t') p++;
        saved = *p;
        *p = 0;
        if (strcmp(host, name) == 0) {
            *p = saved;
            return 1;
        }
        *p = saved;
        while (*p == ' ' || *p == '\t') p++;
    }
    return 0;
}

static int do_hosts(char **keys, int nkeys) {
    char buf[1024];
    long n;
    long i;
    int found = 0;
    int k;

    if (nkeys == 0) {
        n = slurp("/etc/hosts", buf, (long)sizeof(buf));
        if (n < 0) {
            u_write(2, "getent: cannot open /etc/hosts\n", 31);
            return 1;
        }
        i = 0;
        while (i < n) {
            char *line = buf + i;
            while (i < n && buf[i] != '\n') i++;
            if (i < n) {
                buf[i] = 0;
                i++;
            }
            if (line[0] == '#' || line[0] == 0) continue;
            printf("%s\n", line);
        }
        return 0;
    }

    for (k = 0; k < nkeys; k++) {
        unsigned int ip;
        int hit = 0;
        const char *key = keys[k];

        /* Prefer /etc/hosts dump for matching lines (preserves aliases). */
        n = slurp("/etc/hosts", buf, (long)sizeof(buf));
        if (n >= 0) {
            i = 0;
            while (i < n) {
                char *line = buf + i;
                while (i < n && buf[i] != '\n') i++;
                if (i < n) {
                    buf[i] = 0;
                    i++;
                }
                if (line[0] == '#' || line[0] == 0) continue;
                if (parse_ipv4(key, &ip)) {
                    if (hosts_line_has_ip(line, ip)) {
                        printf("%s\n", line);
                        hit = 1;
                    }
                } else if (hosts_line_has_name(line, key)) {
                    printf("%s\n", line);
                    hit = 1;
                }
            }
        }
        if (hit) {
            found = 1;
            continue;
        }

        /* Name not in hosts file: DNS / dotted via resolve_host. */
        if (parse_ipv4(key, &ip)) {
            /* Bare IP with no hosts hit: print identity mapping. */
            print_ipv4(ip);
            printf("\t%s\n", key);
            found = 1;
            continue;
        }
        if (resolve_host(key, &ip)) {
            print_ipv4(ip);
            printf("\t%s\n", key);
            found = 1;
            continue;
        }
    }
    if (!found) return 2;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "passwd") == 0) {
        return dump_or_filter_colon("/etc/passwd", argv + 2, argc - 2, 0);
    }
    if (strcmp(argv[1], "group") == 0) {
        return dump_or_filter_colon("/etc/group", argv + 2, argc - 2, 1);
    }
    if (strcmp(argv[1], "hosts") == 0) {
        return do_hosts(argv + 2, argc - 2);
    }
    usage();
    return 1;
}
