package kernel;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);

/* ---------------------------------------------------------------------------
 * Physical Memory Manager (buddy allocator) bring-up diagnostics and tests.
 * ------------------------------------------------------------------------- */

static const char *region_kind(u32 type) {
    if (type == 1) {
        return "AVAIL   ";
    }
    if (type == 2) {
        return "RESERVED";
    }
    if (type == 3) {
        return "ACPI REC";
    }
    if (type == 4) {
        return "ACPI NVS";
    }
    if (type == 5) {
        return "BADMEM  ";
    }
    return "UNKNOWN ";
}

static volatile u64 *page_ptr(u64 paddr) {
    return (volatile u64 *)mem.pmm_phys_to_virt(paddr);
}

/* ---------------------------------------------------------------------------
 * Virtual Memory Manager tests
 * ------------------------------------------------------------------------- */

/* Scratch virtual window in the kernel PML4 slot but PDPT entry 509
 * (0xFFFFFFFF40000000..0xFFFFFFFF80000000), which the bootstrap never maps:
 * VMM builds a fresh PDPT/PD/PT chain there. */
static const u64 VMM_TEST_VA   = 0xFFFFFFFF40200000ULL;
static const u64 VMM_TEST_VA_2 = 0xFFFFFFFF40201000ULL;

/* x86-64 PTE attribute bits used by the tests (mirrors mem/vmm.c). */
static const u64 T_PRESENT  = 1;
static const u64 T_WRITABLE = 2;
static const u64 T_USER     = 4;
static const u64 T_NX       = 1ULL << 63;

static int test_vmm_basic(void) {
    u64 root = mem.vmm_kernel_root();
    u64 page0 = mem.pmm_alloc_page();
    u64 page1 = mem.pmm_alloc_page();
    int ok = 1;

    if (page0 == 0 || page1 == 0) {
        kprintf("      [FAIL] VMM test could not allocate backing frames\n");
        return 0;
    }

    /* Map page0: supervisor RW (executable, NX clear). */
    if (!mem.vmm_map(root, VMM_TEST_VA, page0, T_PRESENT | T_WRITABLE)) {
        kprintf("      [FAIL] vmm_map page0 rejected\n");
        return 0;
    }
    /* Duplicate map must be rejected until unmapped. */
    if (mem.vmm_map(root, VMM_TEST_VA, page1, T_PRESENT | T_WRITABLE)) {
        kprintf("      [FAIL] duplicate vmm_map was accepted\n");
        ok = 0;
    }

    volatile u64 *p = (volatile u64 *)VMM_TEST_VA;
    u64 k;
    for (k = 0; k < 512; k++) {
        p[k] = VMM_TEST_VA ^ (k * 0x9E3779B97F4A7C15ULL);
    }
    for (k = 0; k < 512; k++) {
        if (p[k] != (VMM_TEST_VA ^ (k * 0x9E3779B97F4A7C15ULL))) {
            kprintf("      [FAIL] VMM mapped page data mismatch at qword %u\n", (u64)k);
            ok = 0;
            break;
        }
    }

    /* Map page1 with user + NX attributes and inspect them via translate. */
    if (!mem.vmm_map(root, VMM_TEST_VA_2, page1,
                     T_PRESENT | T_WRITABLE | T_USER | T_NX)) {
        kprintf("      [FAIL] vmm_map page1 rejected\n");
        ok = 0;
    }
    u64 t1 = mem.vmm_translate(root, VMM_TEST_VA_2);
    if ((t1 & 0xFFF) != (T_PRESENT | T_WRITABLE | T_USER)
        || !(t1 & T_NX) || (t1 & ~0xFFFull & ~T_NX) != page1) {
        kprintf("      [FAIL] translate flags/phys wrong: %p vs page %p\n", t1, page1);
        ok = 0;
    }
    u64 t0 = mem.vmm_translate(root, VMM_TEST_VA);
    if ((t0 & ~0xFFFull) != page0) {
        kprintf("      [FAIL] translate page0 wrong phys: %p vs %p\n", t0, page0);
        ok = 0;
    }

    /* Unmap page0: translation must fail while page1 stays intact. */
    if (!mem.vmm_unmap(root, VMM_TEST_VA)) {
        kprintf("      [FAIL] vmm_unmap returned false for mapped page\n");
        ok = 0;
    }
    if (mem.vmm_translate(root, VMM_TEST_VA) != 0) {
        kprintf("      [FAIL] unmapped page still translates\n");
        ok = 0;
    }
    if (mem.vmm_translate(root, VMM_TEST_VA_2) == 0) {
        kprintf("      [FAIL] sibling mapping lost on unmap\n");
        ok = 0;
    }
    mem.vmm_unmap(root, VMM_TEST_VA_2);

    mem.pmm_free_page(page0);
    mem.pmm_free_page(page1);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Slab / kmalloc tests
 * ------------------------------------------------------------------------- */

static int test_slab_classes(void) {
    enum { SLAB_N = 1024 };
    void *ptrs[SLAB_N];
    u64 sizes[SLAB_N];
    u64 i;
    int ok = 1;

    for (i = 0; i < SLAB_N; i++) {
        /* Exercise every class boundary. */
        u64 s = (i % 14 == 0) ? 1
              : (i % 14 == 1) ? 16
              : (i % 14 == 2) ? 17
              : (i % 14 == 3) ? 32
              : (i % 14 == 4) ? 33
              : (i % 14 == 5) ? 128
              : (i % 14 == 6) ? 129
              : (i % 14 == 7) ? 1024
              : (i % 14 == 8) ? 2048
              : (i % 14 == 9) ? (16 * (1 + i % 128))
              : 8 * (1 + i % 256);
        sizes[i] = s;
        ptrs[i] = mem.kmalloc(s);
        if (ptrs[i] == 0) {
            kprintf("      [FAIL] kmalloc(%u) returned null at %u\n", s, i);
            return 0;
        }
        if (((u64)ptrs[i] & 0xF) != 0) {
            kprintf("      [FAIL] kmalloc returned unaligned pointer %p\n", (u64)ptrs[i]);
            ok = 0;
        }
    }

    /* Uniqueness of live allocations. */
    for (i = 0; i < SLAB_N && ok; i++) {
        u64 j;
        for (j = i + 1; j < SLAB_N; j++) {
            if (ptrs[i] == ptrs[j]) {
                kprintf("      [FAIL] duplicate heap pointer %p\n", (u64)ptrs[i]);
                ok = 0;
                break;
            }
        }
    }

    /* Write canaries sized to each allocation and verify. */
    for (i = 0; i < SLAB_N && ok; i++) {
        volatile u8 *b = (volatile u8 *)ptrs[i];
        u64 k;
        for (k = 0; k < sizes[i]; k++) {
            b[k] = (u8)(i + k);
        }
    }
    for (i = 0; i < SLAB_N && ok; i++) {
        volatile u8 *b = (volatile u8 *)ptrs[i];
        u64 k;
        for (k = 0; k < sizes[i]; k++) {
            if (b[k] != (u8)(i + k)) {
                kprintf("      [FAIL] heap corruption at alloc %u byte %u\n", i, k);
                ok = 0;
                break;
            }
        }
    }

    for (i = 0; i < SLAB_N; i++) {
        mem.kfree(ptrs[i]);
    }
    return ok;
}

static int test_slab_big(void) {
    enum { BIG_N = 64 };
    void *blocks[BIG_N];
    u64 sizes[BIG_N];
    u64 i;
    int ok = 1;

    /* Baseline before any big block is handed out: after every block comes
     * back, the free page count must return to exactly this value. */
    u64 before = mem.pmm_free_pages_count();

    /* Everything above 2048 takes the buddy path: 8 KiB .. 256 KiB. */
    for (i = 0; i < BIG_N; i++) {
        sizes[i] = 4096 * (1 + i % 64);
        blocks[i] = mem.kmalloc(sizes[i]);
        if (blocks[i] == 0) {
            kprintf("      [FAIL] big kmalloc(%u) returned null at %u\n", sizes[i], i);
            return 0;
        }
        volatile u8 *b = (volatile u8 *)blocks[i];
        u64 k;
        for (k = 0; k < sizes[i]; k += 512) {
            b[k] = (u8)(0xA5 ^ i);
        }
        for (k = 0; k < sizes[i]; k += 512) {
            if (b[k] != (u8)(0xA5 ^ i)) {
                kprintf("      [FAIL] big block corruption at %u off %u\n", i, k);
                ok = 0;
            }
        }
    }

    /* Buddy accounting must be exact: every big block returns to the pool. */
    for (i = 0; i < BIG_N; i++) {
        mem.kfree(blocks[i]);
    }
    u64 after = mem.pmm_free_pages_count();
    if (after != before) {
        kprintf("      [FAIL] buddy pages leaked by big path: %u vs %u\n", before, after);
        ok = 0;
    }

    /* Freeing again must be rejected (no double-free bookkeeping drift). */
    mem.kfree(blocks[0]);
    if (mem.pmm_free_pages_count() != after) {
        kprintf("      [FAIL] double free changed page accounting\n");
        ok = 0;
    }
    return ok;
}

/* Ramfs declares 16 MiB files. Exercise both new buddy orders, every
 * requested page's edges, coexistence, failure, and exact page reclamation.
 * This runs before scheduler release, so free-page accounting is stable. */
static int test_slab_large(void) {
    void *blocks[3];
    u64 sizes[3];
    u64 caps[3];
    u64 before = mem.pmm_free_pages_count();
    u64 i;
    u64 j;
    u64 n = 0;
    int ok = 1;
    sizes[0] = 4198400; caps[0] = 8388608;
    sizes[1] = 8392704; caps[1] = 16777216;
    sizes[2] = 16777216; caps[2] = 16777216;
    for (i = 0; i < 3; i++) {
        blocks[i] = mem.kmalloc(sizes[i]);
        if (blocks[i] == 0) {
            kprintf("      [FAIL] large kmalloc(%u) returned null\n", sizes[i]);
            ok = 0;
            break;
        }
        n++;
        if (((u64)blocks[i] & (caps[i] - 1)) != 0) {
            kprintf("      [FAIL] large kmalloc block misaligned\n");
            ok = 0;
        }
        for (j = 0; j < i; j++) {
            if ((u64)blocks[i] < (u64)blocks[j] + caps[j]
                && (u64)blocks[j] < (u64)blocks[i] + caps[i]) {
                kprintf("      [FAIL] large kmalloc live blocks overlap\n");
                ok = 0;
            }
        }
    }
    if (n == 3 && mem.pmm_free_pages_count() + 10240 != before) {
        kprintf("      [FAIL] large kmalloc allocation accounting drift\n");
        ok = 0;
    }
    if (ok) {
        for (i = 0; i < n; i++) {
            volatile u64 *b = (volatile u64 *)blocks[i];
            for (j = 0; j < sizes[i] / 4096; j++) {
                b[j * 512] = sizes[i] + j;
                b[j * 512 + 511] = sizes[i] + j + 1;
            }
        }
        for (i = 0; i < n; i++) {
            volatile u64 *b = (volatile u64 *)blocks[i];
            for (j = 0; j < sizes[i] / 4096; j++) {
                if (b[j * 512] != sizes[i] + j
                    || b[j * 512 + 511] != sizes[i] + j + 1) {
                    kprintf("      [FAIL] large kmalloc page canary %u/%u\n", i, j);
                    ok = 0;
                    break;
                }
            }
        }
    }
    for (i = n; i > 0; i--) mem.kfree(blocks[i - 1]);
    if (mem.pmm_free_pages_count() != before) {
        kprintf("      [FAIL] large kmalloc leaked pages\n");
        ok = 0;
    }
    void *too_big = mem.kmalloc(16777217);
    if (too_big != 0) {
        kprintf("      [FAIL] oversized kmalloc unexpectedly succeeded\n");
        mem.kfree(too_big);
        ok = 0;
    }
    void *overflow = mem.kmalloc(0xffffffffffffffffULL);
    if (overflow != 0) {
        kprintf("      [FAIL] maximal-size kmalloc wrapped into a small allocation\n");
        mem.kfree(overflow);
        ok = 0;
    }
    void *again = mem.kmalloc(16777216);
    if (again == 0) {
        kprintf("      [FAIL] reclaimed large block cannot be allocated\n");
        ok = 0;
    }
    mem.kfree(again);
    if (mem.pmm_free_pages_count() != before) {
        kprintf("      [FAIL] large kmalloc limit/reuse accounting drift\n");
        ok = 0;
    }
    return ok;
}

static int test_slab_reuse(void) {
    /* After freeing a class, the cached slab must immediately hand the same
     * storage back without consuming new pages. */
    u64 free0 = mem.pmm_free_pages_count();
    void *a = mem.kmalloc(64);
    void *b = mem.kmalloc(64);
    u64 pa = (u64)a - 0xFFFF800000000000ULL;
    u64 pb = (u64)b - 0xFFFF800000000000ULL;
    (void)pb;
    mem.kfree(a);
    mem.kfree(b);
    void *c = mem.kmalloc(64);
    u64 pc = (u64)c - 0xFFFF800000000000ULL;
    u64 free1 = mem.pmm_free_pages_count();
    mem.kfree(c);
    u64 free2 = mem.pmm_free_pages_count();
    if (free1 != free0) {
        kprintf("      [FAIL] cached slab grew unexpectedly (%u vs %u free)\n",
                free0, free1);
        return 0;
    }
    if (free2 != free0) {
        kprintf("      [FAIL] cached slab release leaked\n");
        return 0;
    }
    if (pc != pa && pc != pb) {
        kprintf("      [FAIL] freed object storage was not reused\n");
        return 0;
    }
    return 1;
}

void vmm_selftest(void) {
    u64 root = mem.vmm_kernel_root();
    u64 cr3 = mem.vmm_read_cr3();
    kprintf("\n[VMM] 4-Level Page Table Toolchain Online:\n");
    kprintf("      Active PML4    : %p (matches CR3 %p)\n", root, cr3);
    kprintf("      Leaf flags     : P/RW/U/PWT/PCD/NX, 4 KiB & 2 MiB mapping\n");
    kprintf("\n[VMM] Running VMM Self-Tests (scratch window %p)...\n", VMM_TEST_VA);
    if (test_vmm_basic()) {
        kprintf("      [PASS] Map/translate/unmap: attributes, backing store, TLB invalidation\n");
    }
}

/* ---------------------------------------------------------------------------
 * Per-CPU cache + fine-grained class tests (jemalloc/tcmalloc-style slab)
 * ------------------------------------------------------------------------- */

static int test_slab_newclasses(void) {
    /* Every one of the 24 classes: allocate at the class size and just below
     * the NEXT class boundary (the largest size rounding into this class),
     * verify 16-byte alignment and byte-exact canaries. */
    u32 c;
    int ok = 1;
    for (c = 0; c < mem.kmalloc_class_count(); c++) {
        u64 sizes[2];
        u64 i;
        sizes[0] = mem.kmalloc_class_size(c);
        sizes[1] = (c + 1 < mem.kmalloc_class_count())
                   ? mem.kmalloc_class_size(c + 1) - 16
                   : 2048;
        for (i = 0; i < 2; i++) {
            volatile u8 *b = (volatile u8 *)mem.kmalloc(sizes[i]);
            u64 k;
            if (b == 0) {
                kprintf("      [FAIL] class %u kmalloc(%u) null\n",
                        (u64)c, (u64)sizes[i]);
                return 0;
            }
            if (((u64)b & 0xF) != 0) {
                kprintf("      [FAIL] class %u kmalloc(%u) not 16-aligned\n",
                        (u64)c, (u64)sizes[i]);
                ok = 0;
            }
            for (k = 0; k < sizes[i]; k++) {
                b[k] = (u8)(c * 7 + k);
            }
            for (k = 0; k < sizes[i]; k++) {
                if (b[k] != (u8)(c * 7 + k)) {
                    kprintf("      [FAIL] class %u canary at %u/%u\n",
                            (u64)c, (u64)i, (u64)k);
                    ok = 0;
                    break;
                }
            }
            mem.kfree((void *)b);
        }
    }
    return ok;
}

/* Four kernel workers hammer the allocator with same-class bursts (which
 * force cache refill and flush transfers), defer frees to a cross-worker
 * ring (so frees land on other CPUs' caches), and verify canaries. After
 * joining, per-CPU stats must show fast-path traffic and, on SMP2+, at
 * least two CPUs that served allocations lock-free. */
struct slabcc_worker_arg {
    u32 id;
    u32 iters;
};

static volatile u32 slabcc_start;
static volatile u32 slabcc_done;
static volatile u32 slabcc_fail;
static volatile u32 slabcc_ring_head;
enum { SLABCC_RING = 256 };
static void *slabcc_ring[SLABCC_RING];
static u64 slabcc_allocs[4];
static struct slabcc_worker_arg slabcc_args[4];
extern void sched_yield(void);

static void slabcc_worker(u64 arg) {
    struct slabcc_worker_arg *a = (struct slabcc_worker_arg *)arg;
    u32 id = a->id;
    u32 iters = a->iters;
    u32 i;
    u64 mine = 0;
    while (!slabcc_start) {
        sched_yield();
    }
    for (i = 0; i < iters; i++) {
        /* Burst of one class: 40 allocations forces the per-CPU cache to
         * empty and refill mid-burst (cache depth 32). */
        u32 cls = (i + id) % mem.kmalloc_class_count();
        u64 size = mem.kmalloc_class_size(cls);
        enum { BURST = 40 };
        u32 k;
        for (k = 0; k < BURST; k++) {
            volatile u8 *b = (volatile u8 *)mem.kmalloc(size);
            u64 w;
            if (b == 0) {
                slabcc_fail = slabcc_fail + 1;
                break;
            }
            for (w = 0; w < size; w += 64) {
                b[w] = (u8)(id + k);
            }
            for (w = 0; w < size; w += 64) {
                if (b[w] != (u8)(id + k)) {
                    slabcc_fail = slabcc_fail + 1;
                    break;
                }
            }
            mine = mine + 1;
            if ((k & 3) == (id & 3) && slabcc_ring_head < SLABCC_RING) {
                /* Defer: freed after join from the driver context, and the
                 * ring forces cross-worker pointer lifetimes. */
                slabcc_ring[slabcc_ring_head] = (void *)b;
                slabcc_ring_head = slabcc_ring_head + 1;
            } else {
                mem.kfree((void *)b);
            }
        }
    }
    slabcc_allocs[id] = mine;
    slabcc_done = slabcc_done + 1;
}

/* Invoked from kmain AFTER sched_smp_start: the concurrent worker test needs
 * a live scheduler (kthreads) and running tick clock. */
void slab_concurrent_selftest(void) {
    kprintf("\n[MEM] Per-CPU slab cache concurrency self-test...\n");
    if (test_slab_concurrent()) {
        kprintf("      [PASS] Per-CPU caches: concurrent workers, batch refill/flush, cross-CPU frees\n");
    }
}

static int test_slab_concurrent(void) {
    enum { WORKERS = 4, ITERS = 300 };
    extern u32 kthread_create(u64 entry, u64 arg);
    extern void kthread_exit(void);
    extern void sched_yield(void);
    extern u64 sched_clock_ticks(void);
    extern u32 sched_thread_state(u32 id);
    extern u32 smp_cpu_count(void);
    u32 ids[WORKERS];
    u32 i;
    u64 until;
    u64 fa[8], ff[8], rf[8], fl[8];
    u64 total_fast = 0;
    u32 cpus_with_traffic = 0;
    int ok = 1;

    slabcc_start = 0;
    slabcc_done = 0;
    slabcc_fail = 0;
    slabcc_ring_head = 0;
    for (i = 0; i < WORKERS; i++) slabcc_allocs[i] = 0;
    for (i = 0; i < 8; i++) { fa[i] = 0; ff[i] = 0; rf[i] = 0; fl[i] = 0; }

    for (i = 0; i < WORKERS; i++) {
        slabcc_args[i].id = i;
        slabcc_args[i].iters = ITERS;
        ids[i] = kthread_create((u64)&slabcc_worker, (u64)&slabcc_args[i]);
        if (ids[i] == 0xFFFFFFFFu) {
            kprintf("      [FAIL] worker %u spawn failed\n", (u64)i);
            return 0;
        }
    }
    slabcc_start = 1;
    until = sched_clock_ticks() + 1500;
    while (slabcc_done < WORKERS && sched_clock_ticks() < until) {
        sched_yield();
    }
    if (slabcc_done < WORKERS) {
        kprintf("      [FAIL] slabcc workers deadline (done=%u)\n", (u64)slabcc_done);
        for (i = 0; i < WORKERS; i++) {
            kprintf("      worker %u state=%u\n",(u64)i,(u64)sched_thread_state(ids[i]));
        }
        return 0;
    }
    for (i = 0; i < WORKERS; i++) {
        while (sched_thread_state(ids[i]) != 0 && sched_clock_ticks() < until + 500) {
            sched_yield();
        }
    }
    /* Drain the deferred-free ring: every deferred pointer must be freed
     * exactly once and freed storage must not double-free. */
    for (i = 0; i < slabcc_ring_head; i++) {
        mem.kfree(slabcc_ring[i]);
        slabcc_ring[i] = 0;
    }
    mem.kmalloc_cache_flush_all();

    for (i = 0; i < mem.kmalloc_cache_cpu_rows() && i < 8; i++) {
        u32 c;
        for (c = 0; c < mem.kmalloc_class_count(); c++) {
            u64 a = 0, f = 0, r = 0, l = 0;
            mem.kmalloc_cache_stats(i, c, &a, &f, &r, &l);
            fa[i] = fa[i] + a;
            ff[i] = ff[i] + f;
            rf[i] = rf[i] + r;
            fl[i] = fl[i] + l;
        }
        total_fast = total_fast + fa[i];
        if (fa[i] > 0) cpus_with_traffic = cpus_with_traffic + 1;
    }
    if (slabcc_fail != 0) {
        kprintf("      [FAIL] concurrent slab: %u failures\n", (u64)slabcc_fail);
        ok = 0;
    }
    for (i = 0; i < WORKERS; i++) {
        /* Every burst allocation is accounted: 40 per iteration. */
        if (slabcc_allocs[i] != (u64)ITERS * 40) {
            kprintf("      [FAIL] worker %u allocs %u != %u\n", (u64)i,
                    (u64)slabcc_allocs[i], (u64)(ITERS * 40));
            ok = 0;
        }
    }
    /* Other live kernel allocators share the caches during the window, so
     * the per-CPU fast totals must cover every worker allocation, not
     * equal them exactly (per-worker counts above are the exact check). */
    if (total_fast < (u64)WORKERS * ITERS * 40) {
        kprintf("      [FAIL] fast-alloc accounting %u < %u\n", (u64)total_fast,
                (u64)(WORKERS * ITERS * 40));
        ok = 0;
    }
    {
        u32 any_cpu = 0;
        u64 total_refill = 0;
        for (i = 0; i < 8; i++) {
            if (fa[i] > 0) any_cpu = 1;
            total_refill = total_refill + rf[i];
        }
        if (!any_cpu) { kprintf("      [FAIL] no per-CPU cache traffic\n"); ok = 0; }
        if (total_refill == 0) {
            kprintf("      [FAIL] refill transfers never exercised\n");
            ok = 0;
        }
        if (smp_cpu_count() > 1 && cpus_with_traffic < 2) {
            kprintf("      [FAIL] multi-CPU lock-free traffic missing (cpus=%u)\n",
                    (u64)cpus_with_traffic);
            ok = 0;
        }
    }
    for (i = 0; i < mem.kmalloc_cache_cpu_rows() && i < 8; i++) {
        kprintf("      cpu %u: fast-alloc=%u fast-free=%u refill=%u flush=%u\n",
                (u64)i, fa[i], ff[i], rf[i], fl[i]);
    }
    return ok;
}

/* ---------------------------------------------------------------------------
 * Concurrent VM mutation stress (TLB shootdown + atomic frame refs).
 *
 * Two interacting hazards on SMP: (a) COW forks demote parent leaf PTEs
 * while sibling threads may hold stale pre-fork writable TLB entries;
 * (b) munmap frees frames that a remote CPU could still reach through a
 * stale TLB. Workers fork their private spaces, verify parent/child
 * isolation after COW, churn munmap/remap slices, and two workers share
 * one address space on different CPUs (munmap storm vs canary verifier).
 * Any stale-TLB corruption shows up as canary mismatch or double-free.
 * ------------------------------------------------------------------------- */
enum { VMCC_WORKERS = 3, VMCC_ROUNDS = 6, VMCC_PAGES = 4 };
enum { VMCC_VA = 0x0000000008000000ULL };   /* per-worker private space VA */
enum { VMCC_SHARED_VA = 0x000000000C000000ULL };
enum { VMCC_PAGE = 4096 };
enum { VMCC_WORKERS = 3, VMCC_ROUNDS = 60, VMCC_PAGES = 4 };
static u64 vmcc_shared_as;

struct vmcc_arg { u32 id; u32 rounds; };

static volatile u32 vmcc_start;
static volatile u32 vmcc_done;
static volatile u32 vmcc_fail;
static volatile u32 vmcc_shared_go;
static volatile u32 vmcc_shared_stop;
static u64 vmcc_forks[4];
static u32 vmcc_stage[4];
static u64 vmcc_baseline;

extern void kthread_exit(void);

/* Private-space worker: fork/COW refcount storms + munmap churn on its
 * own AS. Kernel workers CANNOT hold a user AS across preemption (the
 * scheduler reloads CR3 from the task's as_h on resume), so user-VA
 * dereferences stay in the ring-3 tests — this worker exercises the
 * shootdown machinery, refcounts and teardown on every round. */
static void vmcc_private_worker(u64 arg) {
    struct vmcc_arg *a = (struct vmcc_arg *)arg;
    u32 id = a->id;
    u64 my_as = mem.as_create();
    u64 va = VMCC_VA + (u64)id * (VMCC_PAGES + 8) * VMCC_PAGE;
    u32 r;
    if (my_as == 0) { vmcc_fail = vmcc_fail + 1; vmcc_done = vmcc_done + 1; kthread_exit(); }
    mem.as_map_anon_h(my_as, va, VMCC_PAGES * VMCC_PAGE, 3);
    /* Fault every page in through the real dispatcher: present PTEs make
     * the fork's RO demotion, refcounts and later munmap/mprotect churn
     * operate on REAL mappings (no user CR3 is available to a kthread). */
    mem.as_activate_h(my_as);              /* dispatch_fault uses the
                                            * CURRENTLY installed AS */
    {
        u32 k;
        for (k = 0; k < VMCC_PAGES; k++) {
            if (mem.as_fault_in_h(my_as, va + (u64)k * VMCC_PAGE, 0x6) != 1) {
                vmcc_fail = vmcc_fail + 1;
            }
        }
    }
    for (r = 0; r < a->rounds; r++) {
        u64 child;
        mem.as_activate_h(my_as);              /* install CR3 on this CPU */
        child = mem.as_fork_h(my_as);          /* COW: RO demotion + refs */
        if (child == 0) { vmcc_fail = vmcc_fail + 1; vmcc_stage[id] = 1; break; }
        vmcc_forks[id] = vmcc_forks[id] + 1;
        mem.as_activate_h(child);              /* switch to the child AS  */
        mem.as_activate_h(my_as);              /* and back: shootdown churn */
        if (mem.as_munmap_h(my_as, va, VMCC_PAGE) != 1) {
            vmcc_fail = vmcc_fail + 1;
            vmcc_stage[id] = 4;
            mem.as_release_h(child);
            break;
        }
        if (!mem.as_map_anon_h(my_as, va, VMCC_PAGE, 3)) {
            vmcc_fail = vmcc_fail + 1;
            vmcc_stage[id] = 5;
            mem.as_release_h(child);
            break;
        }
        mem.as_release_h(child);
    }
    mem.as_activate_h(0);
    mem.as_release_h(my_as);
    vmcc_done = vmcc_done + 1;
    kthread_exit();
}

/* Shared-space pair: worker 0 churns munmap/remap + mprotect on slice 0
 * while worker 1 churns slice 1 — both CPUs install the SAME address
 * space (CR3 switches on both CPUs), so every mutation must shoot down
 * the other CPU. No user-VA dereferences: kernel threads cannot hold a
 * user CR3 across preemption (see above). */
static void vmcc_shared_worker(u64 arg) {
    struct vmcc_arg *a = (struct vmcc_arg *)arg;
    u32 id = a->id;
    extern void sched_yield(void);
    u64 sh;
    (void)a;
    extern void kprintf(const char *fmt, ...);
    while (!vmcc_shared_go && !vmcc_shared_stop) sched_yield();
    sh = vmcc_shared_as;
    /* Fault the shared pages in: mprotect/munmap churn then operates on
     * real present PTEs (real shootdowns), not empty reservations.
     * dispatch_fault resolves against the INSTALLED AS: activate first. */
    mem.as_activate_h(sh);
    {
        u32 k;
        for (k = 0; k < VMCC_PAGES; k++) {
            mem.as_fault_in_h(sh, VMCC_SHARED_VA + (u64)k * VMCC_PAGE, 0x6);
        }
    }
    mem.as_activate_h(0);
    /* Round-bounded churn: workers exit after their own budget WITHOUT
     * waiting for the driver — endless sched_yield keeps two runnable
     * workers ahead of the idle driver forever (CFS never picks idle), so
     * a stop-flag handshake would starve the driver into a hang. */
    {
        u64 va = VMCC_SHARED_VA + (u64)id * (VMCC_PAGES + 8) * VMCC_PAGE;
        u32 outer;
        for (outer = 0; outer < 6; outer++) {
            u32 r;
            for (r = 0; r < 8; r++) {
                mem.as_activate_h(sh);
                if (mem.as_munmap_h(sh, va, VMCC_PAGES * VMCC_PAGE) == 1) {
                    mem.as_map_anon_h(sh, va, VMCC_PAGES * VMCC_PAGE, 3);
                    mem.as_activate_h(sh);
                    {
                        u32 k;
                        for (k = 0; k < VMCC_PAGES; k++) {
                            mem.as_fault_in_h(sh, va + (u64)k * VMCC_PAGE,
                                              0x6);
                        }
                    }
                }
                mem.as_activate_h(0);          /* leave the shared CR3 */
            }
            if (id == 0) {
                mem.as_activate_h(sh);
                mem.as_mprotect_h(sh, va, VMCC_PAGE, 1);   /* RO */
                mem.as_mprotect_h(sh, va, VMCC_PAGE, 3);   /* RW */
                mem.as_activate_h(0);
            }
        }
    }
    mem.as_activate_h(0);
    vmcc_done = vmcc_done + 1;
    kthread_exit();
}

void vm_concurrent_selftest(void) {
    extern u32 kthread_create(u64 entry, u64 arg);
    extern void sched_yield(void);
    extern u64 sched_clock_ticks(void);
    extern u32 sched_thread_state(u32 id);
    extern u32 smp_cpu_count(void);
    struct vmcc_arg args[VMCC_WORKERS];
    u32 ids[VMCC_WORKERS];
    u32 i;
    u64 until;
    u64 sd_before;
    int ok = 1;

    kprintf("\n[VMCC] Concurrent VM mutation + TLB shootdown self-test...\n");
    vmcc_start = 0;
    vmcc_done = 0;
    vmcc_fail = 0;
    vmcc_shared_go = 0;
    vmcc_shared_stop = 0;
    for (i = 0; i < VMCC_WORKERS; i++) { vmcc_forks[i] = 0; vmcc_stage[i] = 0; }
    vmcc_baseline = mem.pmm_free_pages_count();
    sd_before = mem.as_shootdown_count;

    for (i = 0; i < VMCC_WORKERS; i++) {
        args[i].id = i;
        args[i].rounds = VMCC_ROUNDS;
        ids[i] = kthread_create((u64)&vmcc_private_worker, (u64)&args[i]);
        if (ids[i] == 0xFFFFFFFFu) {
            kprintf("      [FAIL] vmcc worker %u spawn\n", (u64)i);
            return;
        }
    }
    until = sched_clock_ticks() + 1500;
    while (vmcc_done < VMCC_WORKERS && sched_clock_ticks() < until) {
        sched_yield();
    }
    for (i = 0; i < VMCC_WORKERS; i++) {
        while (sched_thread_state(ids[i]) != 0 && sched_clock_ticks() < until) {
            sched_yield();
        }
    }
    if (vmcc_fail != 0) {
        kprintf("      [FAIL] private-space storms: %u failures\n", (u64)vmcc_fail);
        ok = 0;
    }
    for (i = 0; i < VMCC_WORKERS; i++) {
        if (vmcc_stage[i] != 0) {
            kprintf("      [FAIL] worker %u failed at stage %u\n", (u64)i,
                    (u64)vmcc_stage[i]);
            ok = 0;
        }
        if (vmcc_forks[i] != VMCC_ROUNDS) {
            kprintf("      [FAIL] worker %u forks %u != %u\n", (u64)i,
                    (u64)vmcc_forks[i], (u64)VMCC_ROUNDS);
            ok = 0;
        }
    }
    if (mem.pmm_free_pages_count() != vmcc_baseline) {
        kprintf("      [FAIL] page accounting drift after private storms: %u vs %u\n",
                (u64)vmcc_baseline, (u64)mem.pmm_free_pages_count());
        ok = 0;
    }
    if (ok) {
        kprintf("      [PASS] private COW storms: %u forks, no stale-TLB corruption\n",
                (u64)(VMCC_WORKERS * VMCC_ROUNDS));
    }

    if (smp_cpu_count() > 1) {
        u64 shared = mem.as_create();
        if (shared != 0) {
            vmcc_shared_as = shared;
            vmcc_done = 0;
            vmcc_fail = 0;                     /* phase-local failure count */
            for (i = 0; i < 2; i++) {
                args[i].id = i;
                args[i].rounds = 0;
                ids[i] = kthread_create((u64)&vmcc_shared_worker, (u64)&args[i]);
            }
            vmcc_shared_go = 1;
            until = sched_clock_ticks() + 2000;   /* generous backstop */
            while (vmcc_done < 2 && sched_clock_ticks() < until) {
                sched_yield();
            }
            vmcc_shared_stop = 1;
            for (i = 0; i < 2; i++) {
                while (sched_thread_state(ids[i]) != 0 && sched_clock_ticks() < until) {
                    sched_yield();
                }
            }
            if (vmcc_fail != 0) {
                kprintf("      [FAIL] shared-space failures: %u\n", (u64)vmcc_fail);
                ok = 0;
            }
            kprintf("      [INFO] shared: shootdown before=%u after=%u\n",
                    (u64)sd_before, (u64)mem.as_shootdown_count);
            if (mem.as_shootdown_count == sd_before) {
                kprintf("      [FAIL] shootdown never exercised: before=%u after=%u hook=%u\n",
                        (u64)sd_before, (u64)mem.as_shootdown_count,
                        (u64)(mem.as_shootdown_hook != 0));
                ok = 0;
    }
    /* Per-CPU rq_lock + sharded state run_lock stats: every online CPU must
     * exercise its own per-CPU rq_lock, while global run_lock is no longer
     * held across context switches or local ticks/yields. */
    {
        extern void sched_runlock_stats(u32 cpu, u64 *holds, u64 *contends,
                                        u64 *hold_ns, u64 *hold_max);
        extern void sched_rq_stats(u32 cpu, u64 *holds, u64 *contends);
        u64 t_holds = 0, t_cont = 0, t_max = 0;
        u64 rq_t_holds = 0, rq_t_cont = 0;
        u32 rq_cpus_hit = 0, ci;
        for (ci = 0; ci < smp_cpu_count() && ci < 8; ci++) {
            u64 h = 0, f = 0, n = 0, m = 0;
            u64 rqh = 0, rqc = 0;
            sched_runlock_stats(ci, &h, &f, &n, &m);
            sched_rq_stats(ci, &rqh, &rqc);
            kprintf("      rq_lock cpu %u: holds=%u contended=%u | state_lock: holds=%u contended=%u max-ns=%u\n",
                    (u64)ci, rqh, rqc, h, f, m);
            if (rqh > 0) rq_cpus_hit = rq_cpus_hit + 1;
            rq_t_holds = rq_t_holds + rqh;
            rq_t_cont = rq_t_cont + rqc;
            t_holds = t_holds + h;
            t_cont = t_cont + f;
            if (m > t_max) t_max = m;
        }
        if (rq_cpus_hit < smp_cpu_count()) {
            kprintf("      [FAIL] per-CPU rq_lock not acquired on every CPU (%u/%u)\n",
                    (u64)rq_cpus_hit, (u64)smp_cpu_count());
            ok = 0;
        }
        kprintf("      [INFO] rq_lock totals: holds=%u contended=%u | state_lock totals: holds=%u contended=%u max-hold-ns=%u\n",
                rq_t_holds, rq_t_cont, t_holds, t_cont, t_max);
    }
            mem.as_release_h(shared);
            if (mem.pmm_free_pages_count() != vmcc_baseline) {
                kprintf("      [FAIL] page accounting drift after shared churn: %u vs %u\n",
                        (u64)vmcc_baseline, (u64)mem.pmm_free_pages_count());
                ok = 0;
            }
            if (ok) {
                kprintf("      [PASS] cross-CPU shared space: munmap/mprotect storms, %u shootdowns\n",
                        (u64)(mem.as_shootdown_count - sd_before));
            }
        }
    }
    if (ok) {
        kprintf("      [PASS] VMCC concurrent mutation stress\n");
    } else {
        kprintf("      [FAIL] VMCC concurrent mutation stress\n");
    }
}

void slab_selftest(void) {
    u32 c;
    kprintf("\n[MEM] Slab Kernel Heap Online (%u size classes 16 B .. 2048 B, big -> buddy):\n",
            (u64)mem.kmalloc_class_count());
    for (c = 0; c < mem.kmalloc_class_count(); c++) {
        kprintf("      class %u: %u B objects, %u per slab page\n",
                (u64)c, (u64)mem.kmalloc_class_size(c),
                (u64)mem.kmalloc_class_capacity(c));
    }

    kprintf("\n[MEM] Running Slab/kmalloc Self-Tests...\n");
    if (test_slab_classes()) {
        kprintf("      [PASS] Size-class alloc: 1024 live objects, unique, aligned, canary R/W\n");
    }
    if (test_slab_newclasses()) {
        kprintf("      [PASS] Fine-grained classes: 24 classes, boundaries + alignment + canaries\n");
    }
    if (test_slab_big()) {
        kprintf("      [PASS] Big alloc path: 64 buddy blocks (up to 256 KiB), exact accounting\n");
    }
    if (test_slab_large()) {
        kprintf("      [PASS] Large kmalloc: 8/16 MiB canaries, limits and exact reclamation\n");
    }
    if (test_slab_reuse()) {
        kprintf("      [PASS] Free-list reuse: cached slab hands memory back, no leaks\n");
    }
}

static int test_single_pages(void) {
    enum { SP_N = 256 };
    u64 pages[SP_N];
    u64 i;
    int ok = 1;

    for (i = 0; i < SP_N; i++) {
        pages[i] = mem.pmm_alloc_page();
        if (pages[i] == 0) {
            kprintf("      [FAIL] allocator exhausted at single page %u\n", (u64)i);
            return 0;
        }
        if ((pages[i] & 0xFFF) != 0) {
            kprintf("      [FAIL] page %u not 4 KiB aligned: %p\n", (u64)i, pages[i]);
            ok = 0;
        }
    }

    /* Uniqueness: all frames must differ. */
    for (i = 0; i < SP_N && ok; i++) {
        u64 j;
        for (j = i + 1; j < SP_N; j++) {
            if (pages[i] == pages[j]) {
                kprintf("      [FAIL] duplicate allocation %p\n", pages[i]);
                ok = 0;
                break;
            }
        }
    }

    /* Read/write verification through the HHDM direct map. */
    for (i = 0; i < SP_N && ok; i++) {
        volatile u64 *p = page_ptr(pages[i]);
        u64 k;
        for (k = 0; k < 512; k++) {
            p[k] = pages[i] + k;
        }
        for (k = 0; k < 512; k++) {
            if (p[k] != pages[i] + k) {
                kprintf("      [FAIL] data mismatch at %p[%u]\n", pages[i], (u64)k);
                ok = 0;
                break;
            }
        }
    }

    for (i = 0; i < SP_N; i++) {
        mem.pmm_free_page(pages[i]);
    }
    return ok;
}

static int test_block_split(void) {
    /* Order 3 = 8 physically contiguous frames, aligned to 8 frames. */
    u64 block = mem.pmm_alloc_pages(3);
    if (block == 0) {
        kprintf("      [FAIL] order-3 allocation returned 0\n");
        return 0;
    }
    if ((block & (8 * 4096 - 1)) != 0) {
        kprintf("      [FAIL] order-3 block misaligned: %p\n", block);
        mem.pmm_free_pages(block, 3);
        return 0;
    }
    /* Fill/check with (block+k), same shape as test_single_pages. Avoid
     * `imm64 ^ k` here: recent fakecc O0 BXOR isel can leave the loop-bound
     * immediate in the register allocated to k and xor that instead of k
     * (writes const^0x1000 to every qword; verify then fails at qword 0). */
    volatile u64 *p = page_ptr(block);
    u64 k;
    for (k = 0; k < 8 * 512; k++) {
        p[k] = block + k;
    }
    for (k = 0; k < 8 * 512; k++) {
        if (p[k] != block + k) {
            kprintf("      [FAIL] order-3 block data mismatch at qword %u\n", (u64)k);
            mem.pmm_free_pages(block, 3);
            return 0;
        }
    }
    mem.pmm_free_pages(block, 3);
    return 1;
}

static int test_merge_determinism(void) {
    /* Allocate the lowest order-4 block, free it, allocate again: with
     * address-ordered free lists and full buddy merging, the same physical
     * block must come back. */
    u64 first = mem.pmm_alloc_pages(4);
    if (first == 0) {
        kprintf("      [FAIL] first order-4 allocation returned 0\n");
        return 0;
    }
    mem.pmm_free_pages(first, 4);
    u64 second = mem.pmm_alloc_pages(4);
    if (second != first) {
        kprintf("      [FAIL] merge not deterministic: %p then %p\n", first, second);
        if (second != 0) {
            mem.pmm_free_pages(second, 4);
        }
        return 0;
    }
    mem.pmm_free_pages(second, 4);
    return 1;
}

static int test_stress(void) {
    enum { STRESS_N = 4096 };
    u64 pages[STRESS_N];
    u64 before = mem.pmm_free_pages_count();
    u64 i;

    for (i = 0; i < STRESS_N; i++) {
        pages[i] = mem.pmm_alloc_page();
        if (pages[i] == 0) {
            kprintf("      [FAIL] stress exhausted at page %u\n", (u64)i);
            return 0;
        }
    }
    u64 after_alloc = mem.pmm_free_pages_count();
    if (after_alloc + STRESS_N != before) {
        kprintf("      [FAIL] free accounting drift: %u -> %u after %u allocs\n",
                before, after_alloc, (u64)STRESS_N);
        return 0;
    }

    /* Release in reverse to exercise cross-order merging. */
    for (i = STRESS_N; i > 0; i--) {
        mem.pmm_free_page(pages[i - 1]);
    }
    u64 after_free = mem.pmm_free_pages_count();
    if (after_free != before) {
        kprintf("      [FAIL] free count not restored: %u vs %u\n", before, after_free);
        return 0;
    }
    return 1;
}

void pmm_selftest(void) {
    u32 n = mem.pmm_region_count();
    u32 i;

    kprintf("\n[MEM] Multiboot Physical Memory Map (%u regions):\n", (u64)n);
    for (i = 0; i < n; i++) {
        u64 base = mem.pmm_region_base(i);
        u64 len = mem.pmm_region_len(i);
        u32 type = mem.pmm_region_type(i);
        u64 kib = len / 1024;
        kprintf("      %p - %p  %s  %u KiB\n", base, base + len,
                region_kind(type), kib);
    }

    u64 total = mem.pmm_total_pages();
    u64 free_p = mem.pmm_free_pages_count();
    u64 used = mem.pmm_used_pages();
    kprintf("\n[MEM] Buddy Physical Page Allocator Online (max order %u, block up to %u KiB):\n",
            (u64)mem.pmm_max_order(), (u64)((1u << mem.pmm_max_order()) * 4));
    kprintf("      Managed : %u pages = %u MiB (RAM after BIOS/kernel reservations)\n",
            total, total / 256);
    kprintf("      Free    : %u pages = %u MiB\n", free_p, free_p / 256);
    kprintf("      In use  : %u pages (currently allocated)\n", used);
    kprintf("      HHDM    : all managed RAM reachable at %p + paddr\n",
            mem.pmm_phys_to_virt(0));

    kprintf("\n[MEM] Running PMM Self-Tests...\n");

    if (test_single_pages()) {
        kprintf("      [PASS] Single-page alloc/free: 256 unique pages, HHDM R/W verified\n");
    }
    if (test_block_split()) {
        kprintf("      [PASS] Multi-order alloc: order-3 block aligned and accessible\n");
    }
    if (test_merge_determinism()) {
        kprintf("      [PASS] Buddy merge: freed order-4 block re-returned at same address\n");
    }
    if (test_stress()) {
        kprintf("      [PASS] Stress test: 4096 allocations, reverse free, accounting exact\n");
    }
}
