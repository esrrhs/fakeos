package kernel;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* ---------------------------------------------------------------------------
 * Memory and (later) process-life syscalls: brk / mmap.
 * wait4 and execve join this file in their slices.
 * ------------------------------------------------------------------------- */

enum {
    MAP_SHARED    = 0x01,
    MAP_PRIVATE   = 0x02,
    MAP_ANONYMOUS = 0x20
};

extern u64 sched_current_as_h(void);

/* void *mmap(addr, len, prot, flags, fd, off): anonymous private mappings at
 * a kernel-chosen address only. */
u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off) {
    u64 va;
    if (addr != 0 || len == 0 || off != 0) {
        return (u64)(-1);
    }
    if ((flags & (MAP_PRIVATE | MAP_ANONYMOUS)) != (MAP_PRIVATE | MAP_ANONYMOUS)) {
        return (u64)(-1);
    }
    if (fd != (u64)(-1)) {
        return (u64)(-1);
    }
    va = mem.as_mmap_anon_h(sched_current_as_h(), len, (u32)prot);
    return va == 0 ? (u64)(-1) : va;
}

u64 sys_brk(u64 newbrk) {
    return mem.as_brk_h(sched_current_as_h(), newbrk);
}
