/* fakeos userland - minimal init process.
 *
 * Linked at USER_VMA (0x400000) as a 64-bit ELF, embedded into the kernel and
 * mapped into a fresh address space per process. No libc: all output goes
 * through raw SYSCALL wrappers (usys.asm). Exercises:
 *   - getpid / write / yield / fork / exit
 *   - COW: an inherited .data variable is diverged by the child; the parent's
 *     copy must stay intact
 *   - timer preemption: a busy loop long enough to be interrupted at ring 3
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long len);
extern void u_exit(long code);
extern long u_getpid(void);
extern long u_yield(void);
extern long u_fork(void);

int g_shared_marker = 0x1234;   /* .data: inherited by fork, then COW-diverged */

static void puts1(const char *s) {
    unsigned long n = 0;
    while (s[n] != 0) {
        n++;
    }
    u_write(1, s, n);
}

/* Print "prefix" + single-digit number + "\n" as ONE write syscall so lines
 * from preempted processes never interleave mid-line. */
static void print_num_line(const char *prefix, long v) {
    char buf[80];
    unsigned long n = 0;
    while (prefix[n] != 0 && n < 70) {
        buf[n] = prefix[n];
        n++;
    }
    long x = v;
    if (x < 0) {
        buf[n++] = '-';
        x = -x;
    }
    char digits[20];
    int nd = 0;
    do {
        digits[nd++] = (char)('0' + (x % 10));
        x = x / 10;
    } while (x != 0);
    while (nd > 0) {
        buf[n++] = digits[--nd];
    }
    buf[n++] = '\n';
    u_write(1, buf, n);
}

void umain(void) {
    long pid = u_getpid();
    print_num_line("[U] hello from ring 3, pid=", pid);

    /* Busy loop long enough to be preempted by the APIC timer at ring 3,
     * proving timer preemption of user threads (checked on the kernel side
     * by counting user-mode ticks). volatile so fakecc -O0 keeps the loop. */
    volatile unsigned long i;
    for (i = 0; i < 3000000UL; i++) {
    }

    long child = u_fork();
    if (child == 0) {
        /* Child: diverge the inherited .data variable. The write faults on
         * the COW page and must private-copy it in the child's space only. */
        g_shared_marker = 0x5678;
        if (g_shared_marker == 0x5678) {
            puts1("[U][PASS] child COW write took effect\n");
        } else {
            puts1("[U][FAIL] child COW write lost\n");
        }
        print_num_line("[U] child of pid ", pid);
        u_exit(0);
    }

    /* Parent: our copy must be untouched by the child's COW write. */
    if (child > 0 && g_shared_marker == 0x1234) {
        puts1("[U][PASS] parent COW value intact after child write\n");
    } else {
        puts1("[U][FAIL] parent COW value clobbered\n");
    }
    u_yield();
    print_num_line("[U] parent pid ", pid);
    u_exit(0);
}
