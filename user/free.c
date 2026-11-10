/* fakeos userland - free: report physical memory from live /proc/meminfo.
 * Forms: free [-k]
 * Prints MemTotal/MemUsed/MemFree/MemAvailable in kB. Values come from the
 * kernel's buddy counters; missing MemTotal or MemFree>MemTotal is an error.
 * -k is accepted (kB is the only unit).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

static int key_u32(const char *buf, long n, const char *key, unsigned int *out) {
    long i;
    long klen = 0;
    unsigned int v;
    while (key[klen] != 0) {
        klen++;
    }
    for (i = 0; i + klen < n; i++) {
        long k;
        long p;
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
        p = i + klen;
        while (p < n && (buf[p] == ' ' || buf[p] == '\t')) {
            p++;
        }
        if (p >= n || buf[p] < '0' || buf[p] > '9') {
            return 0;
        }
        v = 0;
        while (p < n && buf[p] >= '0' && buf[p] <= '9') {
            v = v * 10u + (unsigned int)(buf[p] - '0');
            p++;
        }
        *out = v;
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    char buf[1024];
    long fd;
    long n;
    int i;
    unsigned int total = 0;
    unsigned int used = 0;
    unsigned int avail = 0;
    unsigned int memfree = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") != 0) {
            u_write(2, "usage: free [-k]\n", 17);
            return 1;
        }
    }

    fd = u_open("/proc/meminfo", 0, 0);
    if (fd < 0) {
        u_write(2, "free: cannot open /proc/meminfo\n", 32);
        return 1;
    }
    n = u_read(fd, buf, 1023);
    u_close(fd);
    if (n <= 0) {
        u_write(2, "free: cannot read /proc/meminfo\n", 32);
        return 1;
    }
    buf[n] = 0;
    if (!key_u32(buf, n, "MemTotal:", &total) || total == 0) {
        u_write(2, "free: MemTotal missing or zero\n", 31);
        return 1;
    }
    if (!key_u32(buf, n, "MemFree:", &memfree)) {
        u_write(2, "free: MemFree missing\n", 22);
        return 1;
    }
    if (memfree > total) {
        u_write(2, "free: MemFree exceeds MemTotal\n", 31);
        return 1;
    }
    if (!key_u32(buf, n, "MemUsed:", &used)) {
        used = total - memfree;
    }
    if (!key_u32(buf, n, "MemAvailable:", &avail)) {
        avail = memfree;
    }
    printf("        total        used        free   available\n");
    printf("Mem:    %u kB    %u kB    %u kB    %u kB\n",
           total, used, memfree, avail);
    return 0;
}
