/* fakeos userland - nproc: print number of processing units (thin).
 * Forms: nproc [-a|--all]
 * Counts "processor" lines in /proc/cpuinfo (published at boot from
 * smp_cpu_count). No affinity mask yet — online == all.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

static int starts_processor(const char *line) {
    const char *p = "processor";
    int i;
    for (i = 0; p[i]; i++) {
        if (line[i] != p[i]) return 0;
    }
    if (line[i] == ':' || line[i] == '\t' || line[i] == ' ') return 1;
    return 0;
}

int main(int argc, char **argv) {
    char buf[512];
    char line[128];
    long fd;
    long n;
    int li = 0;
    int i;
    unsigned int count = 0;

    if (argc > 2) {
        u_write(2, "usage: nproc [-a|--all]\n", 24);
        return 1;
    }
    if (argc == 2) {
        if (strcmp(argv[1], "-a") != 0 && strcmp(argv[1], "--all") != 0) {
            u_write(2, "usage: nproc [-a|--all]\n", 24);
            return 1;
        }
    }

    fd = u_open("/proc/cpuinfo", 0, 0);
    if (fd < 0) {
        printf("1\n");
        return 0;
    }
    while ((n = u_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < (int)n; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[li] = 0;
                if (starts_processor(line)) count++;
                li = 0;
                continue;
            }
            if (li < (int)sizeof(line) - 1) line[li++] = c;
        }
    }
    u_close(fd);
    if (li > 0) {
        line[li] = 0;
        if (starts_processor(line)) count++;
    }
    if (count == 0) count = 1;
    printf("%u\n", count);
    return 0;
}
