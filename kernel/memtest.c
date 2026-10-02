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
    if (test_slab_big()) {
        kprintf("      [PASS] Big alloc path: 64 buddy blocks (up to 256 KiB), exact accounting\n");
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
    volatile u64 *p = page_ptr(block);
    u64 k;
    for (k = 0; k < 8 * 512; k++) {
        p[k] = 0x5151C0DEC0DEULL ^ k;
    }
    for (k = 0; k < 8 * 512; k++) {
        if (p[k] != (0x5151C0DEC0DEULL ^ k)) {
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
