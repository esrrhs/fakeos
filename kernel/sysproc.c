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

/* void *mmap(addr, len, prot, flags, fd, off):
 *  - MAP_PRIVATE|MAP_ANONYMOUS: demand-zero window (existing path)
 *  - MAP_PRIVATE + regular file fd: eager MAP_PRIVATE file fill via
 *    as_copy_in_h (coherent with current VMA model; no VMA_FILE yet).
 * off must be page-aligned for file maps; MAP_SHARED is rejected. */
u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off) {
    u64 va;
    u64 as;
    u32 anon;
    if (len == 0) {
        return (u64)(-1);
    }
    if ((flags & MAP_SHARED) != 0) {
        return (u64)(-1);
    }
    if ((flags & MAP_PRIVATE) == 0) {
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
            va = mem.as_mmap_fixed_h(as, addr, len, (u32)prot);
            return va == 0 ? (u64)(-1) : va;
        }
        if (addr != 0) {
            return (u64)(-1);
        }
        va = mem.as_mmap_anon_h(as, len, (u32)prot);
        return va == 0 ? (u64)(-1) : va;
    }

    /* File-backed MAP_PRIVATE: eager read into anonymous pages. */
    {
        u32 fh;
        u64 file_off = off;
        u64 filled = 0;
        char kbuf[4096];
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
        va = mem.as_mmap_anon_h(as, len, (u32)prot);
        if (va == 0) {
            return (u64)(-1);
        }
        if (fs.ext2_fh_is(fh)) {
            fs.ext2_lseek(fh, file_off, 0);
        } else {
            fs.fs_lseek_h(fh, file_off, 0);
        }
        while (filled < len) {
            u64 want = len - filled;
            u64 n;
            u32 i;
            if (want > 4096) {
                want = 4096;
            }
            for (i = 0; i < 4096; i++) {
                kbuf[i] = 0;
            }
            n = fs.ext2_fh_is(fh)
                ? fs.ext2_read(fh, kbuf, want)
                : fs.fs_read_h(fh, kbuf, want);
            if ((i64)n <= 0) {
                break; /* EOF or error: leave remaining demand-zero */
            }
            if (!mem.as_copy_in_h(as, va + filled, kbuf, n)) {
                mem.as_munmap_h(as, va, len);
                return (u64)(-1);
            }
            filled += n;
            if (n < want) {
                break;
            }
        }
        return va;
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

/* Threading is not implemented; the compiler never creates threads. Report
 * the Linux ENOSYS code so runtime thread waiters fail predictably. */
u64 sys_futex(u64 uaddr, u64 op, u64 val, u64 timeout, u64 uaddr2, u64 val3) {
    (void)uaddr; (void)op; (void)val; (void)timeout; (void)uaddr2; (void)val3;
    return ENOSYS_NEG;
}
