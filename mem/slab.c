package mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern u64 pmm_alloc_page(void);
extern u64 pmm_alloc_pages(u32 order);
extern u32 pmm_max_order(void);
extern void pmm_free_pages(u64 paddr, u32 order);

/* Shared central state is protected across CPUs and local IRQ preemption.
 * Lock ordering: heap_lock -> pmm_lock. Neither path sleeps. */
extern u64 cpu_irq_save(void);
extern void cpu_irq_restore(u64 flags);
extern void spin_lock(volatile u32 *lock);
extern void spin_unlock(volatile u32 *lock);
static volatile u32 heap_lock;

/* Per-CPU identity hook: the kernel scheduler assigns &smp_cpuid at boot
 * (fakecc cannot parse function-pointer parameters in prototypes, so the
 * arch-package global-hook pattern is used instead). Until installed, or
 * when a caller runs on a CPU index beyond the cache table, every CPU maps
 * to cache row 0: correct, merely less concurrent. */
u32 (*kmalloc_cpu_hook)(void) = 0;

/* ---------------------------------------------------------------------------
 * Slab kernel heap — jemalloc/tcmalloc-style design
 *
 * Three allocation paths, all returned pointers are HHDM virtual addresses:
 *
 *   size <= 2048  -> one of 24 size classes (four sub-steps per doubling,
 *                    every class a multiple of 16 B so the historical 16-byte
 *                    alignment invariant holds). Two-tier design:
 *
 *     per-CPU caches  (tcmalloc thread-cache / jemalloc tcache analogue):
 *       kmalloc pops the calling CPU's cache with NO lock — cpu_irq_save is
 *       the only mutual exclusion on that CPU, so concurrent allocators on
 *       different CPUs never share a lock on the fast path. kfree pushes
 *       back; at CACHE_DEPTH it flushes a batch to the central layer, and a
 *       refill pulls a batch when the cache empties (bounded transfer, so a
 *       hot cache cannot pin the whole class).
 *
 *     central slabs   (jemalloc arena/extents analogue, bounded): objects
 *       live in 8-page (32 KiB) runs carved from buddy order-3 blocks. The
 *       64-byte run header sits at the run base; each class draws from its
 *       "current" run and a singly-linked partial chain (head insert on
 *       full->partial transition, head removal when a run becomes current —
 *       both O(1), no back pointers). A run only joins the partial chain
 *       while nfree > 0, and only the current run is ever allocated from,
 *       so the invariant "every partial entry has nfree > 0" holds.
 *
 *   size >  2048  -> a power-of-two buddy block up to pmm_max_order()
 *                    (currently 8 KiB .. 16 MiB), uncached as before.
 *
 * A packed per-frame table (`page_info`) gives O(1) kfree resolution: class
 * runs mark every frame with (class_index + 1) and kfree recovers the run
 * base by masking the frame index (buddy order-3 blocks are run-aligned),
 * buddy blocks store (0x80000000 | order). Slab frames never change class
 * for the life of the run, so the unlocked fast-path dispatch read is stable.
 * ------------------------------------------------------------------------- */

static const u64 HHDM_BASE = 0xFFFF800000000000ULL;
static const u64 PAGE_SIZE = 4096;

enum {
    NCLASSES       = 24,
    MIN_CLASS_SIZE = 16,
    MAX_CLASS_SIZE = 2048,
    RUN_PAGES      = 8,
    RUN_BYTES      = RUN_PAGES * 4096,     /* 32 KiB per run */
    HEADER_SIZE    = 64,                   /* sizeof(struct slab_header)   */
    CACHE_DEPTH    = 32,                   /* per-CPU cache capacity/class */
    CACHE_BATCH    = 16,                   /* refill/flush transfer size   */
    MAX_CPUS_C     = 8,                    /* cache rows (matches sched)   */
    INFO_NONE      = 0xFFFFFFFF,
    INFO_BIG       = 0x80000000,
    SLAB_MAGIC     = 0x534C414242414C53ULL,   /* "SLABBALS" */
    MAX_FRAMES     = 1048576               /* 4 GiB / 4 KiB (must match pmm.c) */
};

struct slab_header {
    u64 magic;
    u32 cls;
    u32 nfree;
    u64 free_head;     /* Physical address of first free object, or 0     */
    u64 next;          /* Next run in the class chain (paddr), or 0       */
    u64 pnext;         /* Next run in the partial chain (paddr), or 0     */
    u32 capacity;
    u32 pad;
};

static const u16 class_size[NCLASSES] = {
    16, 32, 48, 64,
    80, 96, 112, 128,
    160, 192, 224, 256,
    320, 384, 448, 512,
    640, 768, 896, 1024,
    1280, 1536, 1792, 2048
};

struct cpu_cache {
    u64 obj[CACHE_DEPTH];   /* Free-object physical addresses, LIFO stack */
    u32 count;
    u32 pad;
    u64 stat_fast_alloc;    /* Served without touching heap_lock          */
    u64 stat_fast_free;
    u64 stat_refill;        /* Central -> cache batch transfers           */
    u64 stat_flush;         /* Cache -> central batch transfers           */
};

static struct cpu_cache caches[MAX_CPUS_C][NCLASSES];

/* Per-frame dispatch table (one u32 per physical frame). */
static u32 page_info[MAX_FRAMES];

/* Central state per class. */
static u64 slab_head[NCLASSES];      /* All-runs chain (diagnostics)      */
static u64 partial_head[NCLASSES];   /* Runs with nfree > 0, not current  */
static u64 current_run[NCLASSES];    /* Run the allocator draws from      */

static volatile u64 *hhdm(u64 paddr) {
    return (volatile u64 *)(HHDM_BASE + paddr);
}

static u32 cache_cpu_index(void) {
    u32 cpu;
    if (kmalloc_cpu_hook == 0) {
        return 0;
    }
    cpu = kmalloc_cpu_hook();
    if (cpu >= MAX_CPUS_C) {
        cpu = 0;
    }
    return cpu;
}

static u32 size_to_class(u64 size) {
    u32 c;
    for (c = 0; c < NCLASSES; c++) {
        if (size <= (u64)class_size[c]) {
            return c;
        }
    }
    return NCLASSES;
}

static u32 big_order_for(u64 size) {
    u32 order = 1;                    /* 8 KiB minimum for the big path */
    u32 max_order = pmm_max_order();
    while (order <= max_order) {
        u64 bytes = (u64)1 << (order + 12);
        if (bytes >= size) {
            return order;
        }
        order++;
    }
    return max_order + 1;       /* Unsupported: must be > every order */
}

/* Create a fresh 8-page run for class c and make it the current run.
 * Requires heap_lock. */
static int run_grow(u32 c) {
    u64 run = pmm_alloc_pages(3);      /* RUN_PAGES, buddy order 3 */
    if (run == 0) {
        return 0;
    }

    u32 obj_size = class_size[c];
    u32 capacity = (u32)((RUN_BYTES - HEADER_SIZE) / obj_size);
    u32 base_pfn = (u32)(run / PAGE_SIZE);

    struct slab_header *h = (struct slab_header *)(HHDM_BASE + run);
    h->magic = SLAB_MAGIC;
    h->cls = c;
    h->nfree = capacity;
    h->capacity = capacity;
    h->pnext = 0;
    h->next = slab_head[c];

    /* Thread every object onto the free chain (lowest address at the head
     * by building in reverse), intrusive next-pointer in qword 0. Objects
     * span run pages contiguously; the header occupies only the base. */
    u64 prev = 0;
    u32 i;
    for (i = capacity - 1; ; i--) {
        u64 obj = run + HEADER_SIZE + (u64)i * obj_size;
        *hhdm(obj) = prev;
        prev = obj;
        if (i == 0) break;
    }
    h->free_head = prev;

    slab_head[c] = run;
    current_run[c] = run;
    for (i = 0; i < RUN_PAGES; i++) {
        page_info[base_pfn + i] = c + 1;
    }
    return 1;
}

/* Central allocation: draw from the current run, then the partial chain
 * (O(1) head take — every partial entry has nfree > 0), then grow.
 * Requires heap_lock. Returns the object paddr or 0. */
static u64 alloc_from_class_locked(u32 c) {
    u64 run = current_run[c];
    struct slab_header *h;
    u64 obj;

    if (run != 0) {
        h = (struct slab_header *)(HHDM_BASE + run);
        if (h->nfree > 0) {
            obj = h->free_head;
            h->free_head = *hhdm(obj);
            h->nfree = h->nfree - 1;
            if (h->nfree == 0) {
                current_run[c] = 0;    /* exhausted; not in partial chain */
            }
            return obj;
        }
        current_run[c] = 0;
    }
    while (partial_head[c] != 0) {
        run = partial_head[c];
        h = (struct slab_header *)(HHDM_BASE + run);
        partial_head[c] = h->pnext;    /* unlink head: O(1), no back ptr */
        h->pnext = 0;
        if (h->nfree == 0) {
            continue;                  /* defensive: skip stale entries */
        }
        current_run[c] = run;
        obj = h->free_head;
        h->free_head = *hhdm(obj);
        h->nfree = h->nfree - 1;
        if (h->nfree == 0) {
            current_run[c] = 0;
        }
        return obj;
    }
    if (!run_grow(c)) {
        return 0;
    }
    h = (struct slab_header *)(HHDM_BASE + current_run[c]);
    obj = h->free_head;
    h->free_head = *hhdm(obj);
    h->nfree = h->nfree - 1;
    if (h->nfree == 0) {
        current_run[c] = 0;
    }
    return obj;
}

/* Central free: return one object to its run; transition full -> partial
 * by head-inserting the run into the partial chain. Requires heap_lock. */
static void free_to_class_locked(u32 c, u64 paddr) {
    u64 run = paddr & ~(u64)(RUN_BYTES - 1);
    struct slab_header *h = (struct slab_header *)(HHDM_BASE + run);
    u64 obj = paddr;
    if (h->magic != SLAB_MAGIC || h->cls != c) {
        return;                        /* Dispatch table corruption guard */
    }
    *hhdm(obj) = h->free_head;
    h->free_head = obj;
    if (h->nfree == 0 && run != current_run[c]) {
        h->pnext = partial_head[c];
        partial_head[c] = run;
    }
    h->nfree = h->nfree + 1;
}

/* Cache -> central batch transfer. Requires heap_lock. */
static void cache_flush_locked(u32 cpu, u32 c) {
    struct cpu_cache *cc = &caches[cpu][c];
    while (cc->count > CACHE_BATCH) {
        cc->count = cc->count - 1;
        free_to_class_locked(c, cc->obj[cc->count]);
    }
    cc->stat_flush = cc->stat_flush + 1;
}

/* Central -> cache batch transfer. Requires heap_lock. */
static void cache_refill_locked(u32 cpu, u32 c) {
    struct cpu_cache *cc = &caches[cpu][c];
    while (cc->count < CACHE_BATCH) {
        u64 obj = alloc_from_class_locked(c);
        if (obj == 0) {
            break;                     /* OOM: hand back what we have */
        }
        cc->obj[cc->count] = obj;
        cc->count = cc->count + 1;
    }
    cc->stat_refill = cc->stat_refill + 1;
}

static void *kmalloc_big_locked(u64 size) {
    u32 order = big_order_for(size);
    u64 block;
    u32 frames;
    u32 base_pfn;
    u32 i;
    if (order > pmm_max_order()) {
        return 0;
    }
    block = pmm_alloc_pages(order);
    if (block == 0) {
        return 0;
    }
    frames = (u32)1 << order;
    base_pfn = (u32)(block / PAGE_SIZE);
    for (i = 0; i < frames; i++) {
        page_info[base_pfn + i] = INFO_BIG | order;
    }
    return (void *)(HHDM_BASE + block);
}

void *kmalloc(u64 size) {
    u64 flags;
    u32 cpu;
    u32 c;
    void *result;

    if (size == 0) {
        size = MIN_CLASS_SIZE;
    }
    c = size_to_class(size);

    flags = cpu_irq_save();
    cpu = cache_cpu_index();

    if (c < NCLASSES) {
        struct cpu_cache *cc = &caches[cpu][c];
        if (cc->count != 0) {
            /* Per-CPU fast path: no shared lock, IRQ save is the only
             * mutual exclusion on this CPU. */
            cc->count = cc->count - 1;
            result = (void *)(HHDM_BASE + cc->obj[cc->count]);
            cc->stat_fast_alloc = cc->stat_fast_alloc + 1;
            cpu_irq_restore(flags);
            return result;
        }
        /* Slow path: one heap_lock acquisition moves a whole batch into
         * the cache, amortising the central layer across many allocs. */
        spin_lock(&heap_lock);
        cache_refill_locked(cpu, c);
        if (cc->count != 0) {
            cc->count = cc->count - 1;
            result = (void *)(HHDM_BASE + cc->obj[cc->count]);
            cc->stat_fast_alloc = cc->stat_fast_alloc + 1;
        } else {
            result = 0;
        }
        spin_unlock(&heap_lock);
        cpu_irq_restore(flags);
        return result;
    }

    /* Big allocation: contiguous buddy block, uncached. */
    spin_lock(&heap_lock);
    result = kmalloc_big_locked(size);
    spin_unlock(&heap_lock);
    cpu_irq_restore(flags);
    return result;
}

static void kfree_big_locked(u64 paddr, u32 pfn) {
    u32 info = page_info[pfn];
    u32 order;
    u32 frames;
    u32 base_pfn;
    u32 i;
    if (info == INFO_NONE) {
        return;                        /* Double free / foreign pointer */
    }
    order = info & 0x7F;
    frames = (u32)1 << order;
    base_pfn = pfn & ~(frames - 1);
    for (i = 0; i < frames; i++) {
        page_info[base_pfn + i] = INFO_NONE;
    }
    pmm_free_pages((u64)base_pfn * PAGE_SIZE, order);
}

void kfree(void *ptr) {
    u64 vaddr;
    u64 paddr;
    u32 pfn;
    u32 info;
    u32 c;
    u64 flags;
    u32 cpu;
    struct cpu_cache *cc;

    if (ptr == 0) {
        return;
    }
    vaddr = (u64)ptr;
    if (vaddr < HHDM_BASE) {
        return;                        /* Not a kernel heap pointer */
    }
    paddr = vaddr - HHDM_BASE;
    pfn = (u32)(paddr / PAGE_SIZE);
    if (pfn >= MAX_FRAMES) {
        return;
    }

    info = page_info[pfn];
    if (info == INFO_NONE) {
        return;                        /* Double free / foreign pointer */
    }
    if (info & INFO_BIG) {
        /* Buddy block: central accounting under the heap lock. */
        flags = cpu_irq_save();
        spin_lock(&heap_lock);
        kfree_big_locked(paddr, pfn);
        spin_unlock(&heap_lock);
        cpu_irq_restore(flags);
        return;
    }

    c = info - 1;
    if (c >= NCLASSES) {
        return;                        /* Corrupt dispatch table */
    }

    /* Per-CPU fast path: the slab frames' dispatch entries are immutable
     * for the life of the run, so the unlocked read above is stable. */
    flags = cpu_irq_save();
    cpu = cache_cpu_index();
    cc = &caches[cpu][c];
    if (cc->count < CACHE_DEPTH) {
        cc->obj[cc->count] = paddr;
        cc->count = cc->count + 1;
        cc->stat_fast_free = cc->stat_fast_free + 1;
        cpu_irq_restore(flags);
        return;
    }
    /* Cache full: flush half a batch's worth of headroom back to the
     * central slabs in one heap_lock acquisition. */
    spin_lock(&heap_lock);
    cache_flush_locked(cpu, c);
    if (cc->count < CACHE_DEPTH) {
        cc->obj[cc->count] = paddr;
        cc->count = cc->count + 1;
    }
    spin_unlock(&heap_lock);
    cpu_irq_restore(flags);
}

/* Return every cached free object to the central slabs (used by tests and
 * by anyone needing an exact page-accounting checkpoint). */
void kmalloc_cache_flush_all(void) {
    u64 flags = cpu_irq_save();
    u32 cpu;
    u32 c;
    spin_lock(&heap_lock);
    for (cpu = 0; cpu < MAX_CPUS_C; cpu++) {
        for (c = 0; c < NCLASSES; c++) {
            struct cpu_cache *cc = &caches[cpu][c];
            while (cc->count > 0) {
                cc->count = cc->count - 1;
                free_to_class_locked(c, cc->obj[cc->count]);
            }
        }
    }
    spin_unlock(&heap_lock);
    cpu_irq_restore(flags);
}

/* ---------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

u64 kmalloc_class_size(u32 c) {
    if (c >= NCLASSES) return 0;
    return (u64)class_size[c];
}

u32 kmalloc_class_count(void) {
    return NCLASSES;
}

/* Number of 8-page runs currently backing each class. */
u32 kmalloc_class_slabs(u32 c) {
    u32 n = 0;
    u64 page;
    if (c >= NCLASSES) return 0;
    page = slab_head[c];
    while (page != 0) {
        struct slab_header *h = (struct slab_header *)(HHDM_BASE + page);
        n++;
        page = h->next;
    }
    return n;
}

u32 kmalloc_class_capacity(u32 c) {
    if (c >= NCLASSES) return 0;
    return (u32)((RUN_BYTES - HEADER_SIZE) / class_size[c]);
}

/* Per-CPU cache statistics. Exact once the observed CPU is quiescent
 * (e.g. after test workers joined). */
void kmalloc_cache_stats(u32 cpu, u32 c, u64 *fast_alloc, u64 *fast_free,
                         u64 *refill, u64 *flush) {
    if (cpu >= MAX_CPUS_C || c >= NCLASSES) return;
    if (fast_alloc) *fast_alloc = caches[cpu][c].stat_fast_alloc;
    if (fast_free) *fast_free = caches[cpu][c].stat_fast_free;
    if (refill) *refill = caches[cpu][c].stat_refill;
    if (flush) *flush = caches[cpu][c].stat_flush;
}

u32 kmalloc_cache_cpu_rows(void) {
    return MAX_CPUS_C;
}
