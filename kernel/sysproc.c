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
 *  - MAP_PRIVATE|MAP_ANONYMOUS: demand-zero window
 *  - MAP_SHARED|MAP_ANONYMOUS: shm-style shared anon (keep W across fork)
 *  - MAP_PRIVATE / MAP_SHARED + regular file: VMA_FILE demand paging
 *    (MAP_SHARED shares frames across fork; dirty pages write back on munmap).
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
        if (addr != 0) {
            return (u64)(-1);
        }
        va = mem.as_mmap_anon_h(as, len, (u32)prot, shared);
        return va == 0 ? (u64)(-1) : va;
    }

    /* File-backed: true VMA_FILE demand paging. */
    {
        u32 fh;
        u32 ino;
        u32 fs_kind;
        u64 alen;
        if ((off & 0xFFF) != 0) {
            return (u64)(-1);
        }
        if (flags & MAP_FIXED) {
            return (u64)(-1); /* keep MAP_FIXED for ASAN anon only */
        }
        if (addr != 0) {
            return (u64)(-1);
        }
        if ((s32)(fd & 0xFFFFFFFFULL) < 0 || (fd & 0xFFFFFFFFULL) >= 16) {
            return (u64)(-1);
        }
        fh = sched_fd_get((u32)(fd & 0xFFFFFFFFULL));
        if (fh == 0xFFFFFFFF || pipe_fh_is(fh) || sock_fh_is(fh)) {
            return (u64)(-1);
        }
        if (!fs.ext2_fh_is(fh) && fs.fs_file_is_console(fh)) {
            return (u64)(-1);
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
        va = mem.as_mmap_file_h(as, alen, (u32)prot, shared, fs_kind, ino, off);
        return va == 0 ? (u64)(-1) : va;
    }
}

u64 sys_brk(u64 newbrk) {
    return mem.as_brk_h(sched_current_as_h(), newbrk);
}

/* int munmap(addr, len): page-aligned range inside one VMA. */
u64 sys_munmap(u64 addr, u64 len) {
    return mem.as_munmap_h(sched_current_as_h(), addr, len)
           ? 0 : (u64)(-1);
}

/* No permission model: chmod always succeeds (fakecc's output path uses it
 * after writing an executable). */
u64 sys_chmod(u64 path, u64 mode) {
    (void)path;
    (void)mode;
    return 0;
}

u64 sys_gettid(void) {
    return (u64)sched_current_pid();
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
