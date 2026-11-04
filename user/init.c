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
extern long u_chdir(const char *path);
extern long u_mkdir(const char *path);
extern long u_kill(long pid, long sig);
extern int shm_open(const char *name, int oflag, unsigned int mode);
extern int shm_unlink(const char *name);
extern int ftruncate(int fd, long length);
extern long futex(volatile unsigned int *uaddr, int op, unsigned int val,
                  void *timeout, volatile unsigned int *uaddr2,
                  unsigned int val3);

typedef struct {
    volatile unsigned int state;
} pthread_mutex_t;
extern int pthread_mutex_init(pthread_mutex_t *m, void *attr);
extern int pthread_mutex_lock(pthread_mutex_t *m);
extern int pthread_mutex_unlock(pthread_mutex_t *m);
extern int pthread_mutex_trylock(pthread_mutex_t *m);

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
    long fd;
    if (u_mmap(0, 0, 3, 0x22, -1, 0) != -1) {
        puts("[U][FAIL] mmap len=0 should fail");
        return;
    }
    if (u_mmap(0, 4096, 3, 0x01, -1, 0) != -1) {
        puts("[U][FAIL] anon MAP_SHARED should fail");
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

    /* VMA_FILE demand paging: map /etc/hosts MAP_PRIVATE and touch. */
    fd = u_open("/etc/hosts", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /etc/hosts for file mmap");
        return;
    }
    p = (unsigned char *)u_mmap(0, 4096, 1, 0x02, fd, 0); /* PROT_READ|MAP_PRIVATE */
    if (p == (unsigned char *)-1 || p == 0) {
        puts("[U][FAIL] file MAP_PRIVATE mmap");
        u_close(fd);
        return;
    }
    /* First touch faults in file content; hosts starts with a comment or letter. */
    if (p[0] == 0 && p[1] == 0 && p[2] == 0) {
        puts("[U][FAIL] file mmap demand page empty");
        u_close(fd);
        return;
    }
    puts("[U][PASS] VMA_FILE demand mmap");
    u_close(fd);
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
    /* A regular text file from the rootfs is present but not an ELF. */
    argv[0] = "/etc/motd";
    if (u_execve("/etc/motd", argv, 0) != -1) {
        puts("[U][FAIL] execve non-ELF file should fail");
        return;
    }
    puts("[U][PASS] execve rejects non-ELF file");
}

/* Huge write lengths wrap the end pointer past the address space; the VMA
 * range check must reject them instead of dereferencing kernel memory. */
static void wrap_len_test(void) {
    if (u_write(1, "x", 0xFFFFFFFFFFFFFF00UL) != -1) {
        puts("[U][FAIL] wrapped write length should fail");
        return;
    }
    puts("[U][PASS] huge write length rejected");
}

/* Grandchild orphaning: a middle child forks a grandchild and exits before it;
 * the grandchild must be reparented to pid 1, whose wait4(-1) then reaps it
 * with the original exit code 77. */

/* kill(SIGKILL) a looping child; parent must reap it.
 * Use WNOHANG + bounded yields so a broken kernel cannot wedge init
 * before login (seen on ldfake-linked images with PMM mismatch). */
static void signal_test(void) {
    long child = u_fork();
    long status = 0;
    long reaped = 0;
    int spins;
    if (child == 0) {
        for (;;) {
            u_yield();
        }
    }
    if (u_kill(child, 9) != 0) {
        puts("[U][FAIL] kill(SIGKILL) returned error");
        return;
    }
    for (spins = 0; spins < 100000; spins++) {
        reaped = u_wait4(child, &status, 1, 0); /* WNOHANG */
        if (reaped == child) {
            break;
        }
        if ((spins & 63) == 0) {
            u_yield();
        }
    }
    if (reaped == child && (status >> 8) == (128 + 9)) {
        puts("[U][PASS] kill SIGKILL + wait status");
    } else {
        printf("[U][FAIL] kill/wait status=%d reaped=%d\n", (int)(status >> 8), (int)reaped);
    }
}

static void orphan_test(void) {
    long middle = u_fork();
    if (middle == 0) {
        long gc = u_fork();
        if (gc == 0) {
            volatile unsigned long z;
            for (z = 0; z < 200; z++) {
                u_yield();
            }
            u_exit(77);
        }
        /* Middle exits first while the grandchild is still running. */
        u_exit(0);
    }
    {
        long st = 0;
        long st2 = 0;
        long w;
        long w2;
        w = u_wait4(middle, &st, 0, 0);
        if (w != middle) {
            puts("[U][FAIL] orphan: reap middle child");
            return;
        }
        w2 = u_wait4(-1, &st2, 0, 0);
        if (w2 > 0 && (st2 >> 8) == 77) {
            puts("[U][PASS] orphan reparented to pid 1");
        } else {
            puts("[U][FAIL] orphan reparented to pid 1");
        }
    }
}


static void shm_test(void) {
    int fd;
    unsigned char *p;
    long pid;
    long st;
    /* O_RDWR|O_CREAT */
    fd = shm_open("fakeos-shm", 0x42, 0);
    if (fd < 0) {
        puts("[U][FAIL] shm_open create");
        return;
    }
    if (ftruncate(fd, 4096) != 0) {
        puts("[U][FAIL] ftruncate shm");
        u_close(fd);
        return;
    }
    /* MAP_SHARED file (not ANON): flags 0x01 */
    p = (unsigned char *)u_mmap(0, 4096, 3, 0x01, fd, 0);
    if (p == (unsigned char *)-1 || p == 0) {
        puts("[U][FAIL] shm mmap MAP_SHARED");
        u_close(fd);
        return;
    }
    p[0] = 0xA5;
    p[1] = 0x5A;
    pid = u_fork();
    if (pid < 0) {
        puts("[U][FAIL] shm fork");
        u_close(fd);
        return;
    }
    if (pid == 0) {
        if (p[0] != 0xA5 || p[1] != 0x5A) {
            puts("[U][FAIL] shm child saw stale");
            u_exit(1);
        }
        p[0] = 0x11;
        u_exit(0);
    }
    u_wait4(pid, &st, 0, 0);
    if (p[0] != 0x11) {
        puts("[U][FAIL] shm parent missed child store");
        u_close(fd);
        return;
    }
    u_close(fd);
    if (shm_unlink("fakeos-shm") != 0) {
        puts("[U][FAIL] shm_unlink");
        return;
    }
    /* Name must be gone; reopen without CREAT fails. */
    if (shm_open("fakeos-shm", 2, 0) >= 0) {
        puts("[U][FAIL] shm_unlink left object");
        return;
    }
    puts("[U][PASS] POSIX shm_open/mmap/unlink");
}



static void mutex_test(void) {
    /* Shared page: [0]=mutex.state, [1]=counter, [2]=child_done */
    volatile unsigned int *page;
    pthread_mutex_t *mu;
    volatile unsigned int *counter;
    volatile unsigned int *child_done;
    long pid;
    long st;
    unsigned int i;
    unsigned int n = 2000;

    page = (volatile unsigned int *)u_mmap(0, 4096, 3, 0x21, -1, 0);
    if (page == (volatile unsigned int *)-1 || page == 0) {
        puts("[U][FAIL] mutex mmap shared");
        return;
    }
    mu = (pthread_mutex_t *)page;
    counter = page + 1;
    child_done = page + 2;
    if (pthread_mutex_init(mu, 0) != 0) {
        puts("[U][FAIL] mutex init");
        return;
    }
    *counter = 0;
    *child_done = 0;

    if (pthread_mutex_trylock(mu) != 0) {
        puts("[U][FAIL] mutex trylock free");
        return;
    }
    if (pthread_mutex_trylock(mu) == 0) {
        puts("[U][FAIL] mutex trylock held");
        return;
    }
    pthread_mutex_unlock(mu);

    pid = u_fork();
    if (pid < 0) {
        puts("[U][FAIL] mutex fork");
        return;
    }
    if (pid == 0) {
        for (i = 0; i < n; i++) {
            pthread_mutex_lock(mu);
            *counter = *counter + 1;
            pthread_mutex_unlock(mu);
        }
        *child_done = 1;
        u_exit(0);
    }
    for (i = 0; i < n; i++) {
        pthread_mutex_lock(mu);
        *counter = *counter + 1;
        pthread_mutex_unlock(mu);
    }
    u_wait4(pid, &st, 0, 0);
    if (((st >> 8) & 0xff) != 0) {
        puts("[U][FAIL] mutex child status");
        return;
    }
    if (*child_done != 1) {
        puts("[U][FAIL] mutex child_done");
        return;
    }
    if (*counter != n + n) {
        printf("[U][FAIL] mutex counter=%u want %u\n", *counter, n + n);
        return;
    }
    puts("[U][PASS] pthread_mutex futex lock/unlock");
}

static void futex_test(void) {
    volatile unsigned int *p;
    long pid;
    long st;
    long r;
    unsigned int word;

    word = 1;
    r = futex(&word, 0, 0, 0, 0, 0);
    if (r != -11) {
        puts("[U][FAIL] futex WAIT mismatch");
        return;
    }
    r = futex(&word, 1, 1, 0, 0, 0);
    if (r != 0) {
        puts("[U][FAIL] futex WAKE empty");
        return;
    }

    p = (volatile unsigned int *)u_mmap(0, 4096, 3, 0x21, -1, 0);
    if (p == (volatile unsigned int *)-1 || p == 0) {
        puts("[U][FAIL] futex mmap shared");
        return;
    }
    *p = 1;
    pid = u_fork();
    if (pid < 0) {
        puts("[U][FAIL] futex fork");
        return;
    }
    if (pid == 0) {
        r = futex(p, 0, 1, 0, 0, 0);
        while (r == -4) {
            r = futex(p, 0, 1, 0, 0, 0);
        }
        if (*p != 0) {
            u_exit(1);
        }
        u_exit(0);
    }
    {
        volatile unsigned long i;
        for (i = 0; i < 3000000UL; i++) {
        }
        u_yield();
        u_yield();
    }
    *p = 0;
    futex(p, 1, 1, 0, 0, 0);
    u_wait4(pid, &st, 0, 0);
    if (((st >> 8) & 0xff) != 0) {
        puts("[U][FAIL] futex child status");
        return;
    }
    puts("[U][PASS] futex WAIT/WAKE");
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
    shm_test();
    futex_test();
    mutex_test();
    dirent_test();
    bad_exec_test();
    wrap_len_test();
    orphan_test();
    signal_test();

    /* Prefer persistent Ext2 home (/home aliases /mnt/home) when mounted. */
    u_mkdir("/mnt/home");
    if (u_chdir("/home") == 0) {
        puts("[U] cwd -> /home (persistent Ext2)");
    } else if (u_chdir("/mnt/home") == 0) {
        puts("[U] cwd -> /mnt/home (persistent Ext2)");
    }

    /* getty/login stub then shell (same pid). */
    sh_argv[0] = "/bin/login";
    sh_argv[1] = 0;
    if (u_execve("/bin/login", sh_argv, 0) != 0) {
        puts("[U] login missing, falling back to /bin/sh");
        sh_argv[0] = "/bin/sh";
        if (u_execve("/bin/sh", sh_argv, 0) != 0) {
            puts("[U][FAIL] execve /bin/sh failed");
            u_exit(127);
        }
    }
    return 0;                       /* unreachable */
}
