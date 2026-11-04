package kernel;
import fs;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t s32;
typedef types.int64_t i64;

/* ---------------------------------------------------------------------------
 * Memory and (later) process-life syscalls: brk / mmap.
 * wait4 and execve join this file in their slices.
 * ------------------------------------------------------------------------- */

enum {
    MAP_SHARED    = 0x01,
    MAP_PRIVATE   = 0x02,
    MAP_FIXED     = 0x10,
    MAP_ANONYMOUS = 0x20,

    ENOSYS_NEG    = (u64)-38
};

extern u64 sched_current_as_h(void);
extern u32 sched_current_pid(void);
extern u32 sched_current_tid(void);
extern void sched_set_clear_child_tid(u64 addr);
extern u32 sched_fd_get(u32 idx);
extern u32 pipe_fh_is(u32 fh);
extern u32 sock_fh_is(u32 fh);
extern void sched_sleep(u64 chan);
extern u32 sched_wakeup_n(u64 chan, u32 n);
extern u32 sched_should_eintr(void);

/* Fill one page for a VMA_FILE fault. Returns 1 on success (partial/EOF OK). */

/* Write one dirty MAP_SHARED file page back to its inode. */
u32 as_file_page_writeback(u32 fs_kind, u32 ino, u64 file_off, u64 page_kva) {
    const char *src = (const char *)page_kva;
    if (ino == 0 || src == 0) {
        return 0;
    }
    if (fs_kind == 1) {
        u64 n = fs.ext2_ino_pwrite(ino, file_off, src, 4096);
        return (i64)n >= 0 ? 1 : 0;
    }
    {
        u64 n = fs.fs_ino_pwrite(ino, file_off, src, 4096);
        return (i64)n >= 0 ? 1 : 0;
    }
}

u32 as_file_page_fill(u32 fs_kind, u32 ino, u64 file_off, u64 page_kva) {
    char *dst = (char *)page_kva;
    u32 i;
    for (i = 0; i < 4096; i++) {
        dst[i] = 0;
    }
    if (fs_kind == 1) {
        /* Ext2 */
        u64 n = fs.ext2_ino_pread(ino, file_off, dst, 4096);
        if ((i64)n < 0) {
            return 0;
        }
        return 1;
    }
    /* Ramfs: direct inode data window. */
    {
        u64 sz = fs.fs_ino_size(ino);
        u64 src = fs.fs_ino_data(ino);
        u64 n;
        u64 k;
        if (src == 0 && sz != 0) {
            return 0;
        }
        if (file_off >= sz) {
            return 1; /* past EOF: leave zeros */
        }
        n = sz - file_off;
        if (n > 4096) {
            n = 4096;
        }
        for (k = 0; k < n; k++) {
            dst[k] = ((const char *)src)[file_off + k];
        }
        return 1;
    }
}

/* void *mmap(addr, len, prot, flags, fd, off):
 *  - MAP_PRIVATE|MAP_ANONYMOUS: demand-zero window (PROT_NONE reserved OK)
 *  - MAP_SHARED|MAP_ANONYMOUS: shm-style shared anon (keep W across fork)
 *  - MAP_PRIVATE / MAP_SHARED + regular file: VMA_FILE demand paging
 *    (MAP_SHARED shares frames across fork; dirty pages write back on munmap).
 *  - MAP_FIXED: anon (ASAN) or file at a caller-chosen page-aligned VA;
 *    overlaps rejected (no silent unmap / no MAP_FIXED replace).
 *  - Non-zero addr without MAP_FIXED is a hint (try VA, else kernel pick).
 * off must be page-aligned for file maps. */
u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off) {
    u64 va;
    u64 as;
    u32 anon;
    u32 shared;
    u32 priv;
    if (len == 0) {
        return (u64)(-1);
    }
    shared = (flags & MAP_SHARED) != 0;
    priv = (flags & MAP_PRIVATE) != 0;
    if (shared == priv) {
        /* Exactly one of SHARED/PRIVATE required (Linux allows neither as
         * EINVAL; both is also invalid). */
        return (u64)(-1);
    }
    anon = (flags & MAP_ANONYMOUS) != 0;
    as = sched_current_as_h();
    if (as == 0) {
        return (u64)(-1);
    }

    if (anon) {
        if (off != 0) {
            return (u64)(-1);
        }
        /* fd is an int in the Linux ABI; a 32-bit -1 moved by user code may
         * arrive as 0x00000000'FFFFFFFF rather than a sign-extended register. */
        if ((s32)(fd & 0xFFFFFFFFULL) != -1) {
            return (u64)(-1);
        }
        if (flags & MAP_FIXED) {
            /* ASAN shadow stays MAP_PRIVATE; reject SHARED+FIXED for now. */
            if (shared) {
                return (u64)(-1);
            }
            va = mem.as_mmap_fixed_h(as, addr, len, (u32)prot);
            return va == 0 ? (u64)(-1) : va;
        }
        /* addr==0 => auto; addr!=0 => POSIX hint (fallback on overlap). */
        va = mem.as_mmap_anon_h(as, addr, len, (u32)prot, shared);
        return va == 0 ? (u64)(-1) : va;
    }

    /* File-backed: true VMA_FILE demand paging (MAP_FIXED + addr hint). */
    {
        u32 fh;
        u32 ino;
        u32 fs_kind;
        u32 is_fixed;
        u64 alen;
        u64 req_addr;
        if ((off & 0xFFF) != 0) {
            return (u64)(-1);
        }
        is_fixed = (flags & MAP_FIXED) != 0 ? 1 : 0;
        if (is_fixed) {
            if (addr == 0 || (addr & 0xFFF) != 0) {
                return (u64)(-1);
            }
            req_addr = addr;
        } else {
            req_addr = addr;         /* 0 = auto, else hint */
        }
        if ((s32)(fd & 0xFFFFFFFFULL) < 0 || (fd & 0xFFFFFFFFULL) >= 16) {
            return (u64)(-1);
        }
        fh = sched_fd_get((u32)(fd & 0xFFFFFFFFULL));
        if (fh == 0xFFFFFFFF || pipe_fh_is(fh) || sock_fh_is(fh)) {
            return (u64)(-1);
        }
        if (!fs.ext2_fh_is(fh) && !fs.fs_file_is_mmapable(fh)) {
            return (u64)(-1);         /* console/null/zero/dir */
        }
        if (fs.ext2_fh_is(fh) ? fs.ext2_file_is_dir(fh) : fs.fs_file_is_dir(fh)) {
            return (u64)(-1);
        }
        alen = (len + 0xFFF) & ~0xFFFULL;
        if (alen == 0) {
            return (u64)(-1);
        }
        if (fs.ext2_fh_is(fh)) {
            fs_kind = 1;
            ino = fs.ext2_fh_ino(fh);
        } else {
            fs_kind = 0;
            ino = fs.fs_file_ino(fh);
        }
        if (ino == 0) {
            return (u64)(-1);
        }
        va = mem.as_mmap_file_h(as, req_addr, alen, (u32)prot, shared,
                               fs_kind, ino, off, is_fixed);
        return va == 0 ? (u64)(-1) : va;
    }
}

/* int mprotect(addr, len, prot): change VMA prot on a page-aligned range. */
u64 sys_mprotect(u64 addr, u64 len, u64 prot) {
    return mem.as_mprotect_h(sched_current_as_h(), addr, len, (u32)prot)
           ? 0 : (u64)(-1);
}

u64 sys_brk(u64 newbrk) {
    return mem.as_brk_h(sched_current_as_h(), newbrk);
}

/* int munmap(addr, len): page-aligned range inside one VMA. */
u64 sys_munmap(u64 addr, u64 len) {
    return mem.as_munmap_h(sched_current_as_h(), addr, len)
           ? 0 : (u64)(-1);
}

/* chmod(2): update Ramfs/Ext2 permission bits (no ACL / capability model). */
u64 sys_chmod(u64 upath, u64 mode) {
    char path[128];
    extern u32 copy_user_path(char *dst, u64 upath);
    if (!copy_user_path(path, upath)) {
        return (u64)(-1);
    }
    if (fs.ext2_is_mnt_path(path)) {
        if (!fs.ext2_mounted() || !fs.ext2_chmod(path, (u32)mode)) {
            return (u64)(-1);
        }
        return 0;
    }
    return fs.fs_chmod(path, (u32)mode) ? 0 : (u64)(-1);
}

/* umask(2): set process file-creation mask; return previous mask. */
u64 sys_umask(u64 mask) {
    extern u32 sched_umask_set(u32 mask);
    return (u64)sched_umask_set((u32)mask);
}

u64 sys_gettid(void) {
    return (u64)sched_current_tid();
}

/* set_tid_address(2): register CLEARTID word; returns caller's tid. */
u64 sys_set_tid_address(u64 tidptr) {
    u64 as;
    if (tidptr != 0) {
        if ((tidptr & 3ULL) != 0) {
            return (u64)(-22); /* EINVAL */
        }
        as = sched_current_as_h();
        if (as == 0 || !mem.as_user_range_ok(as, tidptr, 4)) {
            return (u64)(-22);
        }
    }
    sched_set_clear_child_tid(tidptr);
    return (u64)sched_current_tid();
}

enum {
    FUTEX_WAIT          = 0,
    FUTEX_WAKE          = 1,
    FUTEX_PRIVATE_FLAG  = 128,
    FUTEX_CLOCK_REALTIME = 256,
    EAGAIN_NEG          = (u64)-11,
    EINTR_NEG           = (u64)-4,
    EINVAL_NEG          = (u64)-22
};

/* futex(2) basics: WAIT/WAKE (+ PRIVATE). Channel is the user VA; works for
 * same-AS threads and MAP_SHARED across fork. No PI/bitset/requeue yet. */
u64 sys_futex(u64 uaddr, u64 op, u64 val, u64 timeout, u64 uaddr2, u64 val3) {
    u64 as;
    u32 cmd;
    (void)uaddr2;
    (void)val3;
    (void)timeout; /* absolute/relative timeout not wired yet */

    if ((uaddr & 3ULL) != 0) {
        return EINVAL_NEG;
    }
    as = sched_current_as_h();
    if (as == 0 || !mem.as_user_range_ok(as, uaddr, 4)) {
        return EINVAL_NEG;
    }

    cmd = (u32)op & ~(u32)(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    if (cmd == FUTEX_WAIT) {
        volatile u32 *p = (volatile u32 *)uaddr;
        u32 cur = *p;
        if (cur != (u32)val) {
            return EAGAIN_NEG;
        }
        sched_sleep(uaddr);
        if (sched_should_eintr()) {
            return EINTR_NEG;
        }
        return 0;
    }
    if (cmd == FUTEX_WAKE) {
        u32 n = (u32)val;
        if (n == 0) {
            return 0;
        }
        return (u64)sched_wakeup_n(uaddr, n);
    }
    return ENOSYS_NEG;
}

/* ---------------------------------------------------------------------------
 * Interactive POSIX basics: getppid / uname / nanosleep / clock_gettime
 * ------------------------------------------------------------------------- */

extern u64 sched_tick_count(void);
extern void sched_yield(void);
extern void sched_check_signals(void);
extern u32 sched_current_ppid(void);

enum {
    CLOCK_REALTIME_ID  = 0,
    CLOCK_MONOTONIC_ID = 1,
    CLOCK_BOOTTIME_ID  = 7,
    UTS_LEN            = 65,
    UTS_SIZE           = 390,   /* 6 * 65 Linux x86-64 utsname */
    NS_PER_SEC         = 1000000000,
    NS_PER_TICK        = 10000000, /* 100 Hz LAPIC timer */
    TICKS_PER_SEC      = 100
};

u64 sys_getppid(void) {
    return (u64)sched_current_ppid();
}

static void uts_put(char *dst, const char *s) {
    u32 i = 0;
    while (s[i] != 0 && i + 1 < UTS_LEN) {
        dst[i] = s[i];
        i++;
    }
    while (i < UTS_LEN) {
        dst[i] = 0;
        i++;
    }
}

/* uname(2): Linux x86-64 struct utsname, 6 fields of 65 bytes. */
u64 sys_uname(u64 ubuf) {
    u64 as = sched_current_as_h();
    char *dst;
    if (as == 0 || ubuf == 0 || !mem.as_user_range_ok(as, ubuf, UTS_SIZE)) {
        return (u64)(-1);
    }
    dst = (char *)ubuf;
    uts_put(dst + 0 * UTS_LEN, "fakeos");
    uts_put(dst + 1 * UTS_LEN, "fakeos");
    uts_put(dst + 2 * UTS_LEN, "0.1.0");
    uts_put(dst + 3 * UTS_LEN, "daily-driver");
    uts_put(dst + 4 * UTS_LEN, "x86_64");
    uts_put(dst + 5 * UTS_LEN, "(none)");
    return 0;
}

static void ticks_to_timespec(u64 ticks, volatile u64 *sec_p, volatile u64 *nsec_p) {
    *sec_p = ticks / TICKS_PER_SEC;
    *nsec_p = (ticks % TICKS_PER_SEC) * NS_PER_TICK;
}

/* clock_gettime(2): REALTIME/MONOTONIC/BOOTTIME from 100 Hz sched ticks
 * (no RTC yet — REALTIME tracks boot-relative time like MONOTONIC). */
u64 sys_clock_gettime(u64 clockid, u64 utp) {
    u64 as = sched_current_as_h();
    u64 ticks;
    if (as == 0 || utp == 0 || !mem.as_user_range_ok(as, utp, 16)) {
        return (u64)(-1);
    }
    if (clockid != CLOCK_REALTIME_ID && clockid != CLOCK_MONOTONIC_ID
        && clockid != CLOCK_BOOTTIME_ID) {
        return EINVAL_NEG;
    }
    ticks = sched_tick_count();
    ticks_to_timespec(ticks, (volatile u64 *)utp, (volatile u64 *)(utp + 8));
    return 0;
}

/* nanosleep(2): relative sleep via 100 Hz ticks + sched_yield (like poll).
 * Writes remaining time to rem on EINTR when rem != NULL. */
u64 sys_nanosleep(u64 ureq, u64 urem) {
    u64 as = sched_current_as_h();
    i64 sec;
    i64 nsec;
    u64 ticks;
    u64 deadline;
    u64 now;
    if (as == 0 || ureq == 0 || !mem.as_user_range_ok(as, ureq, 16)) {
        return (u64)(-1);
    }
    if (urem != 0 && !mem.as_user_range_ok(as, urem, 16)) {
        return (u64)(-1);
    }
    sec = (i64)(*(volatile u64 *)ureq);
    nsec = (i64)(*(volatile u64 *)(ureq + 8));
    if (sec < 0 || nsec < 0 || nsec >= NS_PER_SEC) {
        return EINVAL_NEG;
    }
    if (sec == 0 && nsec == 0) {
        return 0;
    }
    ticks = (u64)sec * TICKS_PER_SEC + (u64)nsec / NS_PER_TICK;
    if (ticks == 0) {
        ticks = 1;
    }
    deadline = sched_tick_count() + ticks;
    for (;;) {
        now = sched_tick_count();
        if (now >= deadline) {
            return 0;
        }
        sched_check_signals();
        if (sched_should_eintr()) {
            if (urem != 0) {
                u64 left = deadline - now;
                ticks_to_timespec(left, (volatile u64 *)urem,
                                  (volatile u64 *)(urem + 8));
            }
            return EINTR_NEG;
        }
        sched_yield();
    }
}

/* ---------------------------------------------------------------------------
 * /proc/self/{status,maps,cmdline,environ} generators (snapshotted on open)
 * ------------------------------------------------------------------------- */

extern u32 sched_current_ppid(void);
extern u32 sched_current_threads(void);
extern u64 sched_cmdline_copy(char *buf, u64 cap);
extern u64 sched_environ_copy(char *buf, u64 cap);
extern void sched_set_cmdline(const char *data, u64 n);
extern void sched_set_cmdline_argv(u32 argc, const char *argbuf);
extern void sched_set_cmdline_tcb(u32 tid, const char *data, u64 n);

static u64 proc_put(char *dst, u64 cap, u64 at, const char *s) {
    u64 i = 0;
    while (s[i] != 0) {
        if (at + i >= cap) {
            return at;
        }
        dst[at + i] = s[i];
        i++;
    }
    return at + i;
}

static u64 proc_put_u32(char *dst, u64 cap, u64 at, u32 v) {
    char tmp[16];
    u32 n = 0;
    u32 i;
    if (v == 0) {
        tmp[0] = '0';
        n = 1;
    } else {
        while (v != 0 && n < 15) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
        /* reverse */
        for (i = 0; i < n / 2; i++) {
            char c = tmp[i];
            tmp[i] = tmp[n - 1 - i];
            tmp[n - 1 - i] = c;
        }
    }
    tmp[n] = 0;
    return proc_put(dst, cap, at, tmp);
}

u64 procfs_gen_status(char *buf, u64 cap) {
    char name[64];
    u64 at = 0;
    u64 cn;
    u64 i;
    u32 slash = 0;
    u64 vmkb;
    if (buf == 0 || cap == 0) {
        return 0;
    }
    cn = sched_cmdline_copy(name, 63);
    name[cn < 63 ? cn : 63] = 0;
    /* Basename of argv[0] (stop at first NUL). */
    for (i = 0; i < cn && name[i] != 0; i++) {
        if (name[i] == '/') {
            slash = (u32)(i + 1);
        }
    }
    at = proc_put(buf, cap, at, "Name:\t");
    at = proc_put(buf, cap, at, name + slash);
    at = proc_put(buf, cap, at, "\nState:\tR (running)\nPid:\t");
    at = proc_put_u32(buf, cap, at, sched_current_pid());
    at = proc_put(buf, cap, at, "\nPPid:\t");
    at = proc_put_u32(buf, cap, at, sched_current_ppid());
    at = proc_put(buf, cap, at, "\nThreads:\t");
    at = proc_put_u32(buf, cap, at, sched_current_threads());
    vmkb = mem.as_vma_kb_h(sched_current_as_h());
    at = proc_put(buf, cap, at, "\nVmSize:\t");
    at = proc_put_u32(buf, cap, at, (u32)vmkb);
    at = proc_put(buf, cap, at, " kB\n");
    return at;
}

u64 procfs_gen_maps(char *buf, u64 cap) {
    return mem.as_format_maps_h(sched_current_as_h(), buf, cap);
}

u64 procfs_gen_cmdline(char *buf, u64 cap) {
    return sched_cmdline_copy(buf, cap);
}

u64 procfs_gen_environ(char *buf, u64 cap) {
    return sched_environ_copy(buf, cap);
}
