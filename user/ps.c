/* fakeos userland - ps: list visible user processes from live /proc/<pid>.
 * Forms: ps [-A|-e|-a]
 * Columns: PID PPID STAT COMMAND. STAT is the /proc status letter (R/S/T/Z).
 * COMMAND comes from /proc/<pid>/cmdline (NULs as spaces), else Name.
 * Flags are accepted as "all processes" (the only table we can show).
 * Vanished pids between getdents and open are skipped, not invented.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_getdents(long fd, void *buf, unsigned long n);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum {
    DENT_RECLEN = 48,
    NAME_OFF = 19,
    MAX_PIDS = 32,
    PATH_CAP = 64,
    SNAP_CAP = 1024
};

static int is_digits(const char *s) {
    int i;
    if (s[0] == 0) {
        return 0;
    }
    for (i = 0; s[i] != 0; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return 1;
}

static unsigned int parse_u(const char *s) {
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned int)(*s - '0');
        s++;
    }
    return v;
}

static int key_val(const char *buf, long n, const char *key, char *out, int cap) {
    long i;
    long klen = 0;
    long start;
    int j;
    while (key[klen] != 0) {
        klen++;
    }
    for (i = 0; i + klen < n; i++) {
        long k;
        if (i != 0 && buf[i - 1] != '\n') {
            continue;
        }
        for (k = 0; k < klen; k++) {
            if (buf[i + k] != key[k]) {
                break;
            }
        }
        if (k != klen) {
            continue;
        }
        start = i + klen;
        if (start < n && buf[start] == '\t') {
            start++;
        }
        j = 0;
        while (start + j < n && buf[start + j] != '\n' && j + 1 < cap) {
            out[j] = buf[start + j];
            j++;
        }
        out[j] = 0;
        return 1;
    }
    return 0;
}

static void join_proc_status(char *out, const char *pid) {
    const char *pre = "/proc/";
    const char *post = "/status";
    int i = 0;
    int j = 0;
    while (pre[j] != 0) {
        out[i++] = pre[j++];
    }
    j = 0;
    while (pid[j] != 0 && i + 12 < PATH_CAP) {
        out[i++] = pid[j++];
    }
    j = 0;
    while (post[j] != 0 && i + 1 < PATH_CAP) {
        out[i++] = post[j++];
    }
    out[i] = 0;
}

static void join_proc_cmdline(char *out, const char *pid) {
    const char *pre = "/proc/";
    const char *post = "/cmdline";
    int i = 0;
    int j = 0;
    while (pre[j] != 0) {
        out[i++] = pre[j++];
    }
    j = 0;
    while (pid[j] != 0 && i + 12 < PATH_CAP) {
        out[i++] = pid[j++];
    }
    j = 0;
    while (post[j] != 0 && i + 1 < PATH_CAP) {
        out[i++] = post[j++];
    }
    out[i] = 0;
}

static void cmdline_spaces(char *buf, long n, int cap) {
    long i;
    long last = 0;
    if (n >= cap) {
        n = cap - 1;
    }
    for (i = 0; i < n; i++) {
        if (buf[i] == 0) {
            buf[i] = ' ';
        } else {
            last = i + 1;
        }
    }
    buf[last] = 0;
}

static int emit_one(const char *pidname) {
    char path[PATH_CAP];
    char buf[SNAP_CAP];
    char name[64];
    char state[32];
    char pidv[16];
    char ppidv[16];
    char cmd[SNAP_CAP];
    long fd;
    long n;
    unsigned int pid;
    unsigned int ppid;
    char st;

    join_proc_status(path, pidname);
    fd = u_open(path, 0, 0);
    if (fd < 0) {
        return 0;                    /* exited between listing and open */
    }
    n = u_read(fd, buf, SNAP_CAP - 1);
    u_close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = 0;
    if (!key_val(buf, n, "Pid:", pidv, 16)
        || !key_val(buf, n, "PPid:", ppidv, 16)
        || !key_val(buf, n, "State:", state, 32)
        || !key_val(buf, n, "Name:", name, 64)) {
        printf("ps: %s: status missing fields\n", path);
        return -1;
    }
    pid = parse_u(pidv);
    ppid = parse_u(ppidv);
    st = state[0] != 0 ? state[0] : '?';

    join_proc_cmdline(path, pidname);
    fd = u_open(path, 0, 0);
    cmd[0] = 0;
    if (fd >= 0) {
        n = u_read(fd, cmd, SNAP_CAP - 1);
        u_close(fd);
        if (n > 0) {
            cmdline_spaces(cmd, n, SNAP_CAP);
        } else {
            cmd[0] = 0;
        }
    }
    if (cmd[0] == 0) {
        int i = 0;
        while (name[i] != 0 && i + 1 < SNAP_CAP) {
            cmd[i] = name[i];
            i++;
        }
        cmd[i] = 0;
    }
    printf("%s\t%s\t%c\t%s\n", pidv, ppidv, (int)st, cmd);
    (void)pid;
    (void)ppid;
    return 1;
}

int main(int argc, char **argv) {
    char dents[1536];
    char names[MAX_PIDS][16];
    int nnames = 0;
    long fd;
    long n;
    long off;
    int i;
    int shown = 0;
    int rc = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-A") != 0 && strcmp(argv[i], "-e") != 0
            && strcmp(argv[i], "-a") != 0) {
            u_write(2, "usage: ps [-A|-e|-a]\n", 21);
            return 1;
        }
    }

    fd = u_open("/proc", 0, 0);
    if (fd < 0) {
        u_write(2, "ps: cannot open /proc\n", 22);
        return 1;
    }
    while ((n = u_getdents(fd, dents, sizeof(dents))) > 0) {
        for (off = 0; off + DENT_RECLEN <= n; off += DENT_RECLEN) {
            const char *nm = dents + off + NAME_OFF;
            int k = 0;
            if (!is_digits(nm)) {
                continue;
            }
            if (nnames >= MAX_PIDS) {
                break;
            }
            while (nm[k] != 0 && k < 15) {
                names[nnames][k] = nm[k];
                k++;
            }
            names[nnames][k] = 0;
            nnames++;
        }
        if (nnames >= MAX_PIDS) {
            break;
        }
    }
    u_close(fd);
    if (n < 0) {
        u_write(2, "ps: getdents /proc failed\n", 26);
        return 1;
    }
    if (nnames == 0) {
        u_write(2, "ps: no processes in /proc\n", 26);
        return 1;
    }

    printf("PID\tPPID\tSTAT\tCOMMAND\n");
    for (i = 0; i < nnames; i++) {
        int r = emit_one(names[i]);
        if (r < 0) {
            rc = 1;
        } else if (r > 0) {
            shown++;
        }
    }
    if (shown == 0) {
        u_write(2, "ps: no readable /proc/<pid>/status\n", 35);
        return 1;
    }
    return rc;
}
