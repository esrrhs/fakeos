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
extern long u_mprotect(void *addr, unsigned long len, long prot);
extern long u_munmap(void *addr, unsigned long len);
extern long u_getpid(void);
extern long u_getppid(void);
extern long u_uname(void *buf);
extern long u_nanosleep(const void *req, void *rem);
extern long u_clock_gettime(long clockid, void *tp);
extern long u_yield(void);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern void u_exit(long code);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern long u_getdents(long fd, void *buf, unsigned long len);
extern long u_chdir(const char *path);
extern long u_mkdir(const char *path);
extern long u_rmdir(const char *path);
extern long u_getcwd(char *buf, unsigned long len);
extern long u_kill(long pid, long sig);
extern long u_pipe(int *pipefd);
extern long u_unlink(const char *path);
extern long u_link(const char *oldpath, const char *newpath);
extern long u_symlink(const char *target, const char *linkpath);
extern long u_readlink(const char *path, char *buf, unsigned long n);
extern long u_lstat(const char *path, void *statbuf);
extern long u_getuid(void);
extern long u_geteuid(void);
extern long u_getgid(void);
extern long u_getegid(void);
extern long u_chown(const char *path, long uid, long gid);
extern long u_setuid(long uid);
extern long u_setgid(long gid);
extern long u_setreuid(long ruid, long euid);
extern long u_setregid(long rgid, long egid);
extern long u_getgroups(long size, unsigned int *list);
extern long u_setgroups(long size, unsigned int *list);
extern long u_stat(const char *path, void *statbuf);
extern long u_fstat(long fd, void *statbuf);
extern long u_newfstatat(long dfd, const char *path, void *statbuf, long flags);
extern void *signal(int sig, void *handler);
extern int sigaction(int sig, void *act, void *oldact);
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
extern long u_gettid(void);
typedef unsigned long pthread_t;
extern int pthread_create(pthread_t *thread, void *attr, void *(*start_routine)(void *),
                          void *arg);
extern int pthread_join(pthread_t thread, void **retval);
extern void pthread_exit(void *retval);
extern int pthread_detach(pthread_t thread);
extern pthread_t pthread_self(void);
extern long arch_prctl(long code, unsigned long addr);

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

    /* Use /tmp (Ramfs overlay). Unclaimed paths like /test.txt route to Ext2
     * when a disk is mounted; that was masking the virtio-blk TCG stall as a
     * mysterious hang inside "vfs file" before login. */
    fd = u_open("/tmp/test.txt", 0x42, 0);       /* O_RDWR | O_CREAT */
    if (fd < 0) {
        puts("[U][FAIL] open O_CREAT /tmp/test.txt");
        return;
    }
    if (u_write(fd, pat, (unsigned long)pat_len) != pat_len
        || u_write(fd, pat, (unsigned long)pat_len) != pat_len) {
        puts("[U][FAIL] write /tmp/test.txt");
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
    if (u_open("/tmp/no-such-file", 0, 0) != -1) {
        puts("[U][FAIL] open missing /tmp path should fail");
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

    /* File MAP_FIXED: place /etc/hosts at a chosen VA outside the auto window. */
    {
        unsigned char *fp;
        const unsigned long fixed = 0x3E000000UL; /* below MMAP_LIMIT, above auto */
        fd = u_open("/etc/hosts", 0, 0);
        if (fd < 0) {
            puts("[U][FAIL] open /etc/hosts for MAP_FIXED");
            return;
        }
        /* PROT_READ|MAP_PRIVATE|MAP_FIXED = 0x12 */
        fp = (unsigned char *)u_mmap((void *)fixed, 4096, 1, 0x12, fd, 0);
        if (fp != (unsigned char *)fixed) {
            puts("[U][FAIL] file MAP_FIXED mmap");
            u_close(fd);
            return;
        }
        if (fp[0] == 0 && fp[1] == 0 && fp[2] == 0) {
            puts("[U][FAIL] file MAP_FIXED demand page empty");
            u_close(fd);
            return;
        }
        /* Overlap must fail (no silent unmap). */
        if (u_mmap((void *)fixed, 4096, 1, 0x12, fd, 0) != -1) {
            puts("[U][FAIL] overlapping MAP_FIXED should fail");
            u_close(fd);
            return;
        }
        u_close(fd);
        puts("[U][PASS] file MAP_FIXED mmap");
    }

    /* mprotect: RW anon -> RO (read ok) -> RW again (write ok); child write
     * while RO is killed by unhandled #PF (exit code 0xff). */
    {
        unsigned char *mp;
        long pid;
        long st;
        mp = (unsigned char *)u_mmap(0, 4096, 3, 0x22, -1, 0);
        if (mp == (unsigned char *)-1 || mp == 0) {
            puts("[U][FAIL] mprotect anon mmap");
            return;
        }
        mp[0] = 0x42;
        if (u_mprotect(mp, 4096, 1) != 0) { /* PROT_READ */
            puts("[U][FAIL] mprotect PROT_READ");
            return;
        }
        if (mp[0] != 0x42) {
            puts("[U][FAIL] mprotect RO read");
            return;
        }
        pid = u_fork();
        if (pid < 0) {
            puts("[U][FAIL] mprotect fork");
            return;
        }
        if (pid == 0) {
            mp[0] = 0x99; /* must #PF: VMA is RO */
            u_exit(0);
        }
        u_wait4(pid, &st, 0, 0);
        if (((st >> 8) & 0xff) != 0xff) {
            puts("[U][FAIL] mprotect RO write not killed");
            return;
        }
        if (mp[0] != 0x42) {
            puts("[U][FAIL] mprotect parent corrupted");
            return;
        }
        if (u_mprotect(mp, 4096, 3) != 0) { /* PROT_READ|WRITE */
            puts("[U][FAIL] mprotect restore RW");
            return;
        }
        mp[0] = 0x77;
        if (mp[0] != 0x77) {
            puts("[U][FAIL] mprotect RW write");
            return;
        }
        u_munmap(mp, 4096);
        puts("[U][PASS] mprotect RO/RW");
    }

    /* PROT_NONE + non-fixed addr hint (overlap falls back; FIXED still rejects). */
    {
        unsigned char *h;
        unsigned char *g;
        unsigned char *fb;
        long pid;
        long st;
        const unsigned long hint = 0x2A000000UL; /* inside mmap window */

        h = (unsigned char *)u_mmap((void *)hint, 4096, 3, 0x22, -1, 0);
        if (h != (unsigned char *)hint) {
            puts("[U][FAIL] mmap addr hint");
            return;
        }
        h[0] = 0x11;
        /* Same hint while occupied: must fall back (not FIXED replace). */
        fb = (unsigned char *)u_mmap((void *)hint, 4096, 3, 0x22, -1, 0);
        if (fb == (unsigned char *)-1 || fb == 0 || fb == h) {
            puts("[U][FAIL] mmap hint fallback");
            return;
        }
        fb[0] = 0x22;
        if (h[0] != 0x11) {
            puts("[U][FAIL] hint fallback clobbered first map");
            return;
        }

        g = (unsigned char *)u_mmap(0, 4096, 0, 0x22, -1, 0); /* PROT_NONE */
        if (g == (unsigned char *)-1 || g == 0) {
            puts("[U][FAIL] PROT_NONE mmap");
            return;
        }
        pid = u_fork();
        if (pid < 0) {
            puts("[U][FAIL] PROT_NONE fork");
            return;
        }
        if (pid == 0) {
            volatile unsigned char c = g[0];
            (void)c;
            u_exit(0);
        }
        u_wait4(pid, &st, 0, 0);
        if (((st >> 8) & 0xff) != 0xff) {
            puts("[U][FAIL] PROT_NONE read not killed");
            return;
        }
        u_munmap(g, 4096);

        g = (unsigned char *)u_mmap(0, 4096, 3, 0x22, -1, 0);
        if (g == (unsigned char *)-1 || g == 0) {
            puts("[U][FAIL] mprotect NONE setup mmap");
            return;
        }
        g[0] = 0x55;
        if (u_mprotect(g, 4096, 0) != 0) {
            puts("[U][FAIL] mprotect PROT_NONE");
            return;
        }
        pid = u_fork();
        if (pid < 0) {
            puts("[U][FAIL] mprotect NONE fork");
            return;
        }
        if (pid == 0) {
            g[0] = 1;
            u_exit(0);
        }
        u_wait4(pid, &st, 0, 0);
        if (((st >> 8) & 0xff) != 0xff) {
            puts("[U][FAIL] mprotect NONE write not killed");
            return;
        }
        if (u_mprotect(g, 4096, 3) != 0) {
            puts("[U][FAIL] mprotect restore after NONE");
            return;
        }
        if (g[0] != 0x55) {
            puts("[U][FAIL] PROT_NONE lost page data");
            return;
        }
        g[0] = 0x66;
        if (g[0] != 0x66) {
            puts("[U][FAIL] write after restore from NONE");
            return;
        }
        u_munmap(h, 4096);
        u_munmap(fb, 4096);
        u_munmap(g, 4096);
        puts("[U][PASS] PROT_NONE + mmap addr hint");
    }
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

/* ---- signals polish: kill, handler (IRQ frame), stop/cont, pipe EINTR ---- */
enum {
    SIGTERM_U = 15,
    SIGSTOP_U = 19,
    SIGCONT_U = 18,
    WNOHANG_U = 1,
    WUNTRACED_U = 2,
    WCONTINUED_U = 8,
    SA_RESTART_U = 0x10000000
};

static volatile int g_sig_flag;

static void on_sigterm_flag(int sig) {
    (void)sig;
    g_sig_flag = 1;
}

/* kill(SIGKILL) a looping child; parent must reap it.
 * Use WNOHANG + bounded yields so a broken kernel cannot wedge init
 * before login (seen on ldfake-linked images with PMM mismatch). */
static void signal_kill_test(void) {
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

/* Custom SIGTERM handler delivered from timer/IPI trap frame while child
 * spins in user space (no syscall). */
static void signal_handler_test(void) {
    long child;
    long status = 0;
    long reaped = 0;
    int spins;
    child = u_fork();
    if (child == 0) {
        g_sig_flag = 0;
        if (signal(SIGTERM_U, (void *)on_sigterm_flag) == (void *)(long)-1) {
            puts("[U][FAIL] signal(SIGTERM) install");
            u_exit(1);
        }
        for (spins = 0; spins < 2000000; spins++) {
            if (g_sig_flag) {
                u_exit(0);
            }
            if ((spins & 63) == 0) {
                u_yield();
            }
        }
        puts("[U][FAIL] SIGTERM handler never ran");
        u_exit(2);
    }
    for (spins = 0; spins < 200; spins++) {
        u_yield();
    }
    if (u_kill(child, SIGTERM_U) != 0) {
        puts("[U][FAIL] kill(SIGTERM) for handler");
        return;
    }
    for (spins = 0; spins < 100000; spins++) {
        reaped = u_wait4(child, &status, WNOHANG_U, 0);
        if (reaped == child) {
            break;
        }
        if ((spins & 63) == 0) {
            u_yield();
        }
    }
    if (reaped == child && (status >> 8) == 0) {
        puts("[U][PASS] SIGTERM handler + sigreturn");
    } else {
        printf("[U][FAIL] SIGTERM handler status=%d reaped=%d\n",
               (int)(status >> 8), (int)reaped);
    }
}

/* SIGSTOP / wait WUNTRACED / SIGCONT / wait WCONTINUED / SIGKILL. */
static void signal_stop_cont_test(void) {
    long child;
    long status = 0;
    long reaped = 0;
    int spins;
    child = u_fork();
    if (child == 0) {
        for (;;) {
            u_yield();
        }
    }
    for (spins = 0; spins < 50; spins++) {
        u_yield();
    }
    if (u_kill(child, SIGSTOP_U) != 0) {
        puts("[U][FAIL] kill(SIGSTOP)");
        u_kill(child, 9);
        return;
    }
    for (spins = 0; spins < 100000; spins++) {
        reaped = u_wait4(child, &status, WNOHANG_U | WUNTRACED_U, 0);
        if (reaped == child) {
            break;
        }
        if ((spins & 63) == 0) {
            u_yield();
        }
    }
    if (reaped != child || (status & 0xff) != 0x7f
        || ((status >> 8) & 0xff) != SIGSTOP_U) {
        printf("[U][FAIL] WUNTRACED status=%d reaped=%d\n",
               (int)status, (int)reaped);
        u_kill(child, 9);
        u_wait4(child, &status, 0, 0);
        return;
    }
    if (u_kill(child, SIGCONT_U) != 0) {
        puts("[U][FAIL] kill(SIGCONT)");
        u_kill(child, 9);
        return;
    }
    reaped = 0;
    status = 0;
    for (spins = 0; spins < 100000; spins++) {
        reaped = u_wait4(child, &status, WNOHANG_U | WCONTINUED_U, 0);
        if (reaped == child) {
            break;
        }
        if ((spins & 63) == 0) {
            u_yield();
        }
    }
    if (reaped != child || (status & 0xffff) != 0xffff) {
        printf("[U][FAIL] WCONTINUED status=%d reaped=%d\n",
               (int)status, (int)reaped);
        u_kill(child, 9);
        u_wait4(child, &status, 0, 0);
        return;
    }
    u_kill(child, 9);
    u_wait4(child, &status, 0, 0);
    puts("[U][PASS] SIGSTOP/CONT + WUNTRACED/WCONTINUED");
}

/* Blocking pipe read interrupted by SIGTERM without SA_RESTART -> EINTR. */
static void signal_pipe_eintr_test(void) {
    int fds[2];
    long child;
    long status = 0;
    long reaped;
    int spins;
    char b;
    if (u_pipe(fds) < 0) {
        puts("[U][FAIL] pipe for EINTR");
        return;
    }
    child = u_fork();
    if (child == 0) {
        long n;
        u_close(fds[1]);
        g_sig_flag = 0;
        if (signal(SIGTERM_U, (void *)on_sigterm_flag) == (void *)(long)-1) {
            u_exit(1);
        }
        n = u_read(fds[0], &b, 1);
        /* After sigreturn, rax is restored to EINTR (-4). */
        if (n == -4 && g_sig_flag) {
            u_exit(0);
        }
        printf("[U][FAIL] pipe EINTR n=%d flag=%d\n", (int)n, g_sig_flag);
        u_exit(2);
    }
    u_close(fds[0]);
    for (spins = 0; spins < 300; spins++) {
        u_yield();
    }
    if (u_kill(child, SIGTERM_U) != 0) {
        puts("[U][FAIL] kill for pipe EINTR");
        u_close(fds[1]);
        return;
    }
    reaped = u_wait4(child, &status, 0, 0);
    u_close(fds[1]);
    if (reaped == child && (status >> 8) == 0) {
        puts("[U][PASS] pipe read EINTR + handler");
    } else {
        printf("[U][FAIL] pipe EINTR status=%d reaped=%d\n",
               (int)(status >> 8), (int)reaped);
    }
}

/* SA_RESTART: signal while blocked on pipe keeps the read; byte still arrives. */
static void signal_pipe_restart_test(void) {
    int fds[2];
    long child;
    long status = 0;
    long reaped;
    int spins;
    struct {
        void *sa_handler;
        unsigned long sa_flags;
        void *sa_restorer;
        unsigned long sa_mask;
    } act;
    if (u_pipe(fds) < 0) {
        puts("[U][FAIL] pipe for SA_RESTART");
        return;
    }
    child = u_fork();
    if (child == 0) {
        char b;
        long n;
        u_close(fds[1]);
        g_sig_flag = 0;
        act.sa_handler = (void *)on_sigterm_flag;
        act.sa_flags = SA_RESTART_U;
        act.sa_restorer = 0;
        act.sa_mask = 0;
        if (sigaction(SIGTERM_U, &act, 0) != 0) {
            u_exit(1);
        }
        n = u_read(fds[0], &b, 1);
        if (n == 1 && b == 'R' && g_sig_flag) {
            u_exit(0);
        }
        printf("[U][FAIL] SA_RESTART n=%d b=%d flag=%d\n",
               (int)n, (int)b, g_sig_flag);
        u_exit(2);
    }
    u_close(fds[0]);
    for (spins = 0; spins < 300; spins++) {
        u_yield();
    }
    if (u_kill(child, SIGTERM_U) != 0) {
        puts("[U][FAIL] kill for SA_RESTART");
        u_close(fds[1]);
        return;
    }
    /* Give the child time to observe the signal and re-sleep, then complete. */
    for (spins = 0; spins < 200; spins++) {
        u_yield();
    }
    {
        char wb = 'R';
        if (u_write(fds[1], &wb, 1) != 1) {
            puts("[U][FAIL] write for SA_RESTART");
        }
    }
    reaped = u_wait4(child, &status, 0, 0);
    u_close(fds[1]);
    if (reaped == child && (status >> 8) == 0) {
        puts("[U][PASS] pipe SA_RESTART keeps read");
    } else {
        printf("[U][FAIL] SA_RESTART status=%d reaped=%d\n",
               (int)(status >> 8), (int)reaped);
    }
}

static void signal_test(void) {
    signal_kill_test();
    signal_handler_test();
    signal_stop_cont_test();
    signal_pipe_eintr_test();
    signal_pipe_restart_test();
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



static int proc_has(const char *buf, long n, const char *needle) {
    long i;
    long m = 0;
    while (needle[m] != 0) {
        m++;
    }
    if (m == 0 || n < m) {
        return 0;
    }
    for (i = 0; i + m <= n; i++) {
        long k;
        for (k = 0; k < m; k++) {
            if (buf[i + k] != needle[k]) {
                break;
            }
        }
        if (k == m) {
            return 1;
        }
    }
    return 0;
}


static void posix_basics_test(void) {
    char uts[390];
    long ts[2];
    long t0[2];
    long t1[2];
    long i;
    long pid;
    long child;
    long status;
    long reaped;

    /* getppid: init's parent is 0 (kernel). */
    if (u_getppid() != 0) {
        printf("[U][FAIL] getppid init=%d\n", (int)u_getppid());
        return;
    }

    pid = u_getpid();
    child = u_fork();
    if (child < 0) {
        puts("[U][FAIL] posix_basics fork");
        return;
    }
    if (child == 0) {
        if (u_getppid() != pid) {
            u_exit(11);
        }
        u_exit(0);
    }
    status = 0;
    reaped = u_wait4(child, &status, 0, 0);
    if (reaped != child || (status >> 8) != 0) {
        printf("[U][FAIL] getppid child status=%d\n", (int)(status >> 8));
        return;
    }

    for (i = 0; i < 390; i++) {
        uts[i] = 0;
    }
    if (u_uname(uts) != 0) {
        puts("[U][FAIL] uname");
        return;
    }
    if (uts[0] != 'f' || uts[1] != 'a' || uts[2] != 'k' || uts[3] != 'e'
        || uts[4] != 'o' || uts[5] != 's' || uts[6] != 0) {
        puts("[U][FAIL] uname sysname");
        return;
    }
    if (uts[260] != 'x' || uts[261] != '8' || uts[262] != '6'
        || uts[263] != '_' || uts[264] != '6' || uts[265] != '4'
        || uts[266] != 0) {
        puts("[U][FAIL] uname machine");
        return;
    }

    /* Invalid timespec nsec */
    ts[0] = 0;
    ts[1] = 1000000000;
    if (u_nanosleep(ts, 0) != -22) {
        puts("[U][FAIL] nanosleep EINVAL");
        return;
    }

    if (u_clock_gettime(0, t0) != 0 || u_clock_gettime(1, t0) != 0) {
        puts("[U][FAIL] clock_gettime");
        return;
    }
    /* Sleep ~50ms (5 ticks @ 100 Hz). */
    ts[0] = 0;
    ts[1] = 50000000;
    if (u_nanosleep(ts, 0) != 0) {
        puts("[U][FAIL] nanosleep 50ms");
        return;
    }
    if (u_clock_gettime(1, t1) != 0) {
        puts("[U][FAIL] clock_gettime after sleep");
        return;
    }
    /* Monotonic must not go backwards; expect some forward progress. */
    if (t1[0] < t0[0] || (t1[0] == t0[0] && t1[1] < t0[1])) {
        puts("[U][FAIL] clock_gettime went backwards");
        return;
    }
    if (t1[0] == t0[0] && t1[1] == t0[1]) {
        /* Allow equal only if tick did not advance — still require sleep OK. */
        /* Prefer seeing progress; retry one more short sleep. */
        ts[0] = 0;
        ts[1] = 100000000;
        u_nanosleep(ts, 0);
        u_clock_gettime(1, t1);
        if (t1[0] == t0[0] && t1[1] == t0[1]) {
            puts("[U][FAIL] clock_gettime no progress after sleep");
            return;
        }
    }

    if (u_clock_gettime(99, t0) != -22) {
        puts("[U][FAIL] clock_gettime bad id");
        return;
    }

    puts("[U][PASS] getppid/uname/nanosleep/clock_gettime");
}

static void proc_self_test(void) {
    long fd;
    long n;
    char buf[1024];
    char name_line[16];

    fd = u_open("/proc/self/status", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /proc/self/status");
        return;
    }
    n = u_read(fd, buf, 1023);
    u_close(fd);
    if (n <= 0) {
        puts("[U][FAIL] read /proc/self/status");
        return;
    }
    if (!proc_has(buf, n, "Name:") || !proc_has(buf, n, "Pid:")
        || !proc_has(buf, n, "PPid:") || !proc_has(buf, n, "VmSize:")) {
        puts("[U][FAIL] /proc/self/status fields");
        return;
    }
    /* Name:<tab>init */
    name_line[0] = 'N'; name_line[1] = 'a'; name_line[2] = 'm';
    name_line[3] = 'e'; name_line[4] = ':'; name_line[5] = '\t';
    name_line[6] = 'i'; name_line[7] = 'n'; name_line[8] = 'i';
    name_line[9] = 't'; name_line[10] = 0;
    if (!proc_has(buf, n, name_line)) {
        puts("[U][FAIL] /proc/self/status Name not init");
        return;
    }

    fd = u_open("/proc/self/maps", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /proc/self/maps");
        return;
    }
    n = u_read(fd, buf, 1023);
    u_close(fd);
    if (n <= 0) {
        puts("[U][FAIL] read /proc/self/maps");
        return;
    }
    if (!proc_has(buf, n, "[stack]")) {
        puts("[U][FAIL] /proc/self/maps missing [stack]");
        return;
    }

    fd = u_open("/proc/self/cmdline", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /proc/self/cmdline");
        return;
    }
    n = u_read(fd, buf, 1023);
    u_close(fd);
    if (n < 9 || !proc_has(buf, n, "/bin/init")) {
        puts("[U][FAIL] /proc/self/cmdline not /bin/init");
        return;
    }

    fd = u_open("/proc/self/environ", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /proc/self/environ");
        return;
    }
    n = u_read(fd, buf, 1023);
    u_close(fd);
    if (n <= 0 || !proc_has(buf, n, "FAKECC_PKG=/src")) {
        puts("[U][FAIL] /proc/self/environ");
        return;
    }

    if (u_unlink("/proc/self/status") == 0) {
        puts("[U][FAIL] unlink /proc/self/status should fail");
        return;
    }

    puts("[U][PASS] /proc/self status/maps/cmdline");
}

static void stat_test(void) {
    char st[144];
    long fd;
    long n;
    unsigned int mode;
    unsigned long size;
    unsigned long ino;
    int pipes[2];
    unsigned long i;

    /* Zero the buffer so stale fields fail loudly. */
    for (i = 0; i < 144; i++) {
        st[i] = (char)0xA5;
    }

    /* stat("/etc/hosts"): regular file with non-zero size. */
    if (u_stat("/etc/hosts", st) != 0) {
        puts("[U][FAIL] stat /etc/hosts");
        return;
    }
    mode = (unsigned int)(unsigned char)st[24]
         | ((unsigned int)(unsigned char)st[25] << 8)
         | ((unsigned int)(unsigned char)st[26] << 16)
         | ((unsigned int)(unsigned char)st[27] << 24);
    size = 0;
    for (i = 0; i < 8; i++) {
        size |= ((unsigned long)(unsigned char)st[48 + i]) << (8 * i);
    }
    ino = 0;
    for (i = 0; i < 8; i++) {
        ino |= ((unsigned long)(unsigned char)st[8 + i]) << (8 * i);
    }
    if ((mode & 0xF000) != 0x8000 || size == 0 || ino == 0) {
        puts("[U][FAIL] stat /etc/hosts fields");
        return;
    }

    /* fstat on the same file. */
    fd = u_open("/etc/hosts", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /etc/hosts for fstat");
        return;
    }
    if (u_fstat(fd, st) != 0) {
        puts("[U][FAIL] fstat /etc/hosts");
        u_close(fd);
        return;
    }
    u_close(fd);

    /* Directory: S_IFDIR. */
    if (u_stat("/bin", st) != 0) {
        puts("[U][FAIL] stat /bin");
        return;
    }
    mode = (unsigned int)(unsigned char)st[24]
         | ((unsigned int)(unsigned char)st[25] << 8)
         | ((unsigned int)(unsigned char)st[26] << 16)
         | ((unsigned int)(unsigned char)st[27] << 24);
    if ((mode & 0xF000) != 0x4000) {
        puts("[U][FAIL] stat /bin not dir");
        return;
    }

    /* Char device /dev/null. */
    if (u_stat("/dev/null", st) != 0) {
        puts("[U][FAIL] stat /dev/null");
        return;
    }
    mode = (unsigned int)(unsigned char)st[24]
         | ((unsigned int)(unsigned char)st[25] << 8)
         | ((unsigned int)(unsigned char)st[26] << 16)
         | ((unsigned int)(unsigned char)st[27] << 24);
    if ((mode & 0xF000) != 0x2000) {
        puts("[U][FAIL] stat /dev/null not chr");
        return;
    }

    /* newfstatat(dirfd, name): open /etc and stat hosts via dfd. */
    fd = u_open("/etc", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /etc for fstatat");
        return;
    }
    if (u_newfstatat(fd, "hosts", st, 0) != 0) {
        puts("[U][FAIL] newfstatat /etc hosts");
        u_close(fd);
        return;
    }
    mode = (unsigned int)(unsigned char)st[24]
         | ((unsigned int)(unsigned char)st[25] << 8)
         | ((unsigned int)(unsigned char)st[26] << 16)
         | ((unsigned int)(unsigned char)st[27] << 24);
    size = 0;
    for (i = 0; i < 8; i++) {
        size |= ((unsigned long)(unsigned char)st[48 + i]) << (8 * i);
    }
    if ((mode & 0xF000) != 0x8000 || size == 0) {
        puts("[U][FAIL] newfstatat hosts fields");
        u_close(fd);
        return;
    }
    /* AT_EMPTY_PATH on the dirfd itself. */
    if (u_newfstatat(fd, "", st, 0x1000) != 0) {
        puts("[U][FAIL] newfstatat AT_EMPTY_PATH");
        u_close(fd);
        return;
    }
    mode = (unsigned int)(unsigned char)st[24]
         | ((unsigned int)(unsigned char)st[25] << 8)
         | ((unsigned int)(unsigned char)st[26] << 16)
         | ((unsigned int)(unsigned char)st[27] << 24);
    if ((mode & 0xF000) != 0x4000) {
        puts("[U][FAIL] AT_EMPTY_PATH not dir");
        u_close(fd);
        return;
    }
    u_close(fd);

    /* Missing path fails. */
    if (u_stat("/no/such/stat/path", st) == 0) {
        puts("[U][FAIL] stat missing should fail");
        return;
    }

    /* Pipe: S_IFIFO via fstat. */
    if (u_pipe(pipes) != 0) {
        puts("[U][FAIL] pipe for fstat");
        return;
    }
    if (u_fstat(pipes[0], st) != 0) {
        puts("[U][FAIL] fstat pipe");
        u_close(pipes[0]);
        u_close(pipes[1]);
        return;
    }
    mode = (unsigned int)(unsigned char)st[24]
         | ((unsigned int)(unsigned char)st[25] << 8)
         | ((unsigned int)(unsigned char)st[26] << 16)
         | ((unsigned int)(unsigned char)st[27] << 24);
    u_close(pipes[0]);
    u_close(pipes[1]);
    if ((mode & 0xF000) != 0x1000) {
        puts("[U][FAIL] fstat pipe not fifo");
        return;
    }

    puts("[U][PASS] stat/fstat/newfstatat");
}

static void dev_nodes_test(void) {
    long fd;
    long n;
    char buf[64];
    unsigned long i;

    /* /dev/null: write discards, read returns EOF. */
    fd = u_open("/dev/null", 2, 0); /* O_RDWR */
    if (fd < 0) {
        puts("[U][FAIL] open /dev/null");
        return;
    }
    if (u_write(fd, "discard-me", 10) != 10) {
        puts("[U][FAIL] write /dev/null");
        u_close(fd);
        return;
    }
    n = u_read(fd, buf, 16);
    if (n != 0) {
        puts("[U][FAIL] read /dev/null not EOF");
        u_close(fd);
        return;
    }
    u_close(fd);

    /* Shell-style O_WRONLY|O_CREAT|O_TRUNC redirect must work on /dev/null. */
    fd = u_open("/dev/null", 0x241, 0x1A4); /* WRONLY|CREAT|TRUNC */
    if (fd < 0) {
        puts("[U][FAIL] open /dev/null redirect flags");
        return;
    }
    if (u_write(fd, "x", 1) != 1) {
        puts("[U][FAIL] redirect write /dev/null");
        u_close(fd);
        return;
    }
    u_close(fd);

    /* /dev/zero: reads zeros; writes discard. */
    fd = u_open("/dev/zero", 2, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /dev/zero");
        return;
    }
    for (i = 0; i < 64; i++) {
        buf[i] = (char)0xA5;
    }
    n = u_read(fd, buf, 64);
    if (n != 64) {
        puts("[U][FAIL] read /dev/zero length");
        u_close(fd);
        return;
    }
    for (i = 0; i < 64; i++) {
        if (buf[i] != 0) {
            puts("[U][FAIL] read /dev/zero not zero");
            u_close(fd);
            return;
        }
    }
    if (u_write(fd, "noise", 5) != 5) {
        puts("[U][FAIL] write /dev/zero");
        u_close(fd);
        return;
    }
    u_close(fd);

    /* /dev/tty: path-openable console alias. */
    fd = u_open("/dev/tty", 1, 0); /* O_WRONLY */
    if (fd < 0) {
        puts("[U][FAIL] open /dev/tty");
        return;
    }
    if (u_write(fd, "[U] tty ok\n", 10) != 10) {
        puts("[U][FAIL] write /dev/tty");
        u_close(fd);
        return;
    }
    u_close(fd);

    /* Device nodes must not be unlinkable. */
    if (u_unlink("/dev/null") == 0) {
        puts("[U][FAIL] unlink /dev/null should fail");
        return;
    }
    puts("[U][PASS] /dev/null /dev/zero /dev/tty");
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


static volatile int g_thr_counter;
static volatile int g_thr_child_tid;
static volatile int g_thr_self_ok;
static volatile int g_det_flag;
static pthread_mutex_t g_thr_mu;

static void *thr_worker(void *arg) {
    int n;
    int i;
    unsigned long fs;
    pthread_t self;
    n = (int)(long)arg;
    g_thr_child_tid = (int)u_gettid();
    self = pthread_self();
    if (self == (pthread_t)g_thr_child_tid
        && arch_prctl(0x1003, (unsigned long)&fs) == 0 && fs != 0) {
        g_thr_self_ok = 1;
    }
    for (i = 0; i < n; i++) {
        pthread_mutex_lock(&g_thr_mu);
        g_thr_counter = g_thr_counter + 1;
        pthread_mutex_unlock(&g_thr_mu);
        if ((i & 0xFF) == 0) {
            u_yield();
        }
    }
    return (void *)(long)0x55AA;
}

static void *thr_exit_worker(void *arg) {
    (void)arg;
    pthread_exit((void *)(long)0xDEAD);
    return (void *)(long)0; /* unreachable */
}

static void *thr_detach_worker(void *arg) {
    (void)arg;
    g_det_flag = 1;
    u_yield();
    return (void *)(long)0xBEEF;
}

/* Bypass pthread_finish user wake: join must rely on kernel CLEARTID. */
static void *thr_raw_exit_worker(void *arg) {
    (void)arg;
    u_exit(0);
    return (void *)(long)0;
}

static void pthread_create_test(void) {
    pthread_t th;
    pthread_t th2;
    long parent_tid;
    long parent_pid;
    int i;
    int n = 1500;
    int spins;
    void *joined;

    parent_pid = u_getpid();
    parent_tid = u_gettid();
    if (parent_tid != parent_pid) {
        puts("[U][FAIL] leader tid != pid");
        return;
    }
    if (pthread_mutex_init(&g_thr_mu, 0) != 0) {
        puts("[U][FAIL] thr mutex init");
        return;
    }
    g_thr_counter = 0;
    g_thr_child_tid = 0;
    g_thr_self_ok = 0;
    joined = 0;

    if (pthread_create(&th, 0, thr_worker, (void *)(long)n) != 0) {
        puts("[U][FAIL] pthread_create");
        return;
    }
    for (i = 0; i < n; i++) {
        pthread_mutex_lock(&g_thr_mu);
        g_thr_counter = g_thr_counter + 1;
        pthread_mutex_unlock(&g_thr_mu);
    }
    if (pthread_join(th, &joined) != 0) {
        puts("[U][FAIL] pthread_join");
        return;
    }
    if (joined != (void *)(long)0x55AA) {
        printf("[U][FAIL] join retval=%p\n", joined);
        return;
    }
    if (g_thr_child_tid == 0 || g_thr_child_tid == (int)parent_tid) {
        printf("[U][FAIL] child tid=%d parent=%d\n", g_thr_child_tid, (int)parent_tid);
        return;
    }
    if ((pthread_t)g_thr_child_tid != th) {
        printf("[U][FAIL] join tid mismatch child=%d th=%d\n",
               g_thr_child_tid, (int)th);
        return;
    }
    if (g_thr_counter != n + n) {
        printf("[U][FAIL] thr counter=%d want %d\n", g_thr_counter, n + n);
        return;
    }
    if (g_thr_self_ok != 1) {
        puts("[U][FAIL] pthread_self / FS TLS");
        return;
    }
    puts("[U][PASS] pthread_create clone same-AS");
    puts("[U][PASS] pthread_join + FS TLS");

    /* pthread_exit publishes retval through TLS owner -> join. */
    joined = 0;
    if (pthread_create(&th, 0, thr_exit_worker, 0) != 0) {
        puts("[U][FAIL] pthread_create (exit)");
        return;
    }
    if (pthread_join(th, &joined) != 0) {
        puts("[U][FAIL] pthread_join (exit)");
        return;
    }
    if (joined != (void *)(long)0xDEAD) {
        printf("[U][FAIL] pthread_exit retval=%p\n", joined);
        return;
    }
    puts("[U][PASS] pthread_exit retval");

    /* detach: join must fail; worker runs to completion; later create reclaims. */
    g_det_flag = 0;
    if (pthread_create(&th, 0, thr_detach_worker, 0) != 0) {
        puts("[U][FAIL] pthread_create (detach)");
        return;
    }
    if (pthread_detach(th) != 0) {
        puts("[U][FAIL] pthread_detach");
        return;
    }
    if (pthread_join(th, &joined) == 0) {
        puts("[U][FAIL] join after detach should fail");
        return;
    }
    spins = 0;
    while (g_det_flag == 0 && spins < 100000) {
        u_yield();
        spins = spins + 1;
    }
    if (g_det_flag != 1) {
        puts("[U][FAIL] detach worker did not run");
        return;
    }
    for (i = 0; i < 8; i++) {
        u_yield();
    }
    /* Second joinable thread forces deferred stack reclaim path. */
    if (pthread_create(&th2, 0, thr_exit_worker, 0) != 0) {
        puts("[U][FAIL] pthread_create (reclaim)");
        return;
    }
    joined = 0;
    if (pthread_join(th2, &joined) != 0 || joined != (void *)(long)0xDEAD) {
        puts("[U][FAIL] reclaim join");
        return;
    }
    puts("[U][PASS] pthread_detach + stack reclaim");

    /* Raw u_exit: no libc futex wake; kernel CLEARTID must unblock join. */
    joined = (void *)(long)0x1111;
    if (pthread_create(&th, 0, thr_raw_exit_worker, 0) != 0) {
        puts("[U][FAIL] pthread_create (cleartid)");
        return;
    }
    if (pthread_join(th, &joined) != 0) {
        puts("[U][FAIL] pthread_join CLEARTID");
        return;
    }
    puts("[U][PASS] pthread_join CLEARTID");
}



static unsigned int st_u32(const char *st, unsigned int off) {
    return (unsigned int)(unsigned char)st[off]
         | ((unsigned int)(unsigned char)st[off + 1] << 8)
         | ((unsigned int)(unsigned char)st[off + 2] << 16)
         | ((unsigned int)(unsigned char)st[off + 3] << 24);
}

static unsigned long st_u64(const char *st, unsigned int off) {
    unsigned long v = 0;
    unsigned long i;
    for (i = 0; i < 8; i++) {
        v |= ((unsigned long)(unsigned char)st[off + i]) << (8 * i);
    }
    return v;
}


/* setuid/seteuid + supplementary groups. */
static void cred_setuid_groups_test(void) {
    unsigned int gids[8];
    unsigned int want[3];
    long ng;
    long fd;
    unsigned int st_uid;
    char st[144];
    unsigned long i;

    if (u_getuid() != 0 || u_geteuid() != 0) {
        puts("[U][FAIL] cred test needs root");
        return;
    }
    want[0] = 10;
    want[1] = 100;
    want[2] = 1000;
    if (u_setgroups(3, want) != 0) {
        puts("[U][FAIL] setgroups");
        return;
    }
    ng = u_getgroups(8, gids);
    if (ng != 3 || gids[0] != 10 || gids[1] != 100 || gids[2] != 1000) {
        puts("[U][FAIL] getgroups roundtrip");
        return;
    }
    if (u_getgroups(0, 0) != 3) {
        puts("[U][FAIL] getgroups size=0 count");
        return;
    }
    /* seteuid via setreuid(-1, e): drop effective, keep real. */
    if (u_setreuid((long)(-1), 1000) != 0) {
        puts("[U][FAIL] seteuid 1000");
        return;
    }
    if (u_getuid() != 0 || u_geteuid() != 1000) {
        puts("[U][FAIL] after seteuid r/e");
        return;
    }
    /* Non-root cannot setgroups. */
    if (u_setgroups(1, want) == 0) {
        puts("[U][FAIL] setgroups as non-root should fail");
        return;
    }
    /* Non-root cannot seteuid to foreign uid. */
    if (u_setreuid((long)(-1), 7) == 0) {
        puts("[U][FAIL] seteuid foreign should fail");
        return;
    }
    /* Restore euid with setreuid to ruid. */
    if (u_setreuid((long)(-1), 0) != 0) {
        puts("[U][FAIL] restore euid");
        return;
    }
    if (u_geteuid() != 0) {
        puts("[U][FAIL] euid not restored");
        return;
    }
    /* setgid + setuid drop for create stamp via euid. */
    if (u_setgid(1000) != 0 || u_setreuid((long)(-1), 1000) != 0) {
        puts("[U][FAIL] prep euid create");
        return;
    }
    fd = u_open("/tmp/cred_euid.txt", 0x40 | 0x200 | 2, 0x1A4);
    if (fd < 0) {
        puts("[U][FAIL] open cred_euid create");
        return;
    }
    u_close(fd);
    for (i = 0; i < 144; i++) st[i] = 0;
    if (u_stat("/tmp/cred_euid.txt", st) != 0) {
        puts("[U][FAIL] stat cred_euid");
        return;
    }
    st_uid = (unsigned int)(unsigned char)st[28]
           | ((unsigned int)(unsigned char)st[29] << 8)
           | ((unsigned int)(unsigned char)st[30] << 16)
           | ((unsigned int)(unsigned char)st[31] << 24);
    if (st_uid != 1000) {
        puts("[U][FAIL] create owner not euid");
        return;
    }
    /* Full setuid drop + restore (init must stay root for later). */
    if (u_setuid(0) != 0) {
        puts("[U][FAIL] setuid restore root");
        return;
    }
    if (u_setgid(0) != 0) {
        puts("[U][FAIL] setgid restore root");
        return;
    }
    if (u_setgroups(0, 0) != 0) {
        puts("[U][FAIL] clear groups");
        return;
    }
    if (u_getuid() != 0 || u_geteuid() != 0 || u_getgid() != 0 || u_getegid() != 0) {
        puts("[U][FAIL] creds not root at end");
        return;
    }
    puts("[U][PASS] setuid/seteuid/groups");
}


/* getuid/chown stubs + mtime stamped into struct stat. */
static void owner_mtime_test(void) {
    char st[144];
    long fd;
    unsigned int uid;
    unsigned int gid;
    unsigned long m0;
    unsigned long m1;
    unsigned long i;

    if (u_getuid() != 0 || u_geteuid() != 0 || u_getgid() != 0 || u_getegid() != 0) {
        puts("[U][FAIL] getuid/getgid not root");
        return;
    }
    fd = u_open("/tmp/own_mtime.txt", 0x40 | 0x200 | 2, 0x1A4); /* CREAT|TRUNC|RDWR 0644 */
    if (fd < 0) {
        puts("[U][FAIL] open own_mtime create");
        return;
    }
    if (u_write(fd, "hi", 2) != 2) {
        puts("[U][FAIL] write own_mtime");
        u_close(fd);
        return;
    }
    u_close(fd);
    for (i = 0; i < 144; i++) st[i] = 0;
    if (u_stat("/tmp/own_mtime.txt", st) != 0) {
        puts("[U][FAIL] stat own_mtime");
        return;
    }
    uid = st_u32(st, 28);
    gid = st_u32(st, 32);
    m0 = st_u64(st, 88);
    if (uid != 0 || gid != 0) {
        puts("[U][FAIL] create owner not root");
        return;
    }
    if (u_chown("/tmp/own_mtime.txt", 1, 2) != 0) {
        puts("[U][FAIL] chown 1:2");
        return;
    }
    for (i = 0; i < 144; i++) st[i] = 0;
    if (u_stat("/tmp/own_mtime.txt", st) != 0) {
        puts("[U][FAIL] stat after chown");
        return;
    }
    uid = st_u32(st, 28);
    gid = st_u32(st, 32);
    m1 = st_u64(st, 88);
    if (uid != 1 || gid != 2) {
        puts("[U][FAIL] chown fields");
        return;
    }
    if (m1 < m0) {
        puts("[U][FAIL] mtime went backwards");
        return;
    }
    /* Ext2 path when /home is live. */
    fd = u_open("/home/own_e2.txt", 0x40 | 0x200 | 2, 0x1A4);
    if (fd >= 0) {
        if (u_write(fd, "e2", 2) != 2) {
            puts("[U][FAIL] write own_e2");
            u_close(fd);
            return;
        }
        u_close(fd);
        if (u_chown("/home/own_e2.txt", 3, 4) != 0) {
            puts("[U][FAIL] chown ext2");
            return;
        }
        for (i = 0; i < 144; i++) st[i] = 0;
        if (u_stat("/home/own_e2.txt", st) != 0) {
            puts("[U][FAIL] stat own_e2");
            return;
        }
        if (st_u32(st, 28) != 3 || st_u32(st, 32) != 4) {
            puts("[U][FAIL] ext2 chown fields");
            return;
        }
        puts("[U] Ext2 chown/mtime OK");
    }
    puts("[U][PASS] getuid/chown/mtime");
}

static void symlink_test(void) {
    char st[144];
    char buf[128];
    long n;
    long fd;
    unsigned int mode;
    unsigned long i;
    unsigned long size;

    u_unlink("/tmp/sym_tgt.txt");
    u_unlink("/tmp/sym_link");
    u_unlink("/tmp/sym_rel");
    u_unlink("/tmp/sym_dirlink");

    fd = u_open("/tmp/sym_tgt.txt", 0x241, 0x1A4); /* WRONLY|CREAT|TRUNC */
    if (fd < 0) {
        puts("[U][FAIL] symlink create target");
        return;
    }
    if (u_write(fd, "symlink-payload", 15) != 15) {
        puts("[U][FAIL] symlink write target");
        u_close(fd);
        return;
    }
    u_close(fd);

    if (u_symlink("/tmp/sym_tgt.txt", "/tmp/sym_link") != 0) {
        puts("[U][FAIL] symlink absolute");
        return;
    }
    n = u_readlink("/tmp/sym_link", buf, sizeof(buf) - 1);
    if (n < 0) {
        puts("[U][FAIL] readlink absolute");
        return;
    }
    buf[n] = 0;
    /* target is "/tmp/sym_tgt.txt" (16 bytes). */
    if (n != 16 || buf[0] != '/' || buf[5] != 's' || buf[9] != 't'
        || buf[15] != 't') {
        puts("[U][FAIL] readlink absolute value");
        return;
    }

    if (u_lstat("/tmp/sym_link", st) != 0) {
        puts("[U][FAIL] lstat symlink");
        return;
    }
    mode = (unsigned int)(unsigned char)st[24]
         | ((unsigned int)(unsigned char)st[25] << 8)
         | ((unsigned int)(unsigned char)st[26] << 16)
         | ((unsigned int)(unsigned char)st[27] << 24);
    if ((mode & 0xF000) != 0xA000) {
        puts("[U][FAIL] lstat not S_IFLNK");
        return;
    }
    if (u_stat("/tmp/sym_link", st) != 0) {
        puts("[U][FAIL] stat follow symlink");
        return;
    }
    mode = (unsigned int)(unsigned char)st[24]
         | ((unsigned int)(unsigned char)st[25] << 8)
         | ((unsigned int)(unsigned char)st[26] << 16)
         | ((unsigned int)(unsigned char)st[27] << 24);
    size = 0;
    for (i = 0; i < 8; i++) {
        size |= ((unsigned long)(unsigned char)st[48 + i]) << (8 * i);
    }
    if ((mode & 0xF000) != 0x8000 || size != 15) {
        puts("[U][FAIL] stat follow not regular");
        return;
    }

    fd = u_open("/tmp/sym_link", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open follow symlink");
        return;
    }
    n = u_read(fd, buf, 32);
    u_close(fd);
    if (n != 15) {
        puts("[U][FAIL] open follow read len");
        return;
    }
    /* "symlink-payload" — indices 0='s', 7='-', 14='d'. */
    if (buf[0] != 's' || buf[7] != '-' || buf[14] != 'd') {
        puts("[U][FAIL] open follow payload");
        return;
    }

    /* Relative symlink from /tmp: target "sym_tgt.txt" */
    if (u_symlink("sym_tgt.txt", "/tmp/sym_rel") != 0) {
        puts("[U][FAIL] symlink relative");
        return;
    }
    fd = u_open("/tmp/sym_rel", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open relative symlink");
        return;
    }
    n = u_read(fd, buf, 32);
    u_close(fd);
    if (n != 15) {
        puts("[U][FAIL] relative symlink read");
        return;
    }

    /* Directory symlink: /tmp/sym_dirlink -> /tmp then open through it. */
    if (u_symlink("/tmp", "/tmp/sym_dirlink") != 0) {
        puts("[U][FAIL] symlink dir");
        return;
    }
    fd = u_open("/tmp/sym_dirlink/sym_tgt.txt", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open via dir symlink");
        return;
    }
    u_close(fd);

    if (u_newfstatat(-100, "/tmp/sym_link", st, 0x100) != 0) {
        puts("[U][FAIL] fstatat NOFOLLOW");
        return;
    }
    mode = (unsigned int)(unsigned char)st[24]
         | ((unsigned int)(unsigned char)st[25] << 8)
         | ((unsigned int)(unsigned char)st[26] << 16)
         | ((unsigned int)(unsigned char)st[27] << 24);
    if ((mode & 0xF000) != 0xA000) {
        puts("[U][FAIL] fstatat NOFOLLOW not link");
        return;
    }

    if (u_unlink("/tmp/sym_link") != 0 || u_unlink("/tmp/sym_rel") != 0
        || u_unlink("/tmp/sym_dirlink") != 0) {
        puts("[U][FAIL] unlink symlink");
        return;
    }
    /* Target file must still exist after unlinking the link. */
    fd = u_open("/tmp/sym_tgt.txt", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] target gone after unlink link");
        return;
    }
    u_close(fd);
    u_unlink("/tmp/sym_tgt.txt");

    /* Ext2: link at /mnt/sym_e2 -> existing /mnt/m6pass.txt when mounted. */
    fd = u_open("/mnt/m6pass.txt", 0, 0);
    if (fd >= 0) {
        u_close(fd);
        u_unlink("/mnt/sym_e2");
        if (u_symlink("/mnt/m6pass.txt", "/mnt/sym_e2") != 0) {
            puts("[U][FAIL] Ext2 symlink create");
            return;
        }
        n = u_readlink("/mnt/sym_e2", buf, sizeof(buf) - 1);
        if (n < 0) {
            puts("[U][FAIL] Ext2 readlink");
            return;
        }
        fd = u_open("/mnt/sym_e2", 0, 0);
        if (fd < 0) {
            puts("[U][FAIL] Ext2 open follow symlink");
            return;
        }
        n = u_read(fd, buf, 8);
        u_close(fd);
        if (n <= 0) {
            puts("[U][FAIL] Ext2 follow read");
            return;
        }
        u_unlink("/mnt/sym_e2");
        puts("[U] Ext2 symlink follow OK");
    }

    puts("[U][PASS] symlink/readlink");
}

static void hardlink_test(void) {
    char st_a[144];
    char st_b[144];
    char buf[32];
    long fd;
    long n;
    unsigned long ino_a;
    unsigned long ino_b;
    unsigned long nlink;
    unsigned long i;

    u_unlink("/tmp/hl_a.txt");
    u_unlink("/tmp/hl_b.txt");
    u_unlink("/mnt/hl_a.txt");
    u_unlink("/mnt/hl_b.txt");

    fd = u_open("/tmp/hl_a.txt", 0x241, 0x1A4); /* WRONLY|CREAT|TRUNC */
    if (fd < 0) {
        puts("[U][FAIL] hardlink create");
        return;
    }
    if (u_write(fd, "hardlink-payload", 16) != 16) {
        puts("[U][FAIL] hardlink write");
        u_close(fd);
        return;
    }
    u_close(fd);

    if (u_link("/tmp/hl_a.txt", "/tmp/hl_b.txt") != 0) {
        puts("[U][FAIL] hardlink ramfs link");
        return;
    }
    if (u_stat("/tmp/hl_a.txt", st_a) != 0 || u_stat("/tmp/hl_b.txt", st_b) != 0) {
        puts("[U][FAIL] hardlink stat");
        return;
    }
    ino_a = 0;
    ino_b = 0;
    nlink = 0;
    for (i = 0; i < 8; i++) {
        ino_a |= ((unsigned long)(unsigned char)st_a[8 + i]) << (8 * i);
        ino_b |= ((unsigned long)(unsigned char)st_b[8 + i]) << (8 * i);
        nlink |= ((unsigned long)(unsigned char)st_a[16 + i]) << (8 * i);
    }
    if (ino_a == 0 || ino_a != ino_b) {
        puts("[U][FAIL] hardlink same inode");
        return;
    }
    if (nlink < 2) {
        puts("[U][FAIL] hardlink nlink");
        return;
    }

    fd = u_open("/tmp/hl_b.txt", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] hardlink open alias");
        return;
    }
    n = u_read(fd, buf, 32);
    u_close(fd);
    if (n != 16 || buf[0] != 'h' || buf[8] != '-' || buf[15] != 'd') {
        puts("[U][FAIL] hardlink alias read");
        return;
    }

    if (u_unlink("/tmp/hl_a.txt") != 0) {
        puts("[U][FAIL] hardlink unlink one");
        return;
    }
    fd = u_open("/tmp/hl_b.txt", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] hardlink survivor gone");
        return;
    }
    n = u_read(fd, buf, 32);
    u_close(fd);
    if (n != 16) {
        puts("[U][FAIL] hardlink survivor read");
        return;
    }
    if (u_unlink("/tmp/hl_b.txt") != 0) {
        puts("[U][FAIL] hardlink unlink last");
        return;
    }

    /* Ext2 when mounted: link under /mnt. */
    fd = u_open("/mnt/hl_a.txt", 0x241, 0x1A4);
    if (fd >= 0) {
        if (u_write(fd, "e2-hard", 7) != 7) {
            puts("[U][FAIL] Ext2 hardlink write");
            u_close(fd);
            return;
        }
        u_close(fd);
        if (u_link("/mnt/hl_a.txt", "/mnt/hl_b.txt") != 0) {
            puts("[U][FAIL] Ext2 hardlink link");
            return;
        }
        if (u_stat("/mnt/hl_a.txt", st_a) != 0 || u_stat("/mnt/hl_b.txt", st_b) != 0) {
            puts("[U][FAIL] Ext2 hardlink stat");
            return;
        }
        ino_a = 0;
        ino_b = 0;
        for (i = 0; i < 8; i++) {
            ino_a |= ((unsigned long)(unsigned char)st_a[8 + i]) << (8 * i);
            ino_b |= ((unsigned long)(unsigned char)st_b[8 + i]) << (8 * i);
        }
        if (ino_a == 0 || ino_a != ino_b) {
            puts("[U][FAIL] Ext2 hardlink inode");
            return;
        }
        if (u_unlink("/mnt/hl_a.txt") != 0) {
            puts("[U][FAIL] Ext2 hardlink unlink one");
            return;
        }
        fd = u_open("/mnt/hl_b.txt", 0, 0);
        if (fd < 0) {
            puts("[U][FAIL] Ext2 hardlink survivor");
            return;
        }
        n = u_read(fd, buf, 8);
        u_close(fd);
        if (n != 7 || buf[0] != 'e' || buf[3] != 'h') {
            puts("[U][FAIL] Ext2 hardlink read");
            return;
        }
        u_unlink("/mnt/hl_b.txt");
        puts("[U] Ext2 hardlink OK");
    }

    puts("[U][PASS] hard link(2)");
}

static void getcwd_test(void) {
    char buf[128];
    long n;

    u_unlink("/tmp/gcwd_link");
    u_rmdir("/tmp/gcwd_real");
    if (u_mkdir("/tmp/gcwd_real") != 0) {
        puts("[U][FAIL] getcwd mkdir");
        return;
    }
    if (u_symlink("/tmp/gcwd_real", "/tmp/gcwd_link") != 0) {
        puts("[U][FAIL] getcwd symlink");
        return;
    }
    if (u_chdir("/tmp/gcwd_link") != 0) {
        puts("[U][FAIL] getcwd chdir link");
        return;
    }
    if (u_getcwd(buf, sizeof(buf)) == -1) {
        puts("[U][FAIL] getcwd after link chdir");
        return;
    }
    /* Physical: /tmp/gcwd_real (14 chars). */
    n = 0;
    while (buf[n] != 0) n++;
    if (n != 14 || buf[0] != '/' || buf[5] != 'g' || buf[10] != 'r'
        || buf[13] != 'l') {
        puts("[U][FAIL] getcwd not physical");
        return;
    }
    if (u_chdir("..") != 0) {
        puts("[U][FAIL] getcwd cd ..");
        return;
    }
    if (u_getcwd(buf, sizeof(buf)) == -1) {
        puts("[U][FAIL] getcwd after ..");
        return;
    }
    n = 0;
    while (buf[n] != 0) n++;
    if (n != 4 || buf[0] != '/' || buf[1] != 't' || buf[3] != 'p') {
        puts("[U][FAIL] getcwd parent not /tmp");
        return;
    }
    u_unlink("/tmp/gcwd_link");
    u_rmdir("/tmp/gcwd_real");

    /* Ext2 physical cwd when mounted. */
    u_unlink("/mnt/gcwd_link");
    u_rmdir("/mnt/gcwd_real");
    if (u_mkdir("/mnt/gcwd_real") == 0) {
        if (u_symlink("/mnt/gcwd_real", "/mnt/gcwd_link") != 0) {
            puts("[U][FAIL] Ext2 getcwd symlink");
            return;
        }
        if (u_chdir("/mnt/gcwd_link") != 0) {
            puts("[U][FAIL] Ext2 getcwd chdir link");
            return;
        }
        if (u_getcwd(buf, sizeof(buf)) == -1) {
            puts("[U][FAIL] Ext2 getcwd");
            return;
        }
        n = 0;
        while (buf[n] != 0) n++;
        if (n != 14 || buf[1] != 'm' || buf[5] != 'g' || buf[13] != 'l') {
            puts("[U][FAIL] Ext2 getcwd not physical");
            return;
        }
        puts("[U] Ext2 getcwd physical OK");
        u_chdir("/mnt");
        u_unlink("/mnt/gcwd_link");
        u_rmdir("/mnt/gcwd_real");
    }

    puts("[U][PASS] getcwd physical");
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

    /* Pthread suite early: TCG often stalls later in init after COW; keep
     * this smokeable ahead of the known hang window. */
    futex_test();
    mutex_test();
    pthread_create_test();

    /* Signals early too: same TCG hang window that forces pthread early. */
    signal_test();

    file_test();
    heap_test();
    mmap_test();
    shm_test();
    dev_nodes_test();
    posix_basics_test();
    proc_self_test();
    stat_test();
    owner_mtime_test();
    cred_setuid_groups_test();
    symlink_test();
    hardlink_test();
    getcwd_test();
    dirent_test();
    bad_exec_test();
    wrap_len_test();
    orphan_test();

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
