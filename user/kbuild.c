/* fakeos in-OS bootstrap orchestrator (milestone 5).
 *
 * Runs entirely inside fakeos:
 *   1. smoke-tests the compiler->execve chain on ping.c.
 *   2. Compiles every shipped kernel C module with /bin/fakecc and byte
 *      compares the produced object against the host-generated reference in /ref.
 *   3. Invokes /bin/ldfake to link the rebuilt kernel image.
 *
 * fakecc's package root is supplied through /proc/self/environ
 * (FAKECC_PKG=/src); with cwd /src the input-file directory is also a
 * package search root.
 *
 * FakeOS user program: links against the minimal fakeos libc. */
package user;

extern long u_chdir(const char *path);
extern long u_open(const char *path, long flags, long mode);
extern long u_close(long fd);
extern long u_read(long fd, void *buf, unsigned long len);
extern long u_write(long fd, const void *buf, unsigned long len);
extern long u_fork(void);
extern long u_execve(const char *path, char **argv, char **envp);
extern long u_wait4(long pid, long *status, long options, long *rusage);
extern void u_exit(long code);

extern void printf(const char *fmt, ...);
extern void puts(const char *s);
extern void puterr(const char *s);
extern void *malloc(unsigned long size);
extern void free(void *ptr);
extern void memset(void *dst, int c, unsigned long n);
extern void strcpy(char *dst, const char *src);
extern unsigned long strlen(const char *s);

enum { O_RDONLY = 0, READ_CAP = 65536,
       CC_MAX_FLAGS = 2, CC_FLAG_LEN = 40 };

static unsigned long read_file(const char *path, char *buf);

/* Extra flags the staged in-OS fakecc understands. The build publishes
 * /etc/fakecc.flags (one space-separated line): multi-target builds list
 * "--target=x86_64-linux", single-backend builds ship an empty file. */
static char cc_flags[CC_MAX_FLAGS][CC_FLAG_LEN];
static unsigned long n_cc_flags;

static void load_cc_flags(void) {
    char buf[128];
    unsigned long len = read_file("/etc/fakecc.flags", buf);
    unsigned long i = 0;
    n_cc_flags = 0;
    while (i < len && n_cc_flags < CC_MAX_FLAGS) {
        unsigned long n = 0;
        while (i < len && (buf[i] == ' ' || buf[i] == '\t' ||
                           buf[i] == '\n' || buf[i] == '\r')) {
            i++;
        }
        while (i < len && buf[i] != ' ' && buf[i] != '\t' &&
               buf[i] != '\n' && buf[i] != '\r' && n < CC_FLAG_LEN - 1) {
            cc_flags[n_cc_flags][n++] = buf[i++];
        }
        if (n) {
            cc_flags[n_cc_flags][n] = 0;
            n_cc_flags++;
        }
    }
}

struct mod_entry {
    const char *pkg;
    const char *name;
};

/* All shipped kernel C modules (pkg directory + file stem), matching
 * the host Makefile's object list. */
static const struct mod_entry modules[] = {
    { "types",  "types" },
    { "arch",   "arch" },
    { "arch",   "gdt" },
    { "arch",   "idt" },
    { "arch",   "lapic" },
    { "drivers", "uart" },
    { "drivers", "vga" },
    { "kernel",  "kprintf" },
    { "kernel",  "isr" },
    { "mem",    "pmm" },
    { "mem",    "vmm" },
    { "mem",    "slab" },
    { "mem",    "as" },
    { "fs",     "ramfs" },
    { "kernel",  "memtest" },
    { "kernel",  "sched" },
    { "kernel",  "proctest" },
    { "kernel",  "syscall" },
    { "kernel",  "sysfile" },
    { "kernel",  "sysproc" },
    { "kernel",  "proc" },
    { "kernel",  "tty" },
    { "kernel",  "uproc" },
    { "kernel",  "rootfs" },
    { "kernel",  "vfstest" },
    { "kernel",  "kmain" }
};

/* fork/execve a child and reap it; returns the child's exit status
 * (0..255) or -1 on failure. */
static long run_child(const char *path, char **argv) {
    long pid = u_fork();
    if (pid == 0) {
        long r = u_execve(path, argv, 0);
        puterr("kbuild: exec failed");
        u_exit(r < 0 ? 126 : 0);
    }
    long status = 0;
    if (u_wait4(pid, &status, 0, 0) < 0) {
        return -1;
    }
    return status;
}

/* Read a whole file into a static buffer. Returns the byte count; 0 means
 * missing/empty. Largest kernel object is well under READ_CAP. */
static unsigned long read_file(const char *path, char *buf) {
    long fd = u_open(path, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    unsigned long len = 0;
    for (;;) {
        long r = u_read(fd, buf + len, READ_CAP - len);
        if (r <= 0) {
            break;
        }
        len += (unsigned long)r;
    }
    u_close(fd);
    return len;
}

/* Append "<path> " to a char buffer used as the ldfake response file. */
static void add_arg(char *buf, unsigned long *used, const char *path) {
    unsigned long k = 0;
    while (path[k]) {
        buf[(*used)++] = path[k];
        k++;
    }
    buf[(*used)++] = ' ';
}

static void build_path(char *dst, const char *dir, const char *name,
                    const char *suf) {
    unsigned long k = 0;
    unsigned long i;
    for (i = 0; dir[i]; i++) {
        dst[k++] = dir[i];
    }
    for (i = 0; name[i]; i++) {
        dst[k++] = name[i];
    }
    for (i = 0; suf[i]; i++) {
        dst[k++] = suf[i];
    }
    dst[k] = 0;
}

int main(void) {
    long st;
    u_chdir("/src");
    load_cc_flags();

    puts("[kbuild] stage a: compiler/exec chain on ping.c");

    /* argv: fakecc [extra flags...] ping.c -o /tmp/ping (max 8 slots) */
    char *ping_cc[4 + CC_MAX_FLAGS];
    {
        unsigned long k = 0;
        unsigned long j;
        ping_cc[k++] = "/bin/fakecc";
        for (j = 0; j < n_cc_flags; j++) {
            ping_cc[k++] = cc_flags[j];
        }
        ping_cc[k++] = "ping.c";
        ping_cc[k++] = "-o";
        ping_cc[k++] = "/tmp/ping";
        ping_cc[k] = 0;
    }
    st = run_child("/bin/fakecc", ping_cc);
    if (st != 0) {
        printf("[kbuild] FAIL ping compile exit=%d\n", st);
        return 1;
    }
    char *ping_run[] = { "/tmp/ping", 0 };
    st = run_child("/tmp/ping", ping_run);
    if (st != 0) {
        printf("[kbuild] FAIL ping run exit=%d\n", st);
        return 1;
    }
    puts("[kbuild] ping compile+run OK");

    unsigned long nmod = 26;
    unsigned long ok = 0;
    char got_buf[READ_CAP];
    char ref_buf[READ_CAP];
    for (unsigned long i = 0; i < nmod; i++) {
        const char *pkg = modules[i].pkg;
        const char *nm = modules[i].name;

        char src[72];
        char tmpo[40];
        char refp[40];
        memset(src, 0, 72);
        memset(tmpo, 0, 40);
        memset(refp, 0, 40);
        build_path(src, "/src/", pkg, "");   /* dir + pkg, no suffix yet */
        /* append '/' + name + ".c" */
        {
            unsigned long k = strlen(src);
            src[k++] = '/';
            unsigned long j;
            for (j = 0; nm[j]; j++) { src[k++] = nm[j]; }
            src[k++] = '.';
            src[k++] = 'c';
            src[k] = 0;
        }
        build_path(tmpo, "/tmp/", nm, ".o");
        /* /ref mirrors the source package layout: /ref/<pkg>/<n>.o */
        {
            unsigned long k = 0;
            unsigned long j;
            const char *pre = "/ref/";
            while (pre[k]) { refp[k] = pre[k]; k++; }
            for (j = 0; pkg[j]; j++) { refp[k++] = pkg[j]; }
            refp[k++] = '/';
            for (j = 0; nm[j]; j++) { refp[k++] = nm[j]; }
            refp[k++] = '.';
            refp[k++] = 'o';
            refp[k] = 0;
        }

        /* argv: fakecc [extra flags...] -c -O0 src -o tmpo (max 8) */
        char *cc[7 + CC_MAX_FLAGS];
        {
            unsigned long k = 0;
            unsigned long j;
            cc[k++] = "/bin/fakecc";
            for (j = 0; j < n_cc_flags; j++) {
                cc[k++] = cc_flags[j];
            }
            cc[k++] = "-c";
            cc[k++] = "-O0";
            cc[k++] = src;
            cc[k++] = "-o";
            cc[k++] = tmpo;
            cc[k] = 0;
        }
        st = run_child("/bin/fakecc", cc);
        if (st != 0) {
            printf("[kbuild] FAIL %s/%s compile exit=%d\n",
                    (long)pkg, (long)nm, st);
            return 1;
        }

        unsigned long got_n = read_file(tmpo, got_buf);
        unsigned long ref_n = read_file(refp, ref_buf);
        if (got_n == 0 || ref_n == 0) {
            printf("[kbuild] FAIL %s/%s missing object (got %u ref %u)\n",
                    (long)pkg, (long)nm, got_n, ref_n);
            return 1;
        }
        if (got_n != ref_n) {
            printf("[kbuild] FAIL %s/%s size: got %u ref %u\n",
                    (long)pkg, (long)nm, got_n, ref_n);
            return 1;
        }
        unsigned long bad = (unsigned long)-1;
        for (unsigned long j = 0; j < got_n; j++) {
            if (got_buf[j] != ref_buf[j]) {
                bad = j;
                break;
            }
        }
        if (bad != (unsigned long)-1) {
            printf("[kbuild] FAIL %s/%s first byte diff at offset %u\n",
                    (long)pkg, (long)nm, bad);
            return 1;
        }
        ok++;
        printf("[kbuild] identical %s/%s.o (%u bytes)\n",
                    (long)pkg, (long)nm, got_n);
    }
    printf("[kbuild] all %u kernel objects byte-identical\n", ok);

    /* Final link. execve passes at most 8 argv slots, so the
     * object list goes through an @response file:
     *   ldfake -o /tmp/fakeos-new @/tmp/ldfake.args
     * boot.o leads (low-address multiboot entry); then the 26 C
     * objects, the other 4 nasm objects, and the userblob + empty
     * m5-blob stub. */
    {
        long lfd2 = u_open("/tmp/ldfake.args", 0x42, 0x1FF);
        char line[1024];
        unsigned long used = 0;
        unsigned long i;
        if (lfd2 < 0) {
            puts("[kbuild] FAIL cannot write /tmp/ldfake.args");
            return 1;
        }
        add_arg(line, &used, "/asm/boot.o");
        for (i = 0; i < nmod; i++) {
            char p[32];
            build_path(p, "/tmp/", modules[i].name, ".o");
            add_arg(line, &used, p);
        }
        add_arg(line, &used, "/asm/desc.o");
        add_arg(line, &used, "/asm/io.o");
        add_arg(line, &used, "/asm/sysentry.o");
        add_arg(line, &used, "/asm/switch.o");
        add_arg(line, &used, "/asm/userblob.o");
        add_arg(line, &used, "/asm/m5stub.o");
        u_write(lfd2, line, used);
        u_close(lfd2);

        char *argv[6];
        argv[0] = "/bin/ldfake";
        argv[1] = "-o";
        argv[2] = "/tmp/fakeos-new";
        argv[3] = "@/tmp/ldfake.args";
        argv[4] = 0;
        st = run_child("/bin/ldfake", argv);
        if (st != 0) {
            printf("[kbuild] FAIL ldfake exit=%d\n", st);
            return 1;
        }
        puts("[kbuild] kernel image linked: /tmp/fakeos-new");
    }
    return 0;
}
