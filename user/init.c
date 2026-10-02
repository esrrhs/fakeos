/* fakeos userland - pid 1: boot regression driver then execve()'s into sh.
 * Exercises every stage-3 and stage-4 contract from user space. */
package user;

typedef unsigned long u64;
typedef long i64;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_read(long fd, void *buf, unsigned long n);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_lseek(long fd, long off, long whence);
extern long u_mmap(void *addr, unsigned long len, long prot, long flags,
                   long fd, long off);
extern long u_getpid(void);
extern long u_yield(void);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern long u_getdents(long fd, void *buf, unsigned long len);

/* libc.c (same package) */
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);
extern void puts(const char *s);
extern void *malloc(unsigned long size);
extern void free(void *ptr);

int g_shared_marker = 0x1234;   /* .data: inherited by fork, COW-diverged */

/* One fork/COW/wait round: child diverges the inherited .data variable, the
 * parent reaps it through wait4 and observes its own untouched copy. */
static void cow_round(long pid) {
    long child = u_fork();
    long status = 0;
    long reaped;
    if (child == 0) {
        g_shared_marker = 0x5678;
        if (g_shared_marker == 0x5678) {
            puts("[U][PASS] child COW write took effect");
        } else {
            puts("[U][FAIL] child COW write lost");
        }
        printf("[U] child of pid %d\n", (int)pid);
        u_exit(0);
    }
    reaped = u_wait4(child, &status, 0, 0);
    if (reaped != child) {
        puts("[U][FAIL] wait4 did not reap fork child");
    }
    if (g_shared_marker == 0x1234) {
        puts("[U][PASS] parent COW value intact after child write");
    } else {
        puts("[U][FAIL] parent COW value clobbered");
    }
}

static void file_test(void) {
    static const char pat[] = "fakeos-vfs-pattern-0123456789";
    long pat_len = (long)strlen(pat);
    char buf[128];
    long fd;
    long n;
    long i;

    fd = u_open("/test.txt", 0x42, 0);       /* O_RDWR | O_CREAT */
    if (fd < 0) {
        puts("[U][FAIL] open O_CREAT /test.txt");
        return;
    }
    if (u_write(fd, pat, (unsigned long)pat_len) != pat_len
        || u_write(fd, pat, (unsigned long)pat_len) != pat_len) {
        puts("[U][FAIL] write /test.txt");
        u_close(fd);
        return;
    }
    if (u_lseek(fd, 0, 0) != 0) {
        puts("[U][FAIL] lseek to 0");
        u_close(fd);
        return;
    }
    n = u_read(fd, buf, (unsigned long)pat_len);
    if (n != pat_len) {
        puts("[U][FAIL] short read back");
        u_close(fd);
        return;
    }
    for (i = 0; i < pat_len; i++) {
        if (buf[i] != pat[i]) {
            puts("[U][FAIL] pattern mismatch");
            u_close(fd);
            return;
        }
    }
    if (u_lseek(fd, pat_len, 0) != pat_len) {
        puts("[U][FAIL] lseek to second copy");
        u_close(fd);
        return;
    }
    n = u_read(fd, buf, (unsigned long)pat_len);
    for (i = 0; i < n; i++) {
        if (buf[i] != pat[i]) {
            puts("[U][FAIL] second copy mismatch");
            u_close(fd);
            return;
        }
    }
    u_close(fd);

    /* Negative cases must fail safely without killing the process. */
    if (u_open("/no/such/file", 0, 0) != -1) {
        puts("[U][FAIL] open missing path should fail");
        return;
    }
    if (u_close(99) != -1 || u_read(99, buf, 1) != -1) {
        puts("[U][FAIL] bad fd operations should fail");
        return;
    }
    /* Static published files are immutable. */
    if (u_open("/etc/motd", 2, 0) != -1) {
        puts("[U][FAIL] static file opened writable");
        return;
    }
    puts("[U][PASS] vfs file write/read/seek");
}

static void heap_test(void) {
    unsigned char *a = (unsigned char *)malloc(16);
    unsigned char *b = (unsigned char *)malloc(256);
    unsigned char *c = (unsigned char *)malloc(2048);
    unsigned long i;
    if (a == 0 || b == 0 || c == 0) {
        puts("[U][FAIL] malloc returned null");
        return;
    }
    a[0] = 0xA1;
    b[0] = 0xB2;
    b[255] = 0xB3;
    for (i = 0; i < 2048; i++) {
        c[i] = (unsigned char)(i & 0xFF);
    }
    if (a[0] != 0xA1 || b[0] != 0xB2 || b[255] != 0xB3
        || c[0] != 0 || c[255] != 255 || c[2047] != (unsigned char)(2047 & 0xFF)) {
        puts("[U][FAIL] heap payload corrupted");
        return;
    }
    free(c);
    free(b);
    free(a);
    puts("[U][PASS] brk heap malloc");
}

static void mmap_test(void) {
    unsigned char *p;
    if (u_mmap(0, 0, 3, 0x22, -1, 0) != -1) {
        puts("[U][FAIL] mmap len=0 should fail");
        return;
    }
    if (u_mmap(0, 4096, 3, 0x01, -1, 0) != -1) {
        puts("[U][FAIL] non-anon mmap should fail");
        return;
    }
    p = (unsigned char *)u_mmap(0, 8192, 3, 0x22, -1, 0);
    if (p == (unsigned char *)-1 || p == 0) {
        puts("[U][FAIL] anonymous mmap");
        return;
    }
    p[0] = 1;
    p[4095] = 2;
    p[4096] = 3;
    p[8191] = 4;
    if (p[0] != 1 || p[4095] != 2 || p[4096] != 3 || p[8191] != 4) {
        puts("[U][FAIL] mmap payload corrupted");
        return;
    }
    puts("[U][PASS] anonymous mmap");
}

/* Fixed dirent layout emitted by the kernel: 48-byte records, name at +19. */
static u64 rec_u64(const char *p) {
    u64 v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        v |= ((u64)(unsigned char)p[i]) << (i * 8);
    }
    return v;
}

static void dirent_test(void) {
    static const char *want[5] = { "init", "sh", "hello", "cat", "ls" };
    char buf[1920];
    long fd;
    long n;
    long off;
    int i;

    fd = u_open("/bin", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /bin for getdents");
        return;
    }
    n = u_getdents(fd, buf, sizeof(buf));
    u_close(fd);
    if (n <= 0) {
        puts("[U][FAIL] getdents /bin empty");
        return;
    }
    for (i = 0; i < 5; i++) {
        int found = 0;
        for (off = 0; off + 48 <= n; off += 48) {
            const char *name = buf + off + 19;
            const char *w = want[i];
            int k = 0;
            (void)rec_u64;
            while (w[k] != 0 && name[k] == w[k]) {
                k++;
            }
            if (w[k] == 0 && name[k] == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("[U][FAIL] /bin missing entry %s\n", want[i]);
            return;
        }
    }
    puts("[U][PASS] vfs getdents");
}

static void bad_exec_test(void) {
    char *argv[2];
    argv[0] = "/bin/nope";
    argv[1] = 0;
    if (u_execve("/bin/nope", argv, 0) != -1) {
        puts("[U][FAIL] execve missing path should fail");
        return;
    }
    puts("[U][PASS] execve rejects missing image");
}

int main(int argc, char **argv) {
    char *sh_argv[2];
    long pid = u_getpid();
    volatile unsigned long i;

    (void)argc;
    (void)argv;

    printf("[U] hello from ring 3, pid=%d\n", (int)pid);

    /* Busy loop long enough to be preempted by the APIC timer at ring 3. */
    for (i = 0; i < 3000000UL; i++) {
    }
    u_yield();

    cow_round(pid);
    cow_round(pid);

    file_test();
    heap_test();
    mmap_test();
    dirent_test();
    bad_exec_test();

    /* Replace this image with the interactive shell (same pid/fd/cwd). */
    sh_argv[0] = "/bin/sh";
    sh_argv[1] = 0;
    if (u_execve("/bin/sh", sh_argv, 0) != 0) {
        puts("[U][FAIL] execve /bin/sh failed");
        u_exit(127);
    }
    return 0;                       /* unreachable */
}
