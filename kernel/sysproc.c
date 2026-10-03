package kernel;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;
typedef types.int32_t s32;

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

/* void *mmap(addr, len, prot, flags, fd, off): anonymous private mappings at
 * a kernel-chosen address only. */
u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off) {
    u64 va;
    if (len == 0 || off != 0) {
        return (u64)(-1);
    }
    if ((flags & (MAP_PRIVATE | MAP_ANONYMOUS)) != (MAP_PRIVATE | MAP_ANONYMOUS)) {
        return (u64)(-1);
    }
    /* fd is an int in the Linux ABI; a 32-bit -1 moved by user code may
     * arrive as 0x00000000'FFFFFFFF rather than a sign-extended register. */
    if ((s32)(fd & 0xFFFFFFFFULL) != -1) {
        return (u64)(-1);
    }
    /* MAP_FIXED: reserve exactly at addr (fakecc's 16 TiB ASAN shadow
     * window). Demand paging keeps the huge reservation physically free.
     * Other bits (e.g. MAP_NORESERVE 0x4000) are accepted and ignored. */
    if (flags & MAP_FIXED) {
        va = mem.as_mmap_fixed_h(sched_current_as_h(), addr, len, (u32)prot);
        return va == 0 ? (u64)(-1) : va;
    }
    if (addr != 0) {
        return (u64)(-1);                /* hinted placement is not supported */
    }
    va = mem.as_mmap_anon_h(sched_current_as_h(), len, (u32)prot);
    return va == 0 ? (u64)(-1) : va;
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
