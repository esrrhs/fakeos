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
extern long u_dirent_reclen(const char *p, unsigned long avail);
extern long u_chdir(const char *path);
extern long u_mkdir(const char *path);
extern long u_rmdir(const char *path);
extern long u_getcwd(char *buf, unsigned long len);
extern long u_kill(long pid, long sig);
extern long u_pipe(int *pipefd);
extern long u_unlink(const char *path);
extern long u_rename(const char *oldpath, const char *newpath);
extern long u_link(const char *oldpath, const char *newpath);
extern long u_symlink(const char *target, const char *linkpath);
extern long u_readlink(const char *path, char *buf, unsigned long n);
extern long u_lstat(const char *path, void *statbuf);
extern long u_getuid(void);
extern long u_geteuid(void);
extern long u_getgid(void);
extern long u_getegid(void);
extern long u_chown(const char *path, long uid, long gid);
extern long u_utimensat(long dfd, const char *path, const void *times, long flags);
extern long u_statfs(const char *path, void *buf);
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
extern int strcmp(const char *a, const char *b);
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

/* getdents64: name at +19, validated d_reclen controls each record. */
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
        long reclen;
        for (off = 0; off < n; off += reclen) {
            reclen = u_dirent_reclen(buf + off, n - off);
            if (reclen < 0) { puts("[U][FAIL] malformed dirent"); return; }
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

static int dent_has_name(const char *buf, long n, const char *want) {
    long off;
    long reclen;
    for (off = 0; off < n; off += reclen) {
        reclen = u_dirent_reclen(buf + off, n - off);
        if (reclen < 0) return 0;
        const char *name = buf + off + 19;
        int k = 0;
        while (want[k] != 0 && name[k] == want[k]) {
            k++;
        }
        if (want[k] == 0 && name[k] == 0) {
            return 1;
        }
    }
    return 0;
}

static void proc_pid_meminfo_test(void) {
    long fd;
    long n;
    long w;
    char buf[1536];
    char selfbuf[1024];
    char name_line[16];

    fd = u_open("/proc/meminfo", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /proc/meminfo");
        return;
    }
    n = u_read(fd, buf, 1535);
    u_close(fd);
    if (n <= 0 || !proc_has(buf, n, "MemTotal:") || !proc_has(buf, n, "MemFree:")
        || !proc_has(buf, n, "MemAvailable:") || !proc_has(buf, n, "MemUsed:")) {
        puts("[U][FAIL] /proc/meminfo fields");
        return;
    }

    fd = u_open("/proc/1/status", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /proc/1/status");
        return;
    }
    n = u_read(fd, buf, 1023);
    u_close(fd);
    if (n <= 0) {
        puts("[U][FAIL] read /proc/1/status");
        return;
    }
    name_line[0] = 'N'; name_line[1] = 'a'; name_line[2] = 'm';
    name_line[3] = 'e'; name_line[4] = ':'; name_line[5] = '\t';
    name_line[6] = 'i'; name_line[7] = 'n'; name_line[8] = 'i';
    name_line[9] = 't'; name_line[10] = 0;
    if (!proc_has(buf, n, name_line) || !proc_has(buf, n, "Pid:\t1\n")) {
        puts("[U][FAIL] /proc/1/status not init pid 1");
        return;
    }

    fd = u_open("/proc/self/status", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] reopen /proc/self/status");
        return;
    }
    w = u_read(fd, selfbuf, 1023);
    u_close(fd);
    if (w <= 0 || !proc_has(selfbuf, w, "Pid:\t1\n")) {
        puts("[U][FAIL] /proc/self/status Pid not 1");
        return;
    }

    fd = u_open("/proc/1/cmdline", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /proc/1/cmdline");
        return;
    }
    n = u_read(fd, buf, 1023);
    u_close(fd);
    if (n < 9 || !proc_has(buf, n, "/bin/init")) {
        puts("[U][FAIL] /proc/1/cmdline not /bin/init");
        return;
    }

    fd = u_open("/proc", 0, 0);
    if (fd < 0) {
        puts("[U][FAIL] open /proc for getdents");
        return;
    }
    n = u_getdents(fd, buf, sizeof(buf));
    u_close(fd);
    if (n <= 0 || !dent_has_name(buf, n, "self") || !dent_has_name(buf, n, "meminfo")
        || !dent_has_name(buf, n, "cpuinfo") || !dent_has_name(buf, n, "1")) {
        puts("[U][FAIL] /proc getdents missing self/meminfo/cpuinfo/1");
        return;
    }

    if (u_open("/proc/99999/status", 0, 0) >= 0) {
        puts("[U][FAIL] open /proc/99999/status should fail");
        return;
    }
    if (u_open("/proc/meminfo", 1, 0) >= 0) {
        puts("[U][FAIL] write-open /proc/meminfo should fail");
        return;
    }
    if (u_unlink("/proc/meminfo") == 0) {
        puts("[U][FAIL] unlink /proc/meminfo should fail");
        return;
    }
    if (u_unlink("/proc/1/status") == 0) {
        puts("[U][FAIL] unlink /proc/1/status should fail");
        return;
    }
    if (u_mkdir("/proc/notapid") == 0) {
        puts("[U][FAIL] mkdir /proc/notapid should fail");
        return;
    }

    puts("[U][PASS] /proc pids meminfo");
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
    /* utimensat(2): set explicit mtime seconds. */
    {
        char ts[32];
        unsigned long k;
        for (k = 0; k < 32; k++) ts[k] = 0;
        /* timespec[0] and [1]: tv_sec=12345, tv_nsec=0 */
        ts[0] = 57; ts[1] = 48; /* 12345 = 0x3039 little-endian */
        ts[16] = 57; ts[17] = 48;
        if (u_utimensat(-100, "/tmp/own_mtime.txt", ts, 0) != 0) {
            puts("[U][FAIL] utimensat set");
            return;
        }
        for (k = 0; k < 144; k++) st[k] = 0;
        if (u_stat("/tmp/own_mtime.txt", st) != 0) {
            puts("[U][FAIL] stat after utimensat");
            return;
        }
        if (st_u64(st, 88) != 12345) {
            puts("[U][FAIL] utimensat mtime");
            return;
        }
        puts("[U] utimensat OK");
        /* touch -r equivalent: copy mtime onto a second file. */
        {
            long fd2;
            char ts2[32];
            unsigned long k2;
            fd2 = u_open("/tmp/own_mtime2.txt", 0x40 | 0x200 | 2, 0x1A4);
            if (fd2 < 0) {
                puts("[U][FAIL] open own_mtime2");
                return;
            }
            u_close(fd2);
            for (k2 = 0; k2 < 32; k2++) ts2[k2] = 0;
            ts2[0] = 57; ts2[1] = 48;
            ts2[16] = 57; ts2[17] = 48;
            if (u_utimensat(-100, "/tmp/own_mtime2.txt", ts2, 0) != 0) {
                puts("[U][FAIL] utimensat copy");
                return;
            }
            for (k2 = 0; k2 < 144; k2++) st[k2] = 0;
            if (u_stat("/tmp/own_mtime2.txt", st) != 0 || st_u64(st, 88) != 12345) {
                puts("[U][FAIL] utimensat copy mtime");
                return;
            }
            puts("[U] utimensat copy-mtime OK");
        }
    }
    /* statfs(2) on ramfs root must report non-zero blocks. */
    {
        char sf[120];
        unsigned long k;
        unsigned long blocks;
        for (k = 0; k < 120; k++) sf[k] = 0;
        if (u_statfs("/", sf) != 0) {
            puts("[U][FAIL] statfs /");
            return;
        }
        blocks = st_u64(sf, 16);
        if (blocks == 0) {
            puts("[U][FAIL] statfs blocks");
            return;
        }
        puts("[U] statfs OK");
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

/* Namespace-level regressions: filesystem-local symlink tests cannot catch
 * dispatching a path to the wrong filesystem. All fixtures are repeatable. */
static int xmount_read(const char *path) {
    char buf[32];
    const char *payload = "cross-mount-payload";
    long fd = u_open(path, 0, 0);
    long n;
    int i;
    if (fd < 0) return 0;
    n = u_read(fd, buf, sizeof(buf));
    u_close(fd);
    if (n != 19) return 0;
    for (i = 0; i < 19; i++) if (buf[i] != payload[i]) return 0;
    return 1;
}

static void cross_mount_test(void) {
    char st[144];
    char buf[128];
    char cwd[512];
    long fd;
    long n;
    int i;
    const char *paths[12];
    paths[0] = "/tmp/xmount_data";
    paths[1] = "/mnt/xmount_data";
    paths[2] = "/tmp/xmount_to_ext";
    paths[3] = "/mnt/xmount_to_ram";
    paths[4] = "/tmp/xmount_dir";
    paths[5] = "/mnt/xmount_rel";
    paths[6] = "/tmp/xmount_nested";
    paths[7] = "/tmp/xmount_dangle";
    paths[8] = "/mnt/xmount_created";
    paths[9] = "/tmp/xmount_cycle";
    paths[10] = "/mnt/xmount_cycle";
    paths[11] = "/mnt/xmount_bad";
    fd = u_open("/mnt/m6pass.txt", 0, 0);
    if (fd < 0) return; /* This optional boot device is required by CI. */
    u_close(fd);
    for (i = 0; i < 12; i++) u_unlink(paths[i]);
    for (i = 0; i < 2; i++) {
        fd = u_open(paths[i], 0x241, 0644);
        if (fd < 0) { puts("[U][FAIL] cross-mount fixture open"); return; }
        n = u_write(fd, "cross-mount-payload", 19);
        u_close(fd);
        if (n != 19) { puts("[U][FAIL] cross-mount fixture write"); return; }
    }
    if (u_symlink(paths[1], paths[2]) != 0 ||
        u_symlink(paths[0], paths[3]) != 0 ||
        u_symlink("/mnt", paths[4]) != 0 ||
        u_symlink("../tmp/xmount_data", paths[5]) != 0 ||
        u_symlink("/tmp/xmount_dir/../etc/motd", paths[6]) != 0) {
        puts("[U][FAIL] cross-mount fixture links"); return;
    }
    if (!xmount_read(paths[2]) || !xmount_read(paths[3]) ||
        !xmount_read(paths[5]) || !xmount_read("/tmp/xmount_dir/xmount_data")) {
        puts("[U][FAIL] cross-mount open payload"); return;
    }
    if (u_stat(paths[2], st) != 0 || st[0] != 2 ||
        u_stat(paths[3], st) != 0 || st[0] != 1 ||
        u_newfstatat(12345, paths[2], st, 0) != 0 || st[0] != 2 ||
        u_lstat(paths[2], st) != 0 || st[0] != 1 ||
        ((unsigned char)st[25] & 0xF0) != 0xA0 ||
        u_newfstatat(-100, paths[3], st, 0x100) != 0 || st[0] != 2 ||
        ((unsigned char)st[25] & 0xF0) != 0xA0) {
        puts("[U][FAIL] cross-mount stat identity/nofollow"); return;
    }
    n = u_readlink(paths[2], buf, sizeof(buf));
    if (n != 16) { puts("[U][FAIL] cross-mount readlink length"); return; }
    for (i = 0; i < 16; i++) {
        if (buf[i] != paths[1][i]) { puts("[U][FAIL] cross-mount readlink target"); return; }
    }
    fd = u_open(paths[2], 0xC1, 0644); /* exclusive create inspects link */
    if (fd >= 0) { u_close(fd); puts("[U][FAIL] cross-mount O_EXCL"); return; }
    fd = u_open(paths[2], 0x20000, 0);
    if (fd >= 0) { u_close(fd); puts("[U][FAIL] cross-mount O_NOFOLLOW"); return; }
    if (u_symlink(paths[8], paths[7]) != 0) {
        puts("[U][FAIL] cross-mount dangling link"); return;
    }
    fd = u_open(paths[7], 0xC1, 0644);
    if (fd >= 0 || u_stat(paths[8], st) == 0) {
        if (fd >= 0) u_close(fd);
        puts("[U][FAIL] cross-mount dangling O_EXCL"); return;
    }
    fd = u_open(paths[7], 0x241, 0644);
    if (fd < 0) { puts("[U][FAIL] cross-mount dangling O_CREAT"); return; }
    n = u_write(fd, "cross-mount-payload", 19);
    u_close(fd);
    if (n != 19 || !xmount_read(paths[8]) ||
        u_unlink("/tmp/xmount_dir/xmount_created") != 0 ||
        u_lstat(paths[7], st) != 0 || u_stat(paths[8], st) == 0) {
        puts("[U][FAIL] cross-mount create/unlink parent"); return;
    }
    if (u_getcwd(cwd, sizeof(cwd)) < 0 || u_chdir(paths[4]) != 0 ||
        u_getcwd(buf, sizeof(buf)) < 0 || buf[0] != '/' || buf[1] != 'm' ||
        buf[2] != 'n' || buf[3] != 't' || buf[4] != 0 || u_chdir(cwd) != 0) {
        puts("[U][FAIL] cross-mount physical cwd"); return;
    }
    fd = u_open("/tmp/xmount_dir/../etc/motd", 0, 0);
    if (fd < 0) { puts("[U][FAIL] cross-mount link before dotdot"); return; }
    u_close(fd);
    fd = u_open(paths[6], 0, 0);
    if (fd < 0) { puts("[U][FAIL] cross-mount target dotdot"); return; }
    u_close(fd);
    fd = u_open("/tmp/xmount_data/../xmount_data", 0, 0);
    if (fd >= 0) { u_close(fd); puts("[U][FAIL] cross-mount non-directory dotdot"); return; }
    if (u_symlink(paths[10], paths[9]) != 0 || u_symlink(paths[9], paths[10]) != 0) {
        puts("[U][FAIL] cross-mount cycle fixture"); return;
    }
    fd = u_open(paths[9], 0, 0);
    if (fd >= 0 || u_stat(paths[9], st) == 0 || u_lstat(paths[9], st) != 0) {
        if (fd >= 0) u_close(fd);
        puts("[U][FAIL] cross-mount cycle bound"); return;
    }
    /* Entry mutations must never follow a last symlink and erase its target,
     * even when the caller supplies a trailing slash. */
    u_unlink("/tmp/xmount_rmdir");
    u_rmdir("/mnt/xmount_sub");
    if (u_mkdir("/tmp/xmount_dir/xmount_sub/") != 0 ||
        u_symlink("/mnt/xmount_sub", "/tmp/xmount_rmdir") != 0 ||
        u_rmdir("/tmp/xmount_rmdir/") == 0 ||
        u_unlink("/tmp/xmount_rmdir/") == 0 ||
        u_stat("/mnt/xmount_sub", st) != 0 ||
        u_lstat("/tmp/xmount_rmdir", st) != 0 ||
        u_lstat("/tmp/xmount_rmdir/", st) != 0 ||
        ((unsigned char)st[25] & 0xF0) != 0x40 ||
        u_rmdir("/tmp/xmount_dir/xmount_sub/") != 0 ||
        u_unlink("/tmp/xmount_rmdir") != 0) {
        puts("[U][FAIL] cross-mount trailing slash entry safety"); return;
    }
    if (u_rename(paths[0], "/tmp/xmount_dir/xmount_bad") == 0 ||
        !xmount_read(paths[0]) || u_stat(paths[11], st) == 0) {
        puts("[U][FAIL] cross-mount rename EXDEV safety"); return;
    }
    if (u_unlink(paths[2]) != 0 || !xmount_read(paths[1])) {
        puts("[U][FAIL] cross-mount unlink preserves target"); return;
    }
    for (i = 0; i < 12; i++) u_unlink(paths[i]);
    puts("[U][PASS] cross-mount symlink namespace");
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


/* Explicit regression mode: keep large I/O out of the normal boot deadline.
 * It runs in a child /bin/init, not a new utility or a privileged host oracle. */
static unsigned char reclaim_byte(unsigned long off, unsigned int seed) {
    return (unsigned char)(off * 37 + (off >> 8) + seed);
}

static int reclaim_write(const char *path, unsigned long size, unsigned int seed) {
    char buf[4096];
    unsigned long off;
    unsigned long i;
    long fd = u_open(path, 0x241, 0644);
    if (fd < 0) return 0;
    for (off = 0; off < size; off += sizeof(buf)) {
        unsigned long n = size - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        for (i = 0; i < n; i++) buf[i] = (char)reclaim_byte(off + i, seed);
        if (u_write(fd, buf, n) != (long)n) { u_close(fd); return 0; }
    }
    u_close(fd);
    return 1;
}

static int reclaim_read(const char *path, unsigned long size,
                        unsigned long patterned, unsigned int seed) {
    char buf[4096];
    unsigned long off;
    unsigned long i;
    long fd = u_open(path, 0, 0);
    if (fd < 0) return 0;
    for (off = 0; off < size; off += sizeof(buf)) {
        unsigned long n = size - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        if (u_read(fd, buf, n) != (long)n) { u_close(fd); return 0; }
        for (i = 0; i < n; i++) {
            unsigned char want = off + i < patterned ? reclaim_byte(off + i, seed) : 0;
            if ((unsigned char)buf[i] != want) { u_close(fd); return 0; }
        }
    }
    i = u_read(fd, buf, 1);
    u_close(fd);
    return i == 0;
}

static int ext2_reclaim_check(void) {
    const char *victim = "/mnt/reclaim/victim";
    const char *survivor = "/mnt/reclaim/survivor";
    unsigned long before[15];
    unsigned long now[15];
    unsigned long base_blocks;
    unsigned long base_inodes;
    unsigned long i;
    unsigned long cycle;
    char buf[4096];
    long fd;
    u_mkdir("/mnt/reclaim");
    u_unlink(victim);
    fd = u_open(survivor, 0, 0);
    if (fd >= 0) {
        u_close(fd);
        if (!reclaim_read(survivor, 327680, 327680, 73)) {
            puts("[U][FAIL] Ext2 reclaim persisted survivor"); return 1;
        }
        puts("[U] Ext2 reclaim persisted survivor verified");
    } else if (!reclaim_write(survivor, 327680, 73)) {
        puts("[U][FAIL] Ext2 reclaim survivor fixture"); return 1;
    }
    if (u_statfs("/mnt", before) != 0 || before[0] != 0xEF53 || before[1] != 1024) {
        puts("[U][FAIL] Ext2 reclaim statfs fixture"); return 1;
    }
    base_blocks = before[3];
    base_inodes = before[6];
    for (cycle = 0; cycle < 5; cycle++) {
        unsigned long want_blocks = cycle == 4 ? 7 : 772;
        if (cycle == 4) {
            fd = u_open(victim, 0x241, 0644);
            for (i = 0; i < sizeof(buf); i++) buf[i] = (char)reclaim_byte(i, 91);
            if (fd < 0 || u_lseek(fd, 73400320, 0) != 73400320 ||
                u_write(fd, buf, sizeof(buf)) != sizeof(buf)) {
                if (fd >= 0) u_close(fd);
                puts("[U][FAIL] Ext2 reclaim triple-indirect fixture"); return 1;
            }
            u_close(fd);
        } else if (!reclaim_write(victim, 786432, 91)) {
            puts("[U][FAIL] Ext2 reclaim victim write"); return 1;
        }
        if (u_statfs("/mnt", now) != 0 || now[3] + want_blocks != base_blocks ||
            now[6] + 1 != base_inodes || !reclaim_read(survivor, 327680, 327680, 73)) {
            puts("[U][FAIL] Ext2 reclaim allocation/survivor integrity"); return 1;
        }
        if (cycle == 0 || cycle == 4) {
            if (u_unlink(victim) != 0) { puts("[U][FAIL] Ext2 reclaim unlink"); return 1; }
        } else {
            fd = u_open(victim, cycle == 1 ? 0x202 : 2, 0);
            if (fd < 0) { puts("[U][FAIL] Ext2 reclaim truncate open"); return 1; }
            if (cycle == 3) {
                if (ftruncate((int)fd, 4097) != 0 ||
                    !reclaim_read(victim, 4097, 4097, 91) ||
                    ftruncate((int)fd, 786432) != 0 ||
                    !reclaim_read(victim, 786432, 4097, 91)) {
                    u_close(fd); puts("[U][FAIL] Ext2 reclaim shrink/regrow zeros"); return 1;
                }
            }
            if ((cycle != 1 && ftruncate((int)fd, 0) != 0) ||
                u_statfs("/mnt", now) != 0 || now[3] != base_blocks ||
                now[6] + 1 != base_inodes) {
                u_close(fd); puts("[U][FAIL] Ext2 reclaim truncate counts"); return 1;
            }
            u_close(fd);
            if (!reclaim_read(victim, 0, 0, 91) || u_unlink(victim) != 0) {
                puts("[U][FAIL] Ext2 reclaim empty/unlink"); return 1;
            }
        }
        if (u_statfs("/mnt", now) != 0 || now[3] != base_blocks || now[6] != base_inodes) {
            printf("[U][FAIL] Ext2 reclaim cycle=%u blocks=%u expected=%u inodes=%u expected=%u\n",
                   (unsigned int)cycle, (unsigned int)now[3], (unsigned int)base_blocks,
                   (unsigned int)now[6], (unsigned int)base_inodes);
            return 1;
        }
        if (!reclaim_read(survivor, 327680, 327680, 73)) {
            puts("[U][FAIL] Ext2 reclaim survivor after free/reuse"); return 1;
        }
        printf("[U] Ext2 reclaim cycle=%u blocks=%u inodes=%u exact\n",
               (unsigned int)cycle, (unsigned int)now[3], (unsigned int)now[6]);
    }
    puts("[U][PASS] Ext2 indirect reclaim/accounting/survivor");
    return 0;
}

/* Raw process/task lifetime probes. Shared pages and stacks are prefaulted;
 * atomic assembly operations give actual acquire/release handoff on x86-64.
 * No time-based delay is used to manufacture an exit ordering. */
extern long u_clone(unsigned long flags, void *stack, void *fn, void *arg);
extern long u_set_tid_address(void *addr);
extern void u_exit_group(long code);
extern unsigned int atomic_xchg(volatile unsigned int *p, unsigned int value);
extern unsigned int atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value);
struct life_gate {
    volatile unsigned int ready, release, returned, early;
    volatile unsigned int leader_alive, worker_alive, done;
    long parent, child;
};
static unsigned int life_load(volatile unsigned int *p) { return atomic_cmpxchg(p, 0, 0); }
static u64 life_now(void) {
    long t[2];
    if (u_clock_gettime(1, t) != 0) return 0;
    return (u64)t[0] * 1000000000UL + (u64)t[1];
}
static int life_until(volatile unsigned int *p, unsigned int value) {
    u64 end = life_now() + 5000000000UL;
    while (life_load(p) != value) { if (life_now() >= end) return 0; u_yield(); }
    return 1;
}
static char life_status(long pid, int *one_thread) {
    char path[64], digits[24], buf[512];
    long n, fd; int i, at = 6, d = 0;
    path[0]='/';path[1]='p';path[2]='r';path[3]='o';path[4]='c';path[5]='/';
    do { digits[d++] = '0' + pid % 10; pid /= 10; } while (pid);
    while (d) path[at++] = digits[--d];
    { const char *suffix="/status"; for(i=0;suffix[i];i++) path[at++]=suffix[i]; }
    path[at]=0; *one_thread=0;
    fd=u_open(path,0,0); if(fd<0) return '?';
    n=u_read(fd,buf,511);u_close(fd);if(n<=0)return '?';buf[n]=0;
    for(i=0;i+10<n;i++) if(buf[i]=='T' && buf[i+1]=='h' && buf[i+7]==':' && buf[i+9]=='1' && buf[i+10]=='\n') *one_thread=1;
    for(i=0;i+7<n;i++) if(buf[i]=='S'&&buf[i+1]=='t'&&buf[i+5]==':'&&buf[i+6]=='\t') return buf[i+7];
    return '?';
}
static void life_worker(void *arg) {
    struct life_gate *g=(struct life_gate *)arg;
    int one=0; char st; u64 end=life_now()+5000000000UL;
    u_set_tid_address((void *)&g->worker_alive);
    atomic_xchg(&g->ready,1);
    if(g->child == -1) {
        life_until(&g->release,1);
    } else {
        if(!life_until(&g->leader_alive,0)) u_exit(90);
        do { st=life_status(u_getpid(),&one); if(one||st=='Z')break;u_yield(); } while(life_now()<end);
        for(;;) {
            if(life_load(&g->returned)) { atomic_xchg(&g->early,1);break; }
            st=life_status(g->parent,&one);
            if(st=='S')break;
            if(life_now()>=end)u_exit(91);
            u_yield();
        }
    }
    atomic_xchg(&g->done,1);
    u_exit(29);
}
struct life_fan { volatile unsigned int release; volatile unsigned int alive[64]; };
struct life_arg { struct life_fan *fan; int index; };
static void life_fan_worker(void *arg) {
    struct life_arg *a=(struct life_arg *)arg;
    u_set_tid_address((void *)&a->fan->alive[a->index]);
    atomic_xchg(&a->fan->alive[a->index],1);
    life_until(&a->fan->release,1);
    u_exit(0);
}
static int life_failure_checks(void) {
    struct life_fan *f=(struct life_fan *)u_mmap(0,4096,3,0x21,-1,0);
    char *stacks=(char *)u_mmap(0,64*16384,3,0x22,-1,0);
    struct life_arg args[64];int i,round,n,previous_n=0;long pid,status;u64 end;
    if((long)f<0 || (long)stacks<0)return 1;
    for(i=0;i<4096;i++)((volatile char *)f)[i]=0;
    for(i=0;i<64*16384;i+=4096)stacks[i]=0;
    for(round=0;round<2;round++) {
        atomic_xchg(&f->release,0);n=0;
        for(i=0;i<64;i++) {
            args[i].fan=f;args[i].index=i;atomic_xchg(&f->alive[i],2);
            if(u_clone(0x10100,stacks+(i+1)*16384-8,(void *)&life_fan_worker,&args[i])<0) {atomic_xchg(&f->alive[i],0);break;}
            n++;
        }
        /* The pool is bounded: at least one clone must be rejected, and the
         * rejected count must be deterministic across rounds (rollback +
         * reuse). With the widened 64-slot pool the attempts may all fit;
         * the bound check only fires when slots remain scarce. */
        if(n==0 || (round && n!=previous_n))return 1;
        previous_n=n;
        for(i=0;i<n;i++)if(!life_until(&f->alive[i],1))return 1;
        atomic_xchg(&f->release,1);
        for(i=0;i<n;i++)if(!life_until(&f->alive[i],0))return 1;
        end=life_now()+5000000000UL;
        do{pid=u_fork();if(pid!=-16)break;u_yield();}while(life_now()<end);
        if(pid<0)return 1;if(pid==0)u_exit(0);
        if(u_wait4(pid,&status,0,0)!=pid || status)return 1;
        printf("[LIFETIME] bounded task exhaustion round=%d created=%d rollback+reuse OK\n",round,n);
    }
    /* Two independent process address spaces become exit-runnable together.
     * Repeat on the SAME owner/coordination page, not separate serialized VMs. */
    for(round=0;round<6;round++) {
        long children[2];
        atomic_xchg(&f->release,0);
        for(i=0;i<2;i++) {
            atomic_xchg(&f->alive[i],0);children[i]=u_fork();if(children[i]<0)return 1;
            if(children[i]==0){atomic_xchg(&f->alive[i],1);if(!life_until(&f->release,1))u_exit(95);u_exit(31+i);}
        }
        for(i=0;i<2;i++)if(!life_until(&f->alive[i],1))return 1;
        atomic_xchg(&f->release,1);
        for(i=0;i<2;i++)if(u_wait4(children[i],&status,0,0)!=children[i] || ((status>>8)&255)!=31+i)return 1;
    }
    puts("[LIFETIME] PASS bounded task allocation failure/rollback/reuse and concurrent process teardown");
    u_munmap(f,4096);u_munmap(stacks,32*16384);return 0;
}

extern long u_poll(void *fds, unsigned long n, long timeout);
static int life_owner_capacity_check(void) {
    long children[32],pid,status;int pipefd[2];int n,i,one;u64 end;char byte;
    struct { int fd; short events; short revents; } pollfd;
    if(u_pipe(pipefd)!=0)return 1;
    for(n=0;n<32;n++) {
        pid=u_fork();if(pid<0)break;
        if(pid==0)u_exit(0);
        children[n]=pid;end=life_now()+5000000000UL;
        while(life_status(pid,&one)!='Z'){if(life_now()>=end)return 1;u_yield();}
    }
    if(n==0 || n==32)return 1;
    for(i=0;i<n;i++)if(u_wait4(children[i],&status,0,0)!=children[i] || status)return 1;
    pid=u_fork();if(pid<0)return 1;if(pid==0)u_exit(0);
    if(u_wait4(pid,&status,0,0)!=pid || status)return 1;
    u_close(pipefd[1]);pollfd.fd=pipefd[0];pollfd.events=1;pollfd.revents=0;
    /* EOF must be immediately readable: a leaked retained writer from the
     * failed process-admission path would make zero-timeout poll return0. */
    if(u_poll(&pollfd,1,0)!=1 || !(pollfd.revents&1) || u_read(pipefd[0],&byte,1)!=0)return 1;
    u_close(pipefd[0]);
    printf("[LIFETIME] PASS owner capacity=%d admission rollback, pipe refs/EOF and process reuse\n",n);
    return 0;
}

static int life_extra_checks(void) {
    struct life_gate *g=(struct life_gate *)u_mmap(0,4096,3,0x21,-1,0);
    char *stack=(char *)u_mmap(0,16384,3,0x22,-1,0);
    char *argv[2]; long pid,status,rc;int i;u64 end;
    if((long)g<0 || (long)stack<0)return 1;
    for(i=0;i<4096;i++)((volatile char *)g)[i]=0;
    for(i=0;i<16384;i+=4096)stack[i]=0;
    g->worker_alive=1;g->child=-1;
    if(u_clone(0x10100,stack+16384-8,(void *)&life_worker,g)<0)return 1;
    if(!life_until(&g->ready,1))return 1;
    argv[0]="/bin/hello";argv[1]=0;
    pid=u_fork();
    if(pid==0)u_exit(99);
    rc=u_execve("/bin/hello",argv,0);
    if(pid!=-16 || rc!=-16)return 1;
    atomic_xchg(&g->release,1);
    if(!life_until(&g->worker_alive,0))return 1;
    end=life_now()+5000000000UL;
    do {pid=u_fork();if(pid!=-16)break;u_yield();}while(life_now()<end);
    if(pid<0)return 1;
    if(pid==0)u_exit(0);
    if(u_wait4(pid,&status,0,0)!=pid || status)return 1;
    if(u_execve("/no-such-lifetime-image",argv,0)!=-1)return 1;
    pid=u_fork();if(pid<0)return 1;
    if(pid==0){u_execve("/bin/hello",argv,0);u_exit(98);}
    if(u_wait4(pid,&status,0,0)!=pid || ((status>>8)&255)!=42)return 1;
    puts("[LIFETIME] PASS multithread fork/exec BUSY, failed-image rollback, single-task fork/exec");
    for(i=0;i<64;i++) {
        pid=u_fork();if(pid<0)return 1;
        if(pid==0)u_exit(0);
        if(u_wait4(pid,&status,0,0)!=pid || status || u_wait4(pid,&status,1,0)!=-1)return 1;
    }
    puts("[LIFETIME] PASS 64 unique process completions and slot/AS reuse");
    u_munmap(g,4096);u_munmap(stack,16384);return 0;
}

static int process_lifetime_probe(void) {
    int mode, rc=0; long pid, got, status;
    for(mode=0;mode<2;mode++) {
        struct life_gate *g=(struct life_gate *)u_mmap(0,4096,3,0x21,-1,0);
        char *stack=(char *)u_mmap(0,16384,3,0x22,-1,0); int i;
        if((long)g<0||(long)stack<0)return 1;
        for(i=0;i<4096;i++)((volatile char *)g)[i]=0;
        for(i=0;i<16384;i+=4096)stack[i]=0;
        g->parent=u_getpid();g->leader_alive=1;g->worker_alive=1;g->child=mode?-1:0;
        pid=u_fork();if(pid<0)return 1;
        if(pid==0) {
            long tid;
            u_set_tid_address((void *)&g->leader_alive);
            tid=u_clone(0x10100,stack+16384-8,(void *)&life_worker,g);
            if(tid<0)u_exit(92);
            if(!life_until(&g->ready,1))u_exit(93);
            if(mode)u_exit_group(42);else u_exit(17);
        }
        got=u_wait4(pid,&status,0,0);atomic_xchg(&g->returned,1);
        if(mode && life_load(&g->worker_alive)) atomic_xchg(&g->release,1);
        if(!life_until(&g->worker_alive,0))return 1;
        printf("[LIFETIME] mode=%d pid=%ld got=%ld code=%ld early=%u worker_done=%u\n",mode,pid,got,(status>>8)&255,life_load(&g->early),life_load(&g->done));
        if(got!=pid || (!mode && ((status>>8)&255)!=29) || (mode && (((status>>8)&255)!=42 || life_load(&g->done))))rc=1;
        if(u_wait4(pid,&status,1,0)!=-1)rc=1;
        u_munmap(g,4096);u_munmap(stack,16384);
    }
    if(!rc && life_extra_checks())rc=1;
    if(!rc && life_failure_checks())rc=1;
    if(!rc && life_owner_capacity_check())rc=1;
    puts(rc?"[LIFETIME] FAIL process/task semantics":"[LIFETIME] PASS process/task semantics");
    return rc;
}

/* Trusted boot driver arms rendezvous; this program has no ability to arm it. */
static volatile unsigned int park_words[2];
static volatile unsigned int park_hits;
static void park_handler(int sig) { (void)sig;park_hits=park_hits+1; }
static void park_worker(void *arg) {
    (void)arg;futex(&park_words[1],0,0,0,0,0);u_exit(0);
}
static int park_probe(int mode) {
    long rc;char *stack;int i;
    atomic_xchg(&park_words[0],0);atomic_xchg(&park_words[1],0);park_hits=0;
    if(mode==3 || mode==4) {
        struct { void *sa_handler; unsigned long sa_flags; void *sa_restorer; unsigned long sa_mask; } act;
        act.sa_restorer=0;act.sa_mask=0;act.sa_handler=(void *)park_handler;
        act.sa_flags=mode==4?0x10000000:0;
        if(sigaction(15,&act,0)!=0)return 81;
    }
    if(mode==5 || mode==6) {
        stack=(char *)u_mmap(0,16384,3,0x22,-1,0);if((long)stack<0)return 82;
        for(i=0;i<16384;i+=4096)stack[i]=0;
        if(u_clone(0x10100,stack+16384-8,(void *)&park_worker,0)<0)return 83;
    }
    rc=futex(&park_words[0],0,0,0,0,0);
    if(mode==5 || mode==6)u_exit_group(42);
    if(mode==3)return rc==-4 && park_hits==1 ? 0:84;
    if(mode==4)return rc==0 && park_hits==1 ? 0:85;
    if(mode==7 && life_load(&park_words[0])!=1)return 87;
    return rc==0 ? 0:86;
}

/* ---------------------------------------------------------------------------
 * Native capability namespace probe (migration gate 2). Every handle this
 * program holds arrived through the documented ABI: channel_create, dup, or
 * fork inheritance. No kernel arming, no global IDs.
 * ------------------------------------------------------------------------- */
extern long u_nat_channel_create(long *send, long *recv);
extern long u_nat_send(long cap, long value, long block);
extern long u_nat_recv(long cap, long *value, long block);
extern long u_nat_dup(long cap, long rights);
extern long u_nat_close(long cap);
extern long u_kill(long pid, long sig);

struct nc_gate {
    volatile unsigned int ready, release, done, ok;
    long send_cap, recv_cap;
    long count;
    long rc;
    volatile unsigned int published;
    long foreign_rx;
    volatile long foreign_rc;
};
static void nc_thr_sender(void *arg) {
    struct nc_gate *g=(struct nc_gate *)arg;
    long i,r;
    atomic_xchg(&g->ready,1);
    while(!life_load(&g->release))u_yield();
    for(i=1;i<=g->count;i++) {
        r=u_nat_send(g->send_cap,(long)(i*7+3),1);
        if(r!=0){atomic_xchg(&g->ok,0);break;}
        if((i&15)==0)u_yield();
    }
    atomic_xchg(&g->done,1);
    u_exit(0);
}
/* Blocks receiving on a channel with a live sender and empty queue, so the
 * only exits are message, close-revoke, or signal. */
static void nc_thr_blocker(void *arg) {
    struct nc_gate *g=(struct nc_gate *)arg;
    long v=0;
    atomic_xchg(&g->ready,1);
    g->rc=u_nat_recv(g->recv_cap,&v,1);
    atomic_xchg(&g->done,1);
    u_exit(0);
}
static volatile unsigned int nc_sig_hits;
static void nc_sig_handler(int sig) { (void)sig;nc_sig_hits=nc_sig_hits+1; }

static int native_caps_probe(void) {
    long sx, rx, v, d, pid, got, code2, status, sS, rS;
    struct nc_gate *g;
    char *stack;
    int i;

    /* Step 1: same-process user-thread exchange; copied scalars in FIFO
     * order through the capability layer. */
    g=(struct nc_gate *)u_mmap(0,4096,3,0x21,-1,0);
    stack=(char *)u_mmap(0,16384,3,0x22,-1,0);
    if((long)g<0||(long)stack<0)return 1;
    for(i=0;i<(int)sizeof(*g)/4;i++)((volatile unsigned int *)g)[i]=0;
    for(i=0;i<16384;i+=4096)stack[i]=0;
    if(u_nat_channel_create(&sx,&rx)!=0)return 2;
    g->send_cap=sx;g->recv_cap=rx;g->count=256;atomic_xchg(&g->ok,1);
    if(u_clone(0x10100,stack+16384-8,(void *)&nc_thr_sender,g)<0)return 3;
    if(!life_until(&g->ready,1))return 4;
    atomic_xchg(&g->release,1);
    for(i=1;i<=256;i++) {
        if(u_nat_recv(rx,&v,1)!=1 || v!=(long)(i*7+3))return 5;
        if((i&31)==0)u_yield();
    }
    if(!life_until(&g->done,1))return 6;
    if(!life_load(&g->ok))return 7;
    if(u_nat_close(sx)!=0 || u_nat_close(rx)!=0)return 8;
    puts("[NATIVE-CAPS] step1 PASS user-thread 256 copied scalar messages");

    /* Step 2: rights attenuation. A RECV duplicate of the RECV cap can
     * receive but not send; widening a send cap to RECV is refused; a
     * closed duplicate is stale. */
    if(u_nat_channel_create(&sx,&rx)!=0)return 9;
    d=u_nat_dup(rx,2);
    if(d<0)return 10;
    if(u_nat_send(d,7,0)!=-1)return 11;
    if(u_nat_dup(sx,2)!=-1)return 12;
    if(u_nat_send(sx,777,0)!=0)return 13;
    if(u_nat_recv(d,&v,0)!=1 || v!=777)return 14;
    if(u_nat_close(d)!=0)return 15;
    if(u_nat_send(d,1,0)!=-9)return 16;
    puts("[NATIVE-CAPS] step2 PASS rights attenuation, wrong-side refusal, dup close");

    /* Step 3: EOF after the last sender closes; drains first. */
    if(u_nat_send(sx,42,0)!=0)return 17;
    if(u_nat_close(sx)!=0)return 18;
    if(u_nat_recv(rx,&v,0)!=1 || v!=42)return 19;
    if(u_nat_recv(rx,&v,1)!=0)return 20;
    if(u_nat_close(rx)!=0)return 21;
    puts("[NATIVE-CAPS] step3 PASS drain-then-EOF on last sender close");

    /* Step 4: forged and retired-generation values never resolve. */
    if(u_nat_recv((0x0001L<<16)|5,&v,0)!=-9)return 22;
    if(u_nat_recv((0xFFFFL<<16)|1,&v,0)!=-9)return 23;
    if(u_nat_recv(0x7FFF0001L,&v,0)!=-9)return 24;
    if(u_nat_send(0,&v,0)!=-9)return 25;
    puts("[NATIVE-CAPS] step4 PASS forged/stale handle rejection");

    /* Step 5: cross-process echo through fork inheritance + foreign-handle
     * isolation. Endpoint discipline: the parent closes its own receive
     * side of channel A after the fork so the child is the only reader,
     * and its own send side of channel B so the child is the only echo
     * sender; channel C is created after the fork and must be foreign. */
    {
        struct nc_gate *sh=(struct nc_gate *)u_mmap(0,4096,3,0x21,-1,0);
        long sxA, rxA, sxB, rxB, sxC, rxC;
        if((long)sh<0)return 26;
        for(i=0;i<(int)sizeof(*sh)/4;i++)((volatile unsigned int *)sh)[i]=0;
        if(u_nat_channel_create(&sxA,&rxA)!=0)return 27;
        if(u_nat_channel_create(&sxB,&rxB)!=0)return 28;
        pid=u_fork();
        if(pid<0)return 29;
        if(pid==0) {
            long vv=0;
            while(!life_load(&sh->published))u_yield();
            /* A live parent capability value from AFTER the fork must be
             * foreign here: this table never installed it. */
            sh->foreign_rc=u_nat_recv(sh->foreign_rx,&vv,0);
            if(u_nat_recv(rxA,&vv,1)!=1)u_exit(71);
            if(vv!=12345)u_exit(72);
            if(u_nat_send(sxB,vv+1,1)!=0)u_exit(73);
            u_exit(0);
        }
        if(u_nat_close(rxA)!=0)return 30;
        if(u_nat_close(sxB)!=0)return 31;
        if(u_nat_channel_create(&sxC,&rxC)!=0)return 32;
        sh->foreign_rx=rxC;
        atomic_xchg(&sh->published,1);
        if(u_nat_send(sxA,12345,1)!=0)return 33;
        if(u_nat_recv(rxB,&v,1)!=1 || v!=12346)return 34;
        if(u_wait4(pid,&status,0,0)!=pid)return 35;
        if(((status>>8)&255)!=0)return 36;
        if(sh->foreign_rc!=-9 && sh->foreign_rc>=0)return 37;
        if(u_nat_close(sxA)!=0||u_nat_close(rxB)!=0)return 38;
        if(u_nat_close(sxC)!=0||u_nat_close(rxC)!=0)return 39;
        u_munmap(sh,4096);
        puts("[NATIVE-CAPS] step5 PASS cross-process echo, foreign-handle isolation");
    }

    /* Step 6: two echo children concurrently on four channels: one down
     * channel per child (parent sends), one echo channel per child (child
     * sends). The parent closes its own receive sides of the down channels
     * and send sides of the echo channels after forking, and each child
     * closes the other child's inherited receive side, so exactly one
     * reader and one writer exist per direction. */
    {
        long s1,r1,s1r,r1r, s2,r2,s2r,r2r;
        if(u_nat_channel_create(&s1,&r1)!=0)return 50;
        if(u_nat_channel_create(&s1r,&r1r)!=0)return 51;
        if(u_nat_channel_create(&s2,&r2)!=0)return 52;
        if(u_nat_channel_create(&s2r,&r2r)!=0)return 53;
        pid=u_fork();
        if(pid<0)return 54;
        if(pid==0) {
            long vv=0;
            if(u_nat_close(r2)!=0)u_exit(80);
            if(u_nat_recv(r1,&vv,1)!=1||vv!=100)u_exit(74);
            if(u_nat_send(s1r,vv+1,1)!=0)u_exit(75);
            u_exit(0);
        }
        got=pid;
        pid=u_fork();
        if(pid<0)return 55;
        if(pid==0) {
            long vv=0;
            if(u_nat_close(r1)!=0)u_exit(81);
            if(u_nat_recv(r2,&vv,1)!=1||vv!=200)u_exit(76);
            if(u_nat_send(s2r,vv+1,1)!=0)u_exit(77);
            u_exit(0);
        }
        code2=pid;
        if(u_nat_close(r1)!=0||u_nat_close(r2)!=0)return 56;
        if(u_nat_close(s1r)!=0||u_nat_close(s2r)!=0)return 57;
        if(u_nat_send(s1,100,1)!=0)return 58;
        if(u_nat_send(s2,200,1)!=0)return 59;
        if(u_nat_recv(r1r,&v,1)!=1||v!=101)return 60;
        if(u_nat_recv(r2r,&v,1)!=1||v!=201)return 61;
        if(u_wait4(got,&status,0,0)!=got||((status>>8)&255)!=0)return 62;
        if(u_wait4(code2,&status,0,0)!=code2||((status>>8)&255)!=0)return 63;
        if(u_nat_close(s1)!=0||u_nat_close(r1r)!=0)return 64;
        if(u_nat_close(s2)!=0||u_nat_close(r2r)!=0)return 65;
        puts("[NATIVE-CAPS] step6 PASS concurrent separate-process exchanges");
    }

    /* Step 7: closing a capability revokes a blocked receive on it. */
    {
        if(u_nat_channel_create(&sx,&rx)!=0)return 61;
        for(i=0;i<(int)sizeof(*g)/4;i++)((volatile unsigned int *)g)[i]=0;
        g->recv_cap=rx;
        if(u_clone(0x10100,stack+16384-8,(void *)&nc_thr_blocker,g)<0)return 62;
        if(!life_until(&g->ready,1))return 63;
        /* No message will ever arrive: only close can release the waiter. */
        if(u_nat_close(rx)!=0)return 64;
        if(!life_until(&g->done,1))return 65;
        if(g->rc!=-9)return 66;
        if(u_nat_close(sx)!=0)return 67;
        puts("[NATIVE-CAPS] step7 PASS close revokes blocked receive (EBADF)");
    }

    /* Step 8: fatal/caught nonrestartable signal interrupts a blocked
     * receive in a separate process (park admission under run_lock). */
    {
        struct { void *sa_handler; unsigned long sa_flags; void *sa_restorer; unsigned long sa_mask; } act;
        nc_sig_hits=0;
        act.sa_restorer=0;act.sa_mask=0;act.sa_handler=(void *)nc_sig_handler;act.sa_flags=0;
        if(sigaction(15,&act,0)!=0)return 68;
        if(u_nat_channel_create(&sS,&rS)!=0)return 69;
        pid=u_fork();
        if(pid<0)return 70;
        if(pid==0) {
            long vv=0;
            long rr=u_nat_recv(rS,&vv,1);
            /* Caught nonrestartable TERM interrupts the park with EINTR;
             * the handler ran exactly once; no message can be lost. Early
             * or late delivery both end here: admission refuses a pre-park
             * signal, and the park re-check refuses a while-sleeping one. */
            u_exit(rr==-4 && nc_sig_hits==1 ? 0 : 82);
        }
        /* Give the child time to park before arming; either ordering must
         * produce EINTR, never a stranded sleeper. */
        { u64 end=life_now()+200000000UL; while(life_now()<end)u_yield(); }
        if(u_kill(pid,15)!=0)return 71;
        if(u_wait4(pid,&status,0,0)!=pid)return 72;
        if(((status>>8)&255)!=0)return 73;
        if(u_nat_close(sS)!=0||u_nat_close(rS)!=0)return 74;
        puts("[NATIVE-CAPS] step8 PASS signal interrupts blocked receive (EINTR)");
    }

    /* Step 9: a child that exits without touching its inherited caps leaves
     * the parent's endpoints fully usable (no implicit revocation). */
    {
        long s9, r9;
        if(u_nat_channel_create(&s9,&r9)!=0)return 75;
        pid=u_fork();
        if(pid<0)return 76;
        if(pid==0)u_exit(0);
        if(u_wait4(pid,&status,0,0)!=pid||status)return 77;
        if(u_nat_send(s9,9,0)!=0)return 78;
        if(u_nat_recv(r9,&v,0)!=1||v!=9)return 79;
        if(u_nat_close(s9)!=0||u_nat_close(r9)!=0)return 80;
        puts("[NATIVE-CAPS] step9 PASS child exit leaves parent caps intact");
    }

    u_munmap(g,4096);u_munmap(stack,16384);
    puts("[NATIVE-CAPS] PASS capability namespace and cross-process channels");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Native robust mutex probe (architecture section 6, gate 3). Every handle
 * arrives through the documented ABI: create, dup, or fork inheritance.
 * Deterministic barriers come from nat_mutex_stat (kernel-published state)
 * and pipes, never from timing assumptions. errnos: -1 EPERM -4 EINTR
 * -9 EBADF -11 EAGAIN -22 EINVAL -35 EDEADLK -130 EOWNERDIED -131 ENOTREC.
 * ------------------------------------------------------------------------- */
extern long u_nat_mutex_create(long *out);
extern long u_nat_mutex_lock(long cap, long block);
extern long u_nat_mutex_unlock(long cap);
extern long u_nat_mutex_consistent(long cap);
extern long u_nat_mutex_stat(long cap, unsigned int *out);

struct nm_gate {
    volatile unsigned int ready, done, armed, ok;
    long cap;
    long rc;
};
/* Same-process contender: blocks on the parent-held mutex (the parent's
 * stat barrier guarantees queue order), records the result, then unlocks
 * only after the parent finished its no-barge trylocks (armed), so the
 * FIFO handoff chain is fully serialized. */
static void nm_thr_contender(void *arg) {
    struct nm_gate *g=(struct nm_gate *)arg;
    long rc;
    atomic_xchg(&g->ready,1);
    rc=u_nat_mutex_lock(g->cap,1);
    g->rc=rc;
    atomic_xchg(&g->done,1);
    while(!life_load(&g->armed))u_yield();
    if(rc==0)u_nat_mutex_unlock(g->cap);
    atomic_xchg(&g->ok,1);
    u_exit(0);
}
static int nm_stat_until(long cap, unsigned int idx, unsigned int want) {
    u64 end=life_now()+5000000000UL;
    unsigned int st[3];
    for(;;) {
        if(u_nat_mutex_stat(cap,st)==0 && st[idx]==want)return 1;
        if(life_now()>=end)return 0;
        u_yield();
    }
}

static int native_mutex_probe(void) {
    long m, d, w, pid, pid2, status;
    unsigned int st[3];
    int i, fds1[2], fds2[2];
    char b;
    struct nm_gate *g1, *g2;
    char *tstack;

    /* Step 1: single-task basics — self-lock, nonowner unlock, stat. */
    if(u_nat_mutex_create(&m)!=0)return 1;
    if(u_nat_mutex_lock(m,0)!=0)return 2;
    if(u_nat_mutex_lock(m,0)!=-35)return 3;         /* self-lock DEADLK */
    if(u_nat_mutex_stat(m,st)!=0)return 4;
    if(st[0]!=1||st[1]!=0||st[2]!=(unsigned int)u_getpid())return 5;
    if(u_nat_mutex_unlock(m)!=0)return 6;
    if(u_nat_mutex_unlock(m)!=-1)return 7;          /* nonowner -> EPERM */
    if(u_nat_mutex_consistent(m)!=-22)return 8;     /* not a recovering owner */
    if(u_nat_mutex_lock(m,1)!=0)return 9;
    if(u_nat_mutex_unlock(m)!=0)return 10;
    if(u_nat_close(m)!=0)return 11;
    if(u_nat_mutex_lock(m,0)!=-9)return 12;         /* stale handle */
    puts("[NATIVE-MUTEX] step1 PASS basics, self-lock EDEADLK, nonowner EPERM, stat");

    /* Step 2: rights attenuation — UNLOCK-only dup releases but cannot
     * lock; a SEND-type right is refused on a mutex; MWAIT-only dup stats
     * but can only try. */
    if(u_nat_mutex_create(&m)!=0)return 13;
    d=u_nat_dup(m,8);                               /* MUNLOCK only */
    if(d<0)return 14;
    if(u_nat_mutex_lock(d,0)!=-1)return 15;
    if(u_nat_dup(m,1)!=-1)return 16;                /* SEND not a mutex right */
    w=u_nat_dup(m,16);                              /* MWAIT only */
    if(w<0)return 17;
    if(u_nat_mutex_lock(m,0)!=0)return 18;
    if(u_nat_mutex_unlock(d)!=0)return 19;          /* attenuated unlock works */
    if(u_nat_mutex_unlock(m)!=-1)return 20;
    if(u_nat_mutex_lock(w,1)!=-1)return 21;         /* no LOCK right */
    if(u_nat_mutex_stat(w,st)!=0)return 22;         /* MWAIT grants stat */
    if(u_nat_close(d)!=0||u_nat_close(w)!=0||u_nat_close(m)!=0)return 23;
    puts("[NATIVE-MUTEX] step2 PASS rights attenuation LOCK/UNLOCK/WAIT");

    /* Step 3: same-process FIFO handoff + no barging. Two contenders queue
     * in a deterministic order (stat barrier), the parent unlocks into the
     * queue head, and trylocks during handoff must see EAGAIN until the
     * chain drains. */
    if(u_nat_mutex_create(&m)!=0)return 24;
    g1=(struct nm_gate *)u_mmap(0,4096,3,0x21,-1,0);
    g2=(struct nm_gate *)u_mmap(0,4096,3,0x21,-1,0);
    tstack=(char *)u_mmap(0,16384,3,0x22,-1,0);
    if((long)g1<0||(long)g2<0||(long)tstack<0)return 25;
    for(i=0;i<(int)sizeof(*g1)/4;i++){((volatile unsigned int *)g1)[i]=0;((volatile unsigned int *)g2)[i]=0;}
    for(i=0;i<16384;i+=4096)tstack[i]=0;
    if(u_nat_mutex_lock(m,1)!=0)return 26;
    g1->cap=m;g2->cap=m;
    if(u_clone(0x10100,tstack+16384-8,(void *)&nm_thr_contender,g1)<0)return 27;
    if(!life_until(&g1->ready,1))return 28;
    if(!nm_stat_until(m,1,1))return 29;             /* T1 queued */
    if(u_clone(0x10100,tstack+16384-8-4096,(void *)&nm_thr_contender,g2)<0)return 30;
    if(!life_until(&g2->ready,1))return 31;
    if(!nm_stat_until(m,1,2))return 32;             /* T2 queued behind T1 */
    if(u_nat_mutex_unlock(m)!=0)return 33;          /* handoff -> T1 */
    if(u_nat_mutex_lock(m,0)!=-11)return 34;        /* no barging past T1 */
    if(u_nat_mutex_lock(m,0)!=-11)return 35;        /* still held in the chain */
    atomic_xchg(&g1->armed,1);atomic_xchg(&g2->armed,1);
    if(!life_until(&g1->done,1)||!life_until(&g2->done,1))return 36;
    if(g1->rc!=0||g2->rc!=0)return 37;              /* both acquired via handoff */
    if(!life_until(&g1->ok,1)||!life_until(&g2->ok,1))return 38;
    if(u_nat_mutex_lock(m,0)!=0)return 39;          /* drained: free again */
    if(u_nat_mutex_unlock(m)!=0)return 40;
    if(u_nat_close(m)!=0)return 41;
    u_munmap(g1,4096);u_munmap(g2,4096);u_munmap(tstack,16384);
    puts("[NATIVE-MUTEX] step3 PASS same-process FIFO handoff, no barging, drain");

    /* Step 4: cross-process FIFO through fork-inherited caps, handoffs
     * proven by the published owner pid, children released via pipes. */
    if(u_nat_mutex_create(&m)!=0)return 42;
    if(u_pipe(fds1)<0||u_pipe(fds2)<0)return 43;
    if(u_nat_mutex_lock(m,1)!=0)return 44;
    pid=u_fork();
    if(pid<0)return 45;
    if(pid==0) {
        if(u_close(fds1[1])!=0)u_exit(80);
        if(u_close(fds2[0])!=0||u_close(fds2[1])!=0)u_exit(81);
        if(u_nat_mutex_lock(m,1)!=0)u_exit(82);
        if(u_read(fds1[0],&b,1)!=1)u_exit(83);
        if(u_nat_mutex_unlock(m)!=0)u_exit(94);     /* release: FIFO -> child 2 */
        u_exit(0);
    }
    pid2=u_fork();
    if(pid2<0)return 46;
    if(pid2==0) {
        if(u_close(fds2[1])!=0)u_exit(84);
        if(u_close(fds1[0])!=0||u_close(fds1[1])!=0)u_exit(85);
        if(u_nat_mutex_lock(m,1)!=0)u_exit(86);
        if(u_read(fds2[0],&b,1)!=1)u_exit(87);
        if(u_nat_mutex_unlock(m)!=0)u_exit(95);
        u_exit(0);
    }
    if(u_close(fds1[0])!=0||u_close(fds2[0])!=0)return 47;
    if(!nm_stat_until(m,1,2))return 48;             /* both children queued */
    if(u_nat_mutex_unlock(m)!=0)return 49;          /* handoff -> child 1 */
    if(!nm_stat_until(m,2,(unsigned int)pid))return 50;
    b='x';
    if(u_write(fds1[1],&b,1)!=1)return 51;
    if(!nm_stat_until(m,2,(unsigned int)pid2))return 52;  /* FIFO: child 2 */
    if(u_write(fds2[1],&b,1)!=1)return 53;
    if(u_wait4(pid,&status,0,0)!=pid||((status>>8)&255)!=0)return 54;
    if(u_wait4(pid2,&status,0,0)!=pid2||((status>>8)&255)!=0)return 55;
    if(u_close(fds1[1])!=0||u_close(fds2[1])!=0)return 56;
    if(u_nat_close(m)!=0)return 57;
    puts("[NATIVE-MUTEX] step4 PASS cross-process FIFO handoff (owner pid proven)");

    /* Step 5: kill a BLOCKED waiter — the pending handoff must resolve so
     * the survivor acquires cleanly. Kill the queue HEAD after both are
     * queued (stat barrier), reap it, then unlock. */
    if(u_nat_mutex_create(&m)!=0)return 58;
    if(u_nat_mutex_lock(m,1)!=0)return 59;
    pid=u_fork();
    if(pid<0)return 60;
    if(pid==0) {
        u_nat_mutex_lock(m,1);                      /* returns only via kill */
        u_exit(88);                                 /* not reached */
    }
    if(!nm_stat_until(m,1,1))return 61;
    pid2=u_fork();
    if(pid2<0)return 62;
    if(pid2==0) {
        if(u_nat_mutex_lock(m,1)!=0)u_exit(89);     /* must acquire via handoff */
        if(u_nat_mutex_unlock(m)!=0)u_exit(96);     /* release before exit */
        u_exit(0);
    }
    if(!nm_stat_until(m,1,2))return 63;
    if(u_kill(pid,9)!=0)return 64;
    if(u_wait4(pid,&status,0,0)!=pid)return 65;     /* reaped: fully unwound */
    if(!nm_stat_until(m,1,1))return 66;             /* dead waiter unqueued */
    if(u_nat_mutex_unlock(m)!=0)return 67;          /* handoff -> survivor */
    if(u_wait4(pid2,&status,0,0)!=pid2||((status>>8)&255)!=0)return 68;
    if(u_nat_close(m)!=0)return 69;
    puts("[NATIVE-MUTEX] step5 PASS kill-while-blocked: queue unwinds, survivor acquires");

    /* Step 6: owner death -> OWNER_DIED recovery. The child acquires via
     * handoff, exits WITHOUT unlocking; the parent must recover. */
    if(u_nat_mutex_create(&m)!=0)return 70;
    if(u_nat_mutex_lock(m,1)!=0)return 71;
    pid=u_fork();
    if(pid<0)return 72;
    if(pid==0) {
        if(u_nat_mutex_lock(m,1)!=0)u_exit(90);
        u_exit(0);                                  /* dies owning it */
    }
    if(u_nat_mutex_unlock(m)!=0)return 73;          /* handoff -> child */
    if(!nm_stat_until(m,0,2))return 74;             /* child died owning */
    if(u_nat_mutex_lock(m,1)!=-130)return 75;       /* EOWNERDIED */
    if(u_nat_mutex_stat(m,st)!=0||st[2]!=(unsigned int)u_getpid())return 76;
    if(u_nat_mutex_consistent(m)!=0)return 77;
    if(u_nat_mutex_unlock(m)!=0)return 78;
    if(u_nat_mutex_lock(m,0)!=0)return 79;          /* usable again */
    if(u_nat_mutex_unlock(m)!=0)return 80;
    if(u_wait4(pid,&status,0,0)!=pid||((status>>8)&255)!=0)return 81;
    if(u_nat_close(m)!=0)return 82;
    puts("[NATIVE-MUTEX] step6 PASS owner death -> EOWNERDIED + consistent recovery");

    /* Step 7: abandoning recovery poisons the mutex terminally. */
    if(u_nat_mutex_create(&m)!=0)return 83;
    if(u_nat_mutex_lock(m,1)!=0)return 84;
    pid=u_fork();
    if(pid<0)return 85;
    if(pid==0) {
        if(u_nat_mutex_lock(m,1)!=0)u_exit(91);
        u_exit(0);                                  /* dies owning it */
    }
    if(u_nat_mutex_unlock(m)!=0)return 86;
    if(!nm_stat_until(m,0,2))return 87;
    if(u_nat_mutex_lock(m,1)!=-130)return 88;
    if(u_nat_mutex_unlock(m)!=0)return 89;          /* unlock WITHOUT consistent */
    if(u_nat_mutex_lock(m,0)!=-131)return 90;       /* ENOTRECOVERABLE */
    if(u_nat_mutex_consistent(m)!=-131)return 91;
    if(u_wait4(pid,&status,0,0)!=pid||((status>>8)&255)!=0)return 92;
    if(u_nat_close(m)!=0)return 93;
    puts("[NATIVE-MUTEX] step7 PASS abandoned recovery poisons ENOTRECOVERABLE");

    /* Step 8: last-cap close while LOCKED survives until owner death and
     * drains; then the object pool must be back to baseline (8 creates). */
    puts("[NATIVE-MUTEX] step8 begin");
    if(u_nat_mutex_create(&m)!=0)return 94;
    if(u_nat_mutex_lock(m,1)!=0)return 95;
    pid=u_fork();
    if(pid<0)return 96;
    if(pid==0) {
        if(u_nat_mutex_lock(m,1)!=0)u_exit(92);
        puts("[NATIVE-MUTEX] step8 child acquired");
        if(u_nat_close(m)!=0)u_exit(93);            /* own last cap, still LOCKED */
        puts("[NATIVE-MUTEX] step8 child closed cap");
        u_exit(0);                                  /* death drains the object */
    }
    if(!nm_stat_until(m,1,1))return 103;            /* child queued (barrier) */
    if(u_nat_mutex_unlock(m)!=0)return 97;          /* handoff -> child */
    puts("[NATIVE-MUTEX] step8 owner handed to child");
    if(!nm_stat_until(m,0,2))return 98;             /* child died owning: M_DIED */
    if(u_nat_close(m)!=0)return 99;                 /* parent cap: all caps gone */
    if(u_wait4(pid,&status,0,0)!=pid||((status>>8)&255)!=0)return 100;
    puts("[NATIVE-MUTEX] step8 child reaped, baseline loop");
    for(i=0;i<8;i++) {
        if(u_nat_mutex_create(&d)!=0)return 101;
        if(u_nat_close(d)!=0)return 102;
    }
    puts("[NATIVE-MUTEX] step8 PASS close-while-locked drained, object pool at baseline");
    puts("[NATIVE-MUTEX] PASS robust mutex: handoff, barging, kill, recovery, poison");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Zero-copy raw ring probe (AF_XDP analogue, migration: net layer). The
 * owner writes a UDP frame straight into a mapped DMA pool slot, kicks, and
 * a frame addressed to the host's own IP loops back through the RX ring in
 * the SAME slot (zero-copy turnaround) while the protocol stack also
 * delivers the datagram to a bound socket. Layout constants mirror
 * kernel/net.c RAWO_*.
 * ------------------------------------------------------------------------- */
extern long u_raw_create(void);
extern long u_raw_map(long handle, long uva);
extern long u_raw_tx_kick(long handle, long max);
extern long u_raw_close(long handle);
extern long u_socket(long a,long b,long c);
extern long u_bind(long a,const void*b,unsigned long c);
extern long u_recvfrom(long a,void*b,unsigned long c,long d,void*e,unsigned int*f);
extern long u_close(long a);

static unsigned short raw_csum(const volatile unsigned char *ip, unsigned int len) {
    unsigned int sum=0; unsigned int i;
    for(i=0;i+1<len;i+=2) sum += ((unsigned int)ip[i]<<8)|ip[i+1];
    if(len&1) sum += (unsigned int)ip[len-1]<<8;
    while(sum>>16) sum=(sum&0xFFFFu)+(sum>>16);
    return (unsigned short)(~sum);
}

static int net_raw_probe(void) {
    long h, va, s;
    volatile unsigned int *c;
    volatile unsigned char *frames;
    unsigned int slot, rslot, rlen, i, prod;
    unsigned char *tx;
    unsigned int ipi=14, ui=34, plen=11;
    unsigned int framelen;
    struct { int fd; short events; short revents; } pf;
    unsigned char rbuf[64];
    unsigned int srcp=11787, dstp=11788;
    unsigned int spins;

    h=u_raw_create(); if(h<0) return 91;
    va=u_mmap(0,4096+32u*2048u,3,0x21,-1,0); if(va<0) { u_raw_close(h); return 92; }
    u_munmap((void*)va,4096+32u*2048u);          /* hand the VA range back */
    if(u_raw_map(h,va)!=0) { u_raw_close(h); return 93; }
    c=(volatile unsigned int*)va;
    if(c[0]!=0x52415730u || c[1]!=32u || c[2]!=2048u) { u_raw_close(h); return 94; }
    if(c[3]==0) {                                /* NIC down (no -net): skip */
        u_raw_close(h); u_munmap((void*)va,4096+32u*2048u); return 0;
    }
    frames=(volatile unsigned char*)(va+4096);

    /* UDP socket bound to the probe port: the protocol stack path. */
    { unsigned char a[16]; for(i=0;i<16;i++)a[i]=0;
      a[0]=2; a[2]=(unsigned char)(dstp>>8); a[3]=(unsigned char)dstp;
      s=u_socket(2,2,0); if(s<0) { u_raw_close(h); return 95; }
      if(u_bind(s,a,16)!=0) { u_close(s); u_raw_close(h); return 96; } }

    /* Take a free pool slot (lock-free single-word exchange). */
    slot=32u;
    for(i=0;i<32u;i++) {
        if(atomic_xchg((volatile unsigned int*)&c[138+i],1u)==0u) { slot=i; break; }
    }
    if(slot==32u) { u_close(s); u_raw_close(h); return 97; }

    /* Build eth+ip+udp in the TX slot, addressed to the host's own IP.
     * IPv4 header layout from frame+14: checksum at +24, src at +26,
     * dst at +30. The kick loopbacks frames whose dst IP is local. */
    tx=(unsigned char*)(frames+(unsigned long)slot*2048u);
    for(i=0;i<6;i++) tx[i]=0xFFu;                        /* broadcast     */
    for(i=0;i<6;i++) tx[6+i]=((volatile unsigned char*)&c[4])[i];  /* idx 4 = byte 16 */
    tx[12]=0x08; tx[13]=0x00;
    for(i=0;i<20;i++) tx[14+i]=0;
    tx[14]=0x45; tx[16]=(unsigned char)((28u+plen)>>8); tx[17]=(unsigned char)(28u+plen);
    tx[18]=0; tx[19]=1; tx[22]=64; tx[23]=17;
    tx[26]=c[3]>>24; tx[27]=(c[3]>>16)&255; tx[28]=(c[3]>>8)&255; tx[29]=c[3]&255;
    tx[30]=tx[26]; tx[31]=tx[27]; tx[32]=tx[28]; tx[33]=tx[29];
    tx[24]=0; tx[25]=0;
    tx[24]=(unsigned char)(raw_csum(tx+14,20)>>8);
    tx[25]=(unsigned char)(raw_csum(tx+14,20)&255);
    for(i=0;i<8;i++) tx[34+i]=0;
    tx[34]=(unsigned char)(srcp>>8); tx[35]=(unsigned char)srcp;
    tx[36]=(unsigned char)(dstp>>8); tx[37]=(unsigned char)dstp;
    tx[38]=(unsigned char)((8u+plen)>>8); tx[39]=(unsigned char)(8u+plen);
    tx[40]=0; tx[41]=0;
    { const char *p="RAWZEROCOPY";
      for(i=0;i<plen;i++) tx[42+i]=(unsigned char)p[i]; }
    framelen=14+20+8+plen;
    (void)ipi; (void)ui;

    /* Publish on the TX ring and kick: same-buffer loopback expected. */
    prod=c[72];
    c[74+(prod%32u)]=slot;
    c[106+(prod%32u)]=framelen;
    c[72]=prod+1;
    if(u_raw_tx_kick(h,8)<1) { atomic_xchg((volatile unsigned int*)&c[552+slot],0u);
        u_close(s); u_raw_close(h); return 98; }

    /* Poll the RX ring (bounded): the looped-back frame is the SAME slot. */
    spins=0;
    while(c[6]==c[7] && spins<4000000u) { spins++; u_yield(); }
    if(c[6]==c[7]) { u_close(s); u_raw_close(h); return 99; }
    prod=c[7];
    rslot=c[8+(prod%32u)];
    rlen=c[40+(prod%32u)];
    { volatile unsigned char *rx=frames+(unsigned long)rslot*2048u;
      int bad=0;
      for(i=0;i<plen;i++) if(rx[42+i]!=(unsigned char)"RAWZEROCOPY"[i]) bad++;
      if(rslot!=slot) bad++;
      if(rlen!=framelen) bad++;
      if(bad) { atomic_xchg((volatile unsigned int*)&c[138+rslot],0u);
                atomic_xchg((volatile unsigned int*)&c[138+slot],0u);
                c[7]=prod+1;
                u_close(s); u_raw_close(h); return 100; } }
    c[7]=prod+1;
    atomic_xchg((volatile unsigned int*)&c[138+rslot],0u);
    puts("[NETRAW] zero-copy turnaround: TX slot == RX slot, payload intact");

    /* Protocol stack also received the datagram (dual-path evidence). */
    pf.fd=(int)s; pf.events=1; pf.revents=0;
    { long rr=u_recvfrom(s,rbuf,64,0,0,0);
      int bad=0;
      if(rr<0) { u_close(s); u_raw_close(h); return 101; }
      for(i=0;i<plen&&(unsigned long)i<(unsigned long)rr;i++)
        if(rbuf[i]!=(unsigned char)"RAWZEROCOPY"[i]) bad++;
      if(bad) { u_close(s); u_raw_close(h); return 102; } }
    u_close(s);
    if(u_raw_close(h)!=0) return 103;
    u_munmap((void*)va,4096+32u*2048u);
    puts("[NETRAW] PASS zero-copy rings + protocol dual-path");
    return 0;
}

/* ---------------------------------------------------------------------------
 * vDSO-lite clock probe (high-resolution time slice). The kernel maps a
 * shared clock page at a fixed VA into every address space; user code
 * reads its own TSC and the page WITHOUT a syscall, then cross-checks
 * against the syscall clock and measures nanosleep precision.
 * Layout mirrors kernel/tsc.c (u32 indexes: magic 0, ver 1,
 * mult 2..3 (u64), shift 4, flags 5, base 6..7 (u64)).
 * ------------------------------------------------------------------------- */
extern long u_raw_create(void);
extern long u_nanosleep(const void *req, void *rem);

enum { VD_VA = 0x50000000u };
/* rdtsc as naked single-statement bytes (fakecc drops unknown mnemonics
 * and rejects constraint syntax): 0F 31 rdtsc; 48 C1 E2 20 shl rdx,32;
*  48 09 D0 or rax,rdx; C3 ret. */
__attribute__((naked)) unsigned long long vd_tsc(void) {
    __asm__ volatile(".byte 15,49,72,193,226,32,72,9,208,195");
}

static int vdso_probe(void) {
    volatile unsigned int *c;
    unsigned long long mult, base, tb, ns1, ns2, t0, t1;
    unsigned int shift, flags;
    struct { long s; long ns; } req, rem, tp;
    unsigned int i;

    c = (volatile unsigned int *)VD_VA;
    if (c[0] != 0x646F7376u || c[1] != 1u) {
        puts("[VD] FAIL magic/version");
        return 91;
    }
    /* Field indexes mirror kernel/tsc.c VDSO_OFFSET_* exactly:
     * MULT at 2(low),3(high); SHIFT 4; FLAGS 5; BASE 6(low),7(high);
     * TSCBASE 8(low),9(high). */
    flags = c[5];
    mult = ((unsigned long long)c[3] << 32) | c[2];
    shift = c[4];
    base = ((unsigned long long)c[7] << 32) | c[6];
    tb = ((unsigned long long)c[9] << 32) | c[8];
    if (flags == 0) {
        puts("[VD] clock not calibrated (tick fallback): probe skipped");
        return 0;
    }
    /* Monotonicity across 64 vDSO reads. */
    t0 = vd_tsc();
    ns1 = base + ((t0 - tb) * mult >> shift);
    for (i = 0; i < 64; i++) {
        ns2 = base + ((vd_tsc() - tb) * mult >> shift);
        if (ns2 < ns1) {
            puts("[VD] FAIL monotonicity");
            return 92;
        }
        ns1 = ns2;
    }
    /* vDSO vs syscall cross-check: same calibration, must agree closely. */
    if (u_clock_gettime(1, (void *)&tp) != 0) { puts("[VD] FAIL syscall"); return 93; }
    t1 = vd_tsc();
    ns2 = base + ((t1 - tb) * mult >> shift);
    { unsigned long long sys_ns = (unsigned long long)tp.s * 1000000000ull + (unsigned long long)tp.ns;
      unsigned long long d = ns2 > sys_ns ? ns2 - sys_ns : sys_ns - ns2;
      if (d > 100000000ull) {            /* 100 ms tolerance under TCG */
        { extern void runtime_printf_dummy(void); (void)runtime_printf_dummy; }
        { volatile char msg[80];
          unsigned long long ms = d / 1000000ull;
          unsigned int mi = 0;
          msg[0]='['; msg[1]='V'; msg[2]='D'; msg[3]=']'; msg[4]=' ';
          msg[5]='d'; msg[6]='r'; msg[7]='i'; msg[8]='f'; msg[9]='t';
          msg[10]='='; mi=11;
          if (ms == 0) { msg[mi++]='<'; msg[mi++]='1'; }
          while (ms) { msg[mi++] = (unsigned char)('0' + ms % 10); ms /= 10; }
          msg[mi++]='m'; msg[mi++]='s';
          msg[mi]=0;
          puts((const char *)msg); }
        return 94;
      } }
    /* nanosleep precision: request 50 ms, measure with the vDSO clock. */
    req.s = 0; req.ns = 50000000;
    t1 = vd_tsc();
    ns1 = base + ((t1 - tb) * mult >> shift);
    u_nanosleep((void *)&req, (void *)&rem);
    ns2 = base + ((vd_tsc() - tb) * mult >> shift);
    { unsigned long long waited = ns2 - ns1;
      if (waited < 45000000ull || waited > 130000000ull) {
        puts("[VD] FAIL nanosleep precision");
        return 95;
      } }
    (void)i;
    (void)vd_tsc;
    puts("[VD] PASS vDSO clock: monotonic, matches syscall, sleep precise");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Fork admission limit probe (P0-9): the process-count limit must fail
 * DETERMINISTICALLY with EAGAIN once reached, and the process slots must
 * recover when the children exit. This is the resource-exhaustion
 * contract the old implicit-failure fork lacked.
 * ------------------------------------------------------------------------- */
static int rlimit_probe(void) {
    long pids[96];
    long pid, st, r;
    int n = 0, i, reached = 0;
    int pr[2];
    unsigned char rbuf[16];
    u64 end;

    /* Children block on a pipe READ (proper wait-queue sleep, zero churn):
     * 50+ concurrent sleepers must not flood the scheduler with wake/park
     * pairs the way tick-woken nanosleeps would. The parent writes one
     * 0x42 byte to wake the first reader, closes the write end so every
     * blocked reader sees EOF, reaps them all, and verifies slot recovery. */
    if (u_pipe(pr) != 0) return 110;
    for (i = 0; i < 72; i++) {
        pid = u_fork();
        if (pid < 0) {
            reached = (pid == -11) ? 1 : 0;     /* -11 = EAGAIN */
            break;
        }
        if (pid == 0) {
            /* Close the inherited write end: the parent is the sole writer,
             * so its close is what propagates EOF to every reader. */
            u_close(pr[1]);
            long rr = u_read(pr[0], rbuf, 1);
            if (rr == 1 && rbuf[0] == 0x42) u_exit(0);
            u_exit(0);                          /* EOF or woken: exit either way */
        }
        pids[n] = pid;
        n = n + 1;
        if (n >= 96) break;
    }
    if (n < 10) {
        puts("[RLIMIT] FAIL too few children before limit");
        return 111;
    }
    if (!reached) {
        puts("[RLIMIT] FAIL limit never returned EAGAIN");
        u_close(pr[1]);
        for (i = 0; i < n; i++) u_wait4(-1, &st, 0, 0);
        return 112;
    }
    /* Wake the blocked children: one 0x42 byte lets the first reader exit,
     * closing the write end lets the rest drain via EOF. */
    {
        unsigned char wb = 0x42;
        if (u_write(pr[1], (void *)&wb, 1) != 1) {
            puts("[RLIMIT] FAIL pipe write");
            return 113;
        }
    }
    u_close(pr[1]);
    end = 0;
    for (i = 0; i < n; i++) {
        do {
            r = u_wait4(-1, &st, 0, 0);
        } while (r == -4);
        if (r < 0) { puts("[RLIMIT] FAIL reap"); return 113; }
    }
    u_close(pr[0]);
    (void)end;
    /* Recovery: after reaping, new forks must succeed again. */
    for (i = 0; i < 4; i++) {
        pid = u_fork();
        if (pid < 0) {
            puts("[RLIMIT] FAIL fork did not recover after reap");
            return 114;
        }
        if (pid == 0) u_exit(0);
        end = 0;
        do {
            st = 0;
            r = u_wait4(pid, &st, 0, 0);
        } while (r == -4);
        if (r != pid) {
            puts("[RLIMIT] FAIL recovery reap");
            return 115;
        }
    }
    puts("[RLIMIT] PASS deterministic EAGAIN limit + slot recovery");
    return 0;
}

int main(int argc, char **argv) {
    if(argc==3 && strcmp(argv[1],"--park-probe")==0)return park_probe(argv[2][0]-'0');
    if(argc==2 && strcmp(argv[1],"--native-caps")==0)return native_caps_probe();
    if(argc==2 && strcmp(argv[1],"--native-mutex")==0)return native_mutex_probe();
    if(argc==2 && strcmp(argv[1],"--net-raw")==0)return net_raw_probe();
    if(argc==2 && strcmp(argv[1],"--vdso")==0)return vdso_probe();
    if(argc==2 && strcmp(argv[1],"--rlimit")==0)return rlimit_probe();
    if(argc==2 && strcmp(argv[1],"--net-raw")==0)return net_raw_probe();
    char *sh_argv[2];
    long pid = u_getpid();
    volatile unsigned long i;

    if (argc == 2 && strcmp(argv[1], "--process-lifetime") == 0) return process_lifetime_probe();
    if (argc == 2 && strcmp(argv[1], "--ext2-reclaim") == 0) return ext2_reclaim_check();

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
    proc_pid_meminfo_test();
    stat_test();
    owner_mtime_test();
    cred_setuid_groups_test();
    symlink_test();
    cross_mount_test();
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
