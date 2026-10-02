package mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern u64 pmm_alloc_page(void);
extern void pmm_free_page(u64 paddr);
extern void pmm_free_pages(u64 paddr, u32 order);
extern u64 vmm_kernel_root(void);
extern void cr3_write(u64 value);
extern u64 cr3_read(void);
extern void invlpg_page(u64 vaddr);
extern void tlb_flush(void);

/* ---------------------------------------------------------------------------
 * Virtual address spaces, VMAs, demand paging and copy-on-write
 *
 * An address space owns one PML4 root. Higher-half slots (PML4 256..511) are
 * shared verbatim with the kernel root (HHDM + kernel image), so every
 * address space can still execute kernel code and touch physical memory;
 * lower-half slots (0..255) are private per-process page tables.
 *
 * Anonymous VMAs reserve a range with permissions but map nothing; the first
 * touch raises #PF and as_handle_fault() demand-allocates a zeroed page.
 * as_cow_clone() shallow-copies an address space, marking every shared leaf
 * read-only and bumping a per-frame refcount; the first write after that
 * triggers a private copy.
 * ------------------------------------------------------------------------- */

static const u64 HHDM_BASE = 0xFFFF800000000000ULL;
static const u64 PAGE_SIZE = 4096;

enum {
    PTE_P      = 1,
    PTE_W      = 2,
    PTE_U      = 4,
    PTE_NX     = 1u << 31,
    PT_ENTRIES = 512,
    MAX_VMA    = 64,
    MAX_FRAMES_AS = 1048576,     /* 4 GiB / 4 KiB, matches the PMM ceiling */

    VMA_FREE   = 0,
    VMA_ANON   = 1,

    PROT_READ  = 1,
    PROT_WRITE = 2,
    PROT_EXEC  = 4
};

struct vma {
    u64 start;
    u64 end;
    u32 prot;
    u32 kind;
};

struct address_space {
    u64 pml4;
    struct vma vmas[MAX_VMA];
    u32 nvma;
};

/* Per-frame share count for COW. 1 = single owner (plain writable page),
 * >=2 = mapped read-only into that many address spaces / PTEs. */
static u16 frame_refs[MAX_FRAMES_AS];

/* The address space currently installed in CR3; 0 while the kernel runs on
 * its bootstrap root. */
static struct address_space *current_as;

extern void kprintf(const char *fmt, ...);

static volatile u64 *table(u64 phys) {
    return (volatile u64 *)(HHDM_BASE + phys);
}

static u32 pml4_i(u64 v) { return (u32)((v >> 39) & 511); }
static u32 pdpt_i(u64 v) { return (u32)((v >> 30) & 511); }
static u32 pd_i(u64 v)   { return (u32)((v >> 21) & 511); }
static u32 pt_i(u64 v)   { return (u32)((v >> 12) & 511); }

static u64 entry_phys(u64 e) {
    return e & 0x000FFFFFFFFFF000ULL;
}

static u64 alloc_zero_frame(void) {
    u64 f = pmm_alloc_page();
    if (f == 0) {
        return 0;
    }
    volatile u64 *t = table(f);
    int i;
    for (i = 0; i < PT_ENTRIES; i++) {
        t[i] = 0;
    }
    return f;
}

/* Walk to the PT governing vaddr, creating missing directories when create
 * is non-zero. Returns 0 if absent/not creatable. Only handles 4 KiB chains
 * in the private lower half. */
static volatile u64 *resolve_pt(u64 pml4, u64 vaddr, int create, u64 leaf_flags) {
    u64 e = table(pml4)[pml4_i(vaddr)];
    if (!(e & PTE_P)) {
        if (!create) return 0;
        u64 f = alloc_zero_frame();
        if (f == 0) return 0;
        e = f | PTE_P | PTE_W | (leaf_flags & PTE_U);
        table(pml4)[pml4_i(vaddr)] = e;
    }
    u64 pdpt = entry_phys(e);

    e = table(pdpt)[pdpt_i(vaddr)];
    if (!(e & PTE_P)) {
        if (!create) return 0;
        u64 f = alloc_zero_frame();
        if (f == 0) return 0;
        e = f | PTE_P | PTE_W | (leaf_flags & PTE_U);
        table(pdpt)[pdpt_i(vaddr)] = e;
    }
    if (e & (1u << 7)) {
        return 0;                     /* 1 GiB huge leaf: unsupported here */
    }
    u64 pd = entry_phys(e);

    e = table(pd)[pd_i(vaddr)];
    if (!(e & PTE_P)) {
        if (!create) return 0;
        u64 f = alloc_zero_frame();
        if (f == 0) return 0;
        e = f | PTE_P | PTE_W | (leaf_flags & PTE_U);
        table(pd)[pd_i(vaddr)] = e;
    }
    if (e & (1u << 7)) {
        return 0;                     /* 2 MiB huge leaf: unsupported here */
    }
    return table(entry_phys(e));
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void as_init(struct address_space *as) {
    u32 i;
    as->nvma = 0;
    for (i = 0; i < MAX_VMA; i++) {
        as->vmas[i].kind = VMA_FREE;
        as->vmas[i].start = 0;
        as->vmas[i].end = 0;
        as->vmas[i].prot = 0;
    }

    u64 root = alloc_zero_frame();
    as->pml4 = root;

    /* Share all kernel/HHDM slots (256..511) with the bootstrap root. */
    volatile u64 *k = table(vmm_kernel_root());
    volatile u64 *n = table(root);
    for (i = 256; i < PT_ENTRIES; i++) {
        n[i] = k[i];
    }
}

void as_activate(struct address_space *as) {
    current_as = as;
    if (as) {
        cr3_write(as->pml4);
    } else {
        cr3_write(vmm_kernel_root());
    }
}

/* Reserve an anonymous demand-paged range (page aligned). Returns 1 on
 * success; overlapping reservations are rejected. */
int as_map_anon(struct address_space *as, u64 start, u64 size, u32 prot) {
    if ((start & 0xFFF) != 0 || size == 0) {
        return 0;
    }
    u64 end = (start + size + 0xFFF) & ~0xFFFULL;
    u32 i;
    for (i = 0; i < as->nvma; i++) {
        if (start < as->vmas[i].end && as->vmas[i].start < end) {
            return 0;                 /* Overlap */
        }
    }
    if (as->nvma >= MAX_VMA) {
        return 0;
    }
    as->vmas[as->nvma].start = start;
    as->vmas[as->nvma].end = end;
    as->vmas[as->nvma].prot = prot | PROT_READ;
    as->vmas[as->nvma].kind = VMA_ANON;
    as->nvma++;
    return 1;
}

static struct vma *find_vma(struct address_space *as, u64 vaddr) {
    u32 i;
    for (i = 0; i < as->nvma; i++) {
        if (vaddr >= as->vmas[i].start && vaddr < as->vmas[i].end) {
            return &as->vmas[i];
        }
    }
    return 0;
}

static u64 leaf_perms(struct vma *vma) {
    u64 e = PTE_P | PTE_U;            /* Anon user pages */
    if (vma->prot & PROT_WRITE) {
        e |= PTE_W;
    }
    if (!(vma->prot & PROT_EXEC)) {
        e |= ((u64)1 << 63);          /* NX unless the VMA is executable */
    }
    return e;
}

/* ---------------------------------------------------------------------------
 * Page fault handling
 *
 * err bits: 0=protection (vs non-present), 1=write, 2=user.
 * Returns 1 when the fault was resolved (resume), 0 when unhandled.
 * ------------------------------------------------------------------------- */
extern void kprintf(const char *fmt, ...);

int as_handle_fault(struct address_space *as, u64 vaddr, u64 err) {
    u64 page_va = vaddr & ~0xFFFULL;
    struct vma *vma = find_vma(as, page_va);
    if (vma == 0) {
        return 0;
    }

    volatile u64 *pt = resolve_pt(as->pml4, vaddr, 1, PTE_U);
    if (pt == 0) {
        return 0;
    }
    u32 i = pt_i(vaddr);
    u64 pte = pt[i];

    if (!(pte & PTE_P)) {
        /* Demand-zero page. */
        if ((err & 0x2) && !(vma->prot & PROT_WRITE)) {
            return 0;                 /* Write to a read-only VMA */
        }
        u64 frame = alloc_zero_frame();
        if (frame == 0) {
            return 0;
        }
        frame_refs[(u32)(frame / PAGE_SIZE)] = 1;
        pt[i] = frame | leaf_perms(vma);
        invlpg_page(page_va);
        return 1;
    }

    /* Present page: the only resolvable protection fault is a write to a
     * write-protected COW page. err bit2 (user) is ignored: bring-up tests
     * touch the user half from ring 0 before ring 3 exists. */
    if ((err & 0x1) && (err & 0x2) && !(pte & PTE_W)) {
        if (!(vma->prot & PROT_WRITE)) {
            return 0;
        }
        u64 old = entry_phys(pte);
        u32 pfn = (u32)(old / PAGE_SIZE);
        if (frame_refs[pfn] <= 1) {
            /* Last owner: just lift write protection. */
            frame_refs[pfn] = 1;
            pt[i] = pte | PTE_W;
            invlpg_page(page_va);
            return 1;
        }
        u64 fresh = alloc_zero_frame();
        if (fresh == 0) {
            return 0;
        }
        volatile u8 *src = (volatile u8 *)(HHDM_BASE + old);
        volatile u8 *dst = (volatile u8 *)(HHDM_BASE + fresh);
        u32 k;
        for (k = 0; k < 4096; k++) {
            dst[k] = src[k];
        }
        frame_refs[(u32)(fresh / PAGE_SIZE)] = 1;
        frame_refs[pfn]--;
        pt[i] = fresh | leaf_perms(vma);
        invlpg_page(page_va);
        return 1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Copy-on-write cloning of the private lower half
 * ------------------------------------------------------------------------- */

/* Copy every present leaf of a 4 KiB PT into dst_pt, bumping the frame
 * refcount for every shared page. Writable leaves are demoted to read-only
 * in BOTH spaces (private copy on first write); non-writable leaves (e.g. RX
 * text) stay shared as-is. The refcount bump is mandatory for both kinds:
 * without it, a child exit would free frames the parent still maps. */
static void cow_copy_pt(volatile u64 *src_pt, volatile u64 *dst_pt) {
    u32 i;
    for (i = 0; i < PT_ENTRIES; i++) {
        u64 e = src_pt[i];
        if (!(e & PTE_P)) {
            dst_pt[i] = e;
            continue;
        }
        u32 pfn = (u32)(entry_phys(e) / PAGE_SIZE);
        frame_refs[pfn]++;
        if (e & PTE_W) {
            u64 ro = (e & ~(u64)PTE_W) | ((u64)1 << 63);
            src_pt[i] = ro;
            dst_pt[i] = ro;
        } else {
            dst_pt[i] = e;            /* Already RO / shared */
        }
    }
}

static void cow_walk_pd(volatile u64 *src_pd, volatile u64 *dst_pd) {
    u32 i;
    for (i = 0; i < PT_ENTRIES; i++) {
        u64 e = src_pd[i];
        if (!(e & PTE_P)) {
            continue;
        }
        if (e & (1u << 7)) {
            dst_pd[i] = e;            /* Pass 2 MiB leaves through untouched */
            continue;
        }
        u64 f = alloc_zero_frame();
        if (f == 0) {
            return;
        }
        /* Point the destination PD entry at the NEW PT frame f (not the
         * source PT), then deep-copy leaves into it. */
        dst_pd[i] = f | PTE_P | PTE_W | PTE_U;
        cow_copy_pt(table(entry_phys(e)), table(f));
    }
}

static void cow_walk_pdpt(volatile u64 *src_pdpt, volatile u64 *dst_pdpt) {
    u32 i;
    for (i = 0; i < PT_ENTRIES; i++) {
        u64 e = src_pdpt[i];
        if (!(e & PTE_P)) {
            continue;
        }
        if (e & (1u << 7)) {
            dst_pdpt[i] = e;          /* Pass 1 GiB leaves through untouched */
            continue;
        }
        u64 f = alloc_zero_frame();
        if (f == 0) {
            return;
        }
        /* Point at the NEW PD frame f, then deep-copy its PTs. */
        dst_pdpt[i] = f | PTE_P | PTE_W | PTE_U;
        cow_walk_pd(table(entry_phys(e)), table(f));
    }
}

void as_cow_clone(struct address_space *dst, struct address_space *src) {
    u32 i;
    as_init(dst);

    /* Duplicate the VMA reservations. */
    dst->nvma = src->nvma;
    for (i = 0; i < src->nvma; i++) {
        dst->vmas[i] = src->vmas[i];
    }

    /* Deep-copy the private PML4 slots. Each present entry gets a fresh
     * PDPT frame f which is linked into the child PML4 and then recursively
     * populated (leaves shared read-only, directories private). */
    volatile u64 *s = table(src->pml4);
    volatile u64 *d = table(dst->pml4);
    for (i = 0; i < 256; i++) {
        u64 e = s[i];
        if (!(e & PTE_P)) {
            continue;
        }
        u64 f = alloc_zero_frame();
        if (f == 0) {
            return;
        }
        d[i] = f | PTE_P | PTE_W | PTE_U;
        cow_walk_pdpt(table(entry_phys(e)), table(f));
    }

    /* The demotion above rewrote the SOURCE page tables W->RO in place. If the
     * source space is the one currently installed, its cached writable TLB
     * entries must be dropped: otherwise the parent would keep writing the
     * now-shared pages straight through stale TLB entries (no fault, no COW),
     * silently corrupting the child's view (observed: a fork child's user
     * stack return address overwritten by the parent's later calls). */
    if (current_as == src) {
        tlb_flush();
    }
}

/* ---------------------------------------------------------------------------
 * Teardown: release private leaves and page-table frames (lower half only).
 * Refcounted leaves are freed only when this is the last owner.
 * ------------------------------------------------------------------------- */

static void release_pt(volatile u64 *pt, u64 pt_phys) {
    u32 i;
    for (i = 0; i < PT_ENTRIES; i++) {
        u64 e = pt[i];
        if (e & PTE_P) {
            u32 pfn = (u32)(entry_phys(e) / PAGE_SIZE);
            if (frame_refs[pfn] > 1) {
                frame_refs[pfn]--;
            } else {
                frame_refs[pfn] = 0;
                pmm_free_page(entry_phys(e));
            }
        }
    }
    pmm_free_page(pt_phys);
}

static void release_pd(volatile u64 *pd, u64 pd_phys) {
    u32 i;
    for (i = 0; i < PT_ENTRIES; i++) {
        u64 e = pd[i];
        if (!(e & PTE_P)) {
            continue;
        }
        if (e & (1u << 7)) {
            pmm_free_pages(entry_phys(e), 9);   /* 2 MiB block, order 9 */
        } else {
            release_pt(table(entry_phys(e)), entry_phys(e));
        }
    }
    pmm_free_page(pd_phys);
}

static void release_pdpt(volatile u64 *pdpt, u64 pdpt_phys) {
    u32 i;
    for (i = 0; i < PT_ENTRIES; i++) {
        u64 e = pdpt[i];
        if (!(e & PTE_P)) {
            continue;
        }
        if (e & (1u << 7)) {
            continue;                       /* Boot 1 GiB maps are shared */
        }
        release_pd(table(entry_phys(e)), entry_phys(e));
    }
    pmm_free_page(pdpt_phys);
}

void as_destroy(struct address_space *as) {
    u32 i;
    volatile u64 *p = table(as->pml4);
    for (i = 0; i < 256; i++) {
        u64 e = p[i];
        if (!(e & PTE_P)) {
            continue;
        }
        release_pdpt(table(entry_phys(e)), entry_phys(e));
    }
    pmm_free_page(as->pml4);
    as->pml4 = 0;
    as->nvma = 0;
}

/* ---------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

u64 as_current_pml4(void) {
    return current_as ? current_as->pml4 : vmm_kernel_root();
}

/* Opaque "a user/process address space is installed" indicator for the
 * kernel #PF handler (the concrete struct stays private to this package). */
u64 as_current_handle(void) {
    return current_as ? 1 : 0;
}

int as_dispatch_fault(u64 handle, u64 vaddr, u64 err) {
    (void)handle;
    if (current_as == 0) {
        return 0;
    }
    return as_handle_fault(current_as, vaddr, err);
}

u64 as_translate(struct address_space *as, u64 vaddr) {
    volatile u64 *pt = resolve_pt(as->pml4, vaddr, 0, 0);
    if (pt == 0) {
        return 0;
    }
    u64 e = pt[pt_i(vaddr)];
    if (!(e & PTE_P)) {
        return 0;
    }
    return entry_phys(e) | (e & 0xFFF);
}

u32 as_frame_refs(u64 paddr) {
    return frame_refs[(u32)(paddr / PAGE_SIZE)];
}

/* ---------------------------------------------------------------------------
 * Bring-up self-test: demand paging, permission checks and COW
 * ------------------------------------------------------------------------- */

static struct address_space as_parent;
static struct address_space as_child;

/* Test VMA addresses (lower canonical half). */
static const u64 T_RW_A   = 0x0000000000100000ULL;   /* 2 demand RW pages  */
static const u64 T_RW_END = 0x0000000000102000ULL;
static const u64 T_RO_A   = 0x0000000000200000ULL;   /* 1 RO page          */
static const u64 T_GUARD  = 0x0000000000300000ULL;   /* Unmapped           */

static volatile u8 *va_ptr(u64 v) {
    return (volatile u8 *)v;
}

void as_selftest(void) {
    int ok = 1;
    u32 k;
    u64 RW_A = T_RW_A;
    u64 RW_END = T_RW_END;
    u64 RO_A = T_RO_A;
    u64 GUARD = T_GUARD;

    kprintf("\n[VMA] Virtual Memory Areas + Demand Paging + COW Online:\n");
    kprintf("      Per-space PML4, kernel slots shared, private lower half\n");
    kprintf("      Anonymous VMAs: 4 KiB demand-zero pages, PROT_READ/WRITE\n");
    kprintf("      COW: refcounted read-only shared leaves, private copy on write\n");
    kprintf("\n[VMA] Running Address Space Self-Tests...\n");

    as_init(&as_parent);
    if (!as_map_anon(&as_parent, RW_A, RW_END - RW_A, PROT_READ | PROT_WRITE)
        || !as_map_anon(&as_parent, RO_A, PAGE_SIZE, PROT_READ)) {
        kprintf("      [FAIL] VMA reservation failed\n");
        return;
    }

    as_activate(&as_parent);

    /* Nothing mapped before first touch. */
    if (as_translate(&as_parent, RW_A) != 0
        || as_translate(&as_parent, RW_A + PAGE_SIZE) != 0) {
        kprintf("      [FAIL] demand page present before first touch\n");
        ok = 0;
    }

    /* First writes demand-allocate and fill both pages. */
    for (k = 0; k < 2 * PAGE_SIZE; k++) {
        va_ptr(RW_A + k)[0] = (u8)(0x40 ^ k);
    }
    u64 p0 = as_translate(&as_parent, RW_A);
    u64 p1 = as_translate(&as_parent, RW_A + PAGE_SIZE);
    if ((p0 & PTE_P) == 0 || (p1 & PTE_P) == 0 || (p0 & ~0xFFFull) == (p1 & ~0xFFFull)) {
        kprintf("      [FAIL] demand pages not mapped/distinct: %p %p\n", p0, p1);
        ok = 0;
    }
    if (as_frame_refs(p0 & ~0xFFFull) != 1) {
        kprintf("      [FAIL] fresh demand page refcount != 1\n");
        ok = 0;
    }
    for (k = 0; k < 2 * PAGE_SIZE; k++) {
        if (va_ptr(RW_A + k)[0] != (u8)(0x40 ^ k)) {
            kprintf("      [FAIL] demand page data mismatch at %u\n", (u64)k);
            ok = 0;
            break;
        }
    }

    /* Read-only VMA: read demand works, write fault must be rejected. */
    u8 rd = va_ptr(RO_A)[10];
    (void)rd;
    if (as_dispatch_fault(1, GUARD, 0x2) != 0) {
        kprintf("      [FAIL] write to unmapped guard region accepted\n");
        ok = 0;
    }
    if (as_dispatch_fault(1, RO_A, 0x3) != 0) {
        kprintf("      [FAIL] write to read-only VMA accepted\n");
        ok = 0;
    }

    /* COW clone: parent and child share physical leaves, all read-only. */
    as_cow_clone(&as_child, &as_parent);
    /* Re-read the parent PTEs: cloning rewrites them read-only in place. */
    p0 = as_translate(&as_parent, RW_A);
    p1 = as_translate(&as_parent, RW_A + PAGE_SIZE);
    u64 c0 = as_translate(&as_child, RW_A);
    u64 c1 = as_translate(&as_child, RW_A + PAGE_SIZE);
    if ((c0 & ~0xFFFull) != (p0 & ~0xFFFull)
        || (c1 & ~0xFFFull) != (p1 & ~0xFFFull)) {
        kprintf("      [FAIL] COW child does not share parent frames\n");
        ok = 0;
    }
    if ((c0 & PTE_W) != 0 || (p0 & PTE_W) != 0) {
        kprintf("      [FAIL] COW shared frames not write-protected\n");
        ok = 0;
    }
    if (as_frame_refs(p0 & ~0xFFFull) != 2 || as_frame_refs(p1 & ~0xFFFull) != 2) {
        kprintf("      [FAIL] COW frame refcount != 2\n");
        ok = 0;
    }
    if (va_ptr(RW_A + 100)[0] != (u8)(0x40 ^ 100)) {
        kprintf("      [FAIL] child cannot read inherited COW data\n");
        ok = 0;
    }

    /* Child writes -> private copies; parent must stay untouched. */
    as_activate(&as_child);
    for (k = 0; k < 2 * PAGE_SIZE; k++) {
        va_ptr(RW_A + k)[0] = (u8)(0x90 ^ k);
    }
    for (k = 0; k < 2 * PAGE_SIZE; k++) {
        if (va_ptr(RW_A + k)[0] != (u8)(0x90 ^ k)) {
            kprintf("      [FAIL] child COW copy data mismatch at %u\n", (u64)k);
            ok = 0;
            break;
        }
    }
    u64 n0 = as_translate(&as_child, RW_A);
    if ((n0 & ~0xFFFull) == (p0 & ~0xFFFull)) {
        kprintf("      [FAIL] child still on parent frame after COW write\n");
        ok = 0;
    }

    as_activate(&as_parent);
    for (k = 0; k < 2 * PAGE_SIZE; k++) {
        if (va_ptr(RW_A + k)[0] != (u8)(0x40 ^ k)) {
            kprintf("      [FAIL] parent data corrupted by child COW write at %u\n", (u64)k);
            ok = 0;
            break;
        }
    }
    if (as_frame_refs(p0 & ~0xFFFull) != 1) {
        kprintf("      [FAIL] parent frame refcount not back to 1 after child COW\n");
        ok = 0;
    }

    as_activate(0);
    as_destroy(&as_child);
    as_destroy(&as_parent);

    if (ok) {
        kprintf("      [PASS] Demand paging: first-touch allocation, data, zero-on-alloc\n");
        kprintf("      [PASS] Permissions: read-only VMA rejects writes, guard unmapped\n");
        kprintf("      [PASS] COW clone: shared RO frames (ref 2), private copy diverges, parent intact\n");
    }
}

/* ---------------------------------------------------------------------------
 * Handle-based API for the process layer (kernel package cannot name struct
 * address_space across fakecc packages, so address spaces cross the boundary
 * as opaque u64 handles = kernel virtual addresses of slab-allocated structs).
 * ------------------------------------------------------------------------- */

extern void *kmalloc(u64 size);
extern void kfree(void *ptr);

/* Allocate and initialize a fresh address space. Returns 0 on OOM. */
u64 as_create(void) {
    struct address_space *as = (struct address_space *)kmalloc(8 + 8 + MAX_VMA * 24 + 8);
    if (as == 0) {
        return 0;
    }
    as_init(as);
    return (u64)as;
}

static struct address_space *as_of(u64 handle) {
    return (struct address_space *)handle;
}

/* Install an address space in CR3 (handle 0 = bootstrap kernel root). */
void as_activate_h(u64 handle) {
    as_activate(as_of(handle));
}

/* Reserve a demand-paged anonymous range in the target space. */
int as_map_anon_h(u64 handle, u64 start, u64 size, u32 prot) {
    return as_map_anon(as_of(handle), start, size, prot);
}

/* Record a VMA covering [va, va+memsz) and pre-populate the pages that carry
 * file content (frames allocated up front, bytes copied through the HHDM, so
 * RX code pages never need a kernel write through the user mapping). Pages
 * past filesz (e.g. .bss) stay unmapped and demand-zero fault in later. */
int as_map_preload(u64 handle, u64 va, u64 memsz, u32 prot, u64 src_kva, u64 filesz) {
    struct address_space *as = as_of(handle);
    if (!as_map_anon(as, va, memsz, prot)) {
        return 0;
    }
    struct vma *vma = find_vma(as, va);
    if (vma == 0) {
        return 0;
    }
    u64 off = 0;
    while (off < filesz && off < memsz) {
        u64 page_va = va + off;
        volatile u64 *pt = resolve_pt(as->pml4, page_va, 1, PTE_U);
        if (pt == 0) {
            return 0;
        }
        u64 frame = alloc_zero_frame();
        if (frame == 0) {
            return 0;
        }
        volatile u8 *dst = (volatile u8 *)(HHDM_BASE + frame);
        volatile u8 *src = (volatile u8 *)(src_kva + off);
        u32 k;
        for (k = 0; k < 4096; k++) {
            if (off + k < filesz) {
                dst[k] = src[k];
            } else {
                dst[k] = 0;
            }
        }
        frame_refs[(u32)(frame / PAGE_SIZE)] = 1;
        pt[pt_i(page_va)] = frame | leaf_perms(vma);
        off = off + PAGE_SIZE;
    }
    return 1;
}

/* Copy-on-write fork of an address space. Returns the child handle (0=fail). */
u64 as_fork_h(u64 parent_handle) {
    u64 child = as_create();
    if (child == 0) {
        return 0;
    }
    as_cow_clone(as_of(child), as_of(parent_handle));
    return child;
}

void as_destroy_h(u64 handle) {
    if (handle == 0) {
        return;
    }
    struct address_space *as = as_of(handle);
    as_destroy(as);
    kfree(as);
}

/* True when [va, va+len) is fully covered by one VMA of the space (used by
 * syscall argument validation before the kernel dereferences user memory). */
int as_user_range_ok(u64 handle, u64 va, u64 len) {
    struct address_space *as = as_of(handle);
    if (as == 0 || len == 0) {
        return 0;
    }
    struct vma *vma = find_vma(as, va);
    if (vma == 0) {
        return 0;
    }
    return (va + len) <= vma->end;
}

u64 as_pml4_of(u64 handle) {
    struct address_space *as = as_of(handle);
    return as ? as->pml4 : vmm_kernel_root();
}
