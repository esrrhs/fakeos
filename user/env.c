/* fakeos userland - env: print or run with a modified environment (thin).
 * Forms: env
 *        env [-i] [-u NAME]... [NAME=VALUE]...
 *        env [-i] [-u NAME]... [NAME=VALUE]... COMMAND [ARG...]
 *        env - ...                 (bare '-' same as -i)
 * Without COMMAND: print the resulting environment (one NAME=val per line).
 * With COMMAND: execve with that envp (PATH-lite /bin/ for COMMAND without '/').
 * Inherited start state comes from /proc/self/environ (execve envp).
 * Empty envp still seeds FAKECC_PKG=/src in the kernel (EXEC_MAX_ENVC=8).
 * Compact argv for EXEC_MAX_ARGC=8. printenv is an alias of this binary.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_close(long fd);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern char *strcpy(char *dst, const char *src);
extern unsigned long strlen(const char *s);

enum {
    MAX_ENVC = 8,
    ARG_SLOT = 128,
    ENV_BUF = 256
};

static void usage(void) {
    u_write(2, "usage: env [-i] [-u NAME]... [NAME=VALUE]... [COMMAND [ARG...]]\n", 66);
}

static int name_match(const char *entry, const char *name) {
    int i = 0;
    if (entry == 0 || name == 0) return 0;
    while (name[i] != 0) {
        if (entry[i] != name[i]) return 0;
        i++;
    }
    return entry[i] == '=';
}

/* Copy one NUL-terminated NAME=val into slots[count]; return new count or -1. */
static int slot_add(char slots[][ARG_SLOT], int count, const char *nv) {
    int n;
    int i;
    int eq;
    if (nv == 0 || nv[0] == 0) return -1;
    n = 0;
    while (nv[n] != 0) n++;
    if (n + 1 > ARG_SLOT) return -1;
    eq = 0;
    while (nv[eq] != 0 && nv[eq] != '=') eq++;
    if (eq == 0 || nv[eq] != '=') return -1;
    for (i = 0; i < count; i++) {
        int k = 0;
        while (k < eq && slots[i][k] == nv[k]) k++;
        if (k == eq && slots[i][k] == '=') {
            strcpy(slots[i], nv);
            return count;
        }
    }
    if (count >= MAX_ENVC) return -1;
    strcpy(slots[count], nv);
    return count + 1;
}

static int slot_unset(char slots[][ARG_SLOT], int count, const char *name) {
    int i;
    int j;
    if (name == 0 || name[0] == 0) return count;
    for (i = 0; i < count; ) {
        if (name_match(slots[i], name)) {
            for (j = i; j + 1 < count; j++) {
                strcpy(slots[j], slots[j + 1]);
            }
            count--;
            continue;
        }
        i++;
    }
    return count;
}

static int load_environ(char slots[][ARG_SLOT]) {
    char buf[ENV_BUF];
    long fd;
    long n;
    long i;
    long start;
    int count = 0;

    fd = u_open("/proc/self/environ", 0, 0);
    if (fd < 0) return 0;
    n = u_read(fd, buf, ENV_BUF);
    u_close(fd);
    if (n <= 0) return 0;

    start = 0;
    for (i = 0; i < n; i++) {
        if (buf[i] == 0) {
            if (i > start && count < MAX_ENVC) {
                long len = i - start;
                long k;
                if (len + 1 <= ARG_SLOT) {
                    for (k = 0; k < len; k++) slots[count][k] = buf[start + k];
                    slots[count][len] = 0;
                    count++;
                }
            }
            start = i + 1;
        }
    }
    if (start < n && count < MAX_ENVC) {
        long len = n - start;
        long k;
        if (len + 1 <= ARG_SLOT) {
            for (k = 0; k < len; k++) slots[count][k] = buf[start + k];
            slots[count][len] = 0;
            count++;
        }
    }
    return count;
}

static void print_slots(char slots[][ARG_SLOT], int count) {
    int i;
    for (i = 0; i < count; i++) {
        unsigned long n = strlen(slots[i]);
        if (n > 0) {
            u_write(1, slots[i], n);
            u_write(1, "\n", 1);
        }
    }
}

static int resolve_cmd(const char *cmd, char *pathbuf) {
    unsigned long n;
    if (cmd == 0 || cmd[0] == 0) return -1;
    for (n = 0; cmd[n]; n++) {
        if (cmd[n] == '/') {
            if (n + 1 >= ARG_SLOT) return -1;
            strcpy(pathbuf, cmd);
            return 0;
        }
    }
    if (5 + n + 1 >= ARG_SLOT) return -1;
    strcpy(pathbuf, "/bin/");
    strcpy(pathbuf + 5, cmd);
    return 0;
}

static int is_assignment(const char *s) {
    int i;
    if (s == 0 || s[0] == 0) return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')
          || s[0] == '_')) {
        return 0;
    }
    i = 1;
    while (s[i] != 0 && s[i] != '=') {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
        i++;
    }
    return s[i] == '=' && i > 0;
}

int main(int argc, char **argv) {
    char slots[MAX_ENVC][ARG_SLOT];
    char *envp[MAX_ENVC + 1];
    char pathbuf[ARG_SLOT];
    int count;
    int ai = 1;
    int cleared = 0;
    int i;

    count = load_environ(slots);

    while (ai < argc) {
        const char *a = argv[ai];
        if (a[0] == '-' && a[1] == 0) {
            count = 0;
            cleared = 1;
            ai++;
            continue;
        }
        if (a[0] == '-' && a[1] == 'i' && a[2] == 0) {
            count = 0;
            cleared = 1;
            ai++;
            continue;
        }
        if (a[0] == '-' && a[1] == 'u') {
            const char *name;
            if (a[2] != 0) {
                name = a + 2;
                ai++;
            } else {
                if (ai + 1 >= argc) {
                    usage();
                    return 1;
                }
                name = argv[ai + 1];
                ai += 2;
            }
            if (name[0] == 0) {
                u_write(2, "env: cannot unset empty name\n", 29);
                return 1;
            }
            count = slot_unset(slots, count, name);
            continue;
        }
        if (a[0] == '-') {
            usage();
            return 1;
        }
        break;
    }

    (void)cleared;

    while (ai < argc && is_assignment(argv[ai])) {
        int nc = slot_add(slots, count, argv[ai]);
        if (nc < 0) {
            u_write(2, "env: cannot set environment\n", 28);
            return 1;
        }
        count = nc;
        ai++;
    }

    if (ai >= argc) {
        print_slots(slots, count);
        return 0;
    }

    if (resolve_cmd(argv[ai], pathbuf) != 0) {
        u_write(2, "env: command too long\n", 22);
        return 1;
    }
    for (i = 0; i < count; i++) envp[i] = slots[i];
    envp[count] = 0;
    if (u_execve(pathbuf, argv + ai, envp) < 0) {
        u_write(2, "env: exec failed\n", 17);
        u_exit(127);
    }
    return 127;
}
