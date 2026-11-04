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
 * touch raises #PF and as_handle_fault() demand-pages it. MAP_PRIVATE anon
 * read faults share a global zero frame (mapped RO); the first write
 * allocates a private page (or COWs off the shared zero). MAP_SHARED anon
 * never uses the zero page: first touch allocates a real frame kept writable
 * across fork (shm-style). VMA_FILE ranges demand-read from a ramfs / Ext2
 * inode on first touch; MAP_SHARED file pages keep W across fork (no COW
 * demotion) and write dirty pages back on munmap/exit. as_cow_clone()
 * shallow-copies an address space, marking private leaves read-only and
 * bumping a per-frame refcount; the first private write triggers a copy.
 * ------------------------------------------------------------------------- */

static const u64 HHDM_BASE = 0xFFFF800000000000ULL;
static const u64 PAGE_SIZE = 4096;

enum {
    PTE_P      = 1,
    PTE_W      = 2,
    PTE_U      = 4,
    PTE_D      = 0x40,           /* Dirty */
    PTE_NX     = 1u << 31,
    PT_ENTRIES = 512,
    MAX_VMA    = 64,
    MAX_FRAMES_AS = 1048576,     /* 4 GiB / 4 KiB, matches the PMM ceiling */

    VMA_FREE   = 0,
    VMA_ANON   = 1,
    VMA_FILE   = 2,

    FS_KIND_RAMFS = 0,
    FS_KIND_EXT2  = 1,

    PROT_READ  = 1,
    PROT_WRITE = 2,
    PROT_EXEC  = 4
};

/* Fixed user-space layout: the ELF image loads near 0x400000, the brk heap
 * starts above the image, anonymous mmap allocations grow upward in a window
 * above the heap, the 8 MiB demand stack lives at the 2 GiB ceiling, and
 * MAP_FIXED callers (ASAN shadow) reserve their own ranges above; none of
 * the fixed ranges overlap. */
static const u64 HEAP_BASE = 0x02000000ULL;
static const u64 HEAP_SIZE = 0x04000000ULL;        /* 64 MiB reserved: in-OS fakecc (ext2.c needs ~19MiB+) */
static const u64 MMAP_BASE = 0x10000000ULL;        /* 256 MiB */
static const u64 MMAP_LIMIT = 0x40000000ULL;       /* 1 GiB ceiling */

struct vma {
    u64 start;
    u64 end;
    u32 prot;
    u32 kind;
    u32 shared;                /* MAP_SHARED: keep W across fork */
    u32 fs_kind;               /* FS_KIND_* when kind == VMA_FILE */
    u32 ino;                   /* backing inode */
    u64 file_off;              /* file offset of vma->start */
};

struct address_space {
    u64 pml4;
    struct vma vmas[MAX_VMA];
    u32 nvma;
    u64 brk_base;              /* 0 until the first brk reserves the heap VMA */
    u64 brk_cur;
    u64 mmap_next;             /* next free hint in the anonymous mmap zone  */
    u32 refs;                  /* CLONE_VM sharers; destroy at last release   */
};

/* Per-frame share count for COW. 1 = single owner (plain writable page),
 * >=2 = mapped read-only into that many address spaces / PTEs. */
static u16 frame_refs[MAX_FRAMES_AS];

/* Shared demand-zero page: read faults map this RO frame; write faults
 * always allocate a private copy (never lift W on the shared zero). */
static u64 zero_frame;
static u32 zero_faults;
static u32 private_faults;

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
    as->brk_base = 0;
    as->brk_cur = 0;
    as->mmap_next = 0;
    as->refs = 1;
    for (i = 0; i < MAX_VMA; i++) {
        as->vmas[i].kind = VMA_FREE;
        as->vmas[i].start = 0;
        as->vmas[i].end = 0;
        as->vmas[i].prot = 0;
        as->vmas[i].shared = 0;
        as->vmas[i].fs_kind = 0;
        as->vmas[i].ino = 0;
        as->vmas[i].file_off = 0;
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

/* Reserve an anonymous demand-paged range (page aligned). shared!=0 marks
 * MAP_SHARED (fork keeps W; no zero-page shortcut). Returns 1 on success;
 * overlapping reservations are rejected. */
int as_map_anon(struct address_space *as, u64 start, u64 size, u32 prot,
                u32 shared) {
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
    as->vmas[as->nvma].shared = shared != 0 ? 1 : 0;
    as->vmas[as->nvma].fs_kind = 0;
    as->vmas[as->nvma].ino = 0;
    as->vmas[as->nvma].file_off = 0;
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

static void unmap_present_pages(struct address_space *as, u64 va, u64 len,
                                 struct vma *vma);

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
/* Provided by kernel/sysproc.c: fill one page from a file inode. */
extern u32 as_file_page_fill(u32 fs_kind, u32 ino, u64 file_off, u64 page_kva);
extern u32 as_file_page_writeback(u32 fs_kind, u32 ino, u64 file_off, u64 page_kva);

static u32 ensure_zero_frame(void) {
    if (zero_frame != 0) {
        return 1;
    }
    zero_frame = alloc_zero_frame();
    if (zero_frame == 0) {
        return 0;
    }
    /* Pin so unmap never frees the shared zero page. */
    frame_refs[(u32)(zero_frame / PAGE_SIZE)] = 1;
    return 1;
}

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
        if ((err & 0x2) && !(vma->prot & PROT_WRITE)) {
            return 0;                 /* Write to a read-only VMA */
        }
        if (vma->kind == VMA_FILE) {
            u64 frame = alloc_zero_frame();
            u64 foff;
            if (frame == 0) {
                return 0;
            }
            foff = vma->file_off + (page_va - vma->start);
            if (!as_file_page_fill(vma->fs_kind, vma->ino, foff,
                                   HHDM_BASE + frame)) {
                pmm_free_page(frame);
                return 0;
            }
            frame_refs[(u32)(frame / PAGE_SIZE)] = 1;
            pt[i] = frame | leaf_perms(vma);
            private_faults++;
            invlpg_page(page_va);
            return 1;
        }
        /* Demand-zero anonymous page. */
        if (vma->shared != 0) {
            /* MAP_SHARED anon / shm: always a real frame so fork sharers see
             * the same physical page (never the global zero shortcut). */
            u64 frame = alloc_zero_frame();
            if (frame == 0) {
                return 0;
            }
            if ((err & 0x2) && !(vma->prot & PROT_WRITE)) {
                pmm_free_page(frame);
                return 0;
            }
            frame_refs[(u32)(frame / PAGE_SIZE)] = 1;
            pt[i] = frame | leaf_perms(vma);
            private_faults++;
            invlpg_page(page_va);
            return 1;
        }
        if (err & 0x2) {
            /* Write fault: private zero page. */
            u64 frame = alloc_zero_frame();
            if (frame == 0) {
                return 0;
            }
            frame_refs[(u32)(frame / PAGE_SIZE)] = 1;
            pt[i] = frame | leaf_perms(vma);
            private_faults++;
            invlpg_page(page_va);
            return 1;
        }
        /* Read fault: map shared zero RO (even for RW VMAs). */
        if (!ensure_zero_frame()) {
            return 0;
        }
        {
            u64 e = PTE_P | PTE_U;   /* never PTE_W on shared zero */
            if (!(vma->prot & PROT_EXEC)) {
                e |= ((u64)1 << 63);
            }
            frame_refs[(u32)(zero_frame / PAGE_SIZE)]++;
            pt[i] = zero_frame | e;
            zero_faults++;
            invlpg_page(page_va);
            return 1;
        }
    }

    /* Present page: the only resolvable protection fault is a write to a
     * write-protected COW page. err bit2 (user) is ignored: bring-up tests
     * touch the user half from ring 0 before ring 3 exists. */
    if ((err & 0x1) && (err & 0x2) && !(pte & PTE_W)) {
        if (!(vma->prot & PROT_WRITE)) {
            return 0;
        }
        u64 old_phys = entry_phys(pte);
        u32 pfn = (u32)(old_phys / PAGE_SIZE);
        /* MAP_SHARED (anon or file): never COW; just restore W so all
         * sharers observe the same store. Zero page still needs a real frame. */
        if (vma->shared != 0 && old_phys != zero_frame) {
            pt[i] = pte | PTE_W;
            invlpg_page(page_va);
            return 1;
        }
        /* Shared zero must always COW; never promote it writable. */
        if (old_phys == zero_frame || frame_refs[pfn] > 1) {
            u64 fresh = alloc_zero_frame();
            u32 k;
            volatile u8 *src;
            volatile u8 *dst;
            if (fresh == 0) {
                return 0;
            }
            src = (volatile u8 *)(HHDM_BASE + old_phys);
            dst = (volatile u8 *)(HHDM_BASE + fresh);
            for (k = 0; k < 4096; k++) {
                dst[k] = src[k];
            }
            frame_refs[(u32)(fresh / PAGE_SIZE)] = 1;
            if (frame_refs[pfn] > 0) {
                frame_refs[pfn]--;
            }
            pt[i] = fresh | leaf_perms(vma);
            private_faults++;
            invlpg_page(page_va);
            return 1;
        }
        /* Last private owner: just lift write protection. */
        frame_refs[pfn] = 1;
        pt[i] = pte | PTE_W;
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
static u32 vma_is_shared_at(struct address_space *as, u64 va) {
    struct vma *v = find_vma(as, va);
    return (v != 0 && v->shared != 0) ? 1 : 0;
}

static void cow_copy_pt(struct address_space *as, u64 va_base,
                        volatile u64 *src_pt, volatile u64 *dst_pt) {
    u32 i;
    for (i = 0; i < PT_ENTRIES; i++) {
        u64 e = src_pt[i];
        u64 page_va = va_base + ((u64)i << 12);
        if (!(e & PTE_P)) {
            dst_pt[i] = e;
            continue;
        }
        u32 pfn = (u32)(entry_phys(e) / PAGE_SIZE);
        frame_refs[pfn]++;
        if ((e & PTE_W) && !vma_is_shared_at(as, page_va)) {
            u64 ro = (e & ~(u64)PTE_W) | ((u64)1 << 63);
            src_pt[i] = ro;
            dst_pt[i] = ro;
        } else {
            dst_pt[i] = e;            /* Already RO, or MAP_SHARED keep W */
        }
    }
}

static void cow_walk_pd(struct address_space *as, u64 va_base,
                        volatile u64 *src_pd, volatile u64 *dst_pd) {
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
        dst_pd[i] = f | PTE_P | PTE_W | PTE_U;
        cow_copy_pt(as, va_base + ((u64)i << 21),
                    table(entry_phys(e)), table(f));
    }
}

static void cow_walk_pdpt(struct address_space *as, u64 va_base,
                          volatile u64 *src_pdpt, volatile u64 *dst_pdpt) {
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
        dst_pdpt[i] = f | PTE_P | PTE_W | PTE_U;
        cow_walk_pd(as, va_base + ((u64)i << 30),
                    table(entry_phys(e)), table(f));
    }
}

void as_cow_clone(struct address_space *dst, struct address_space *src) {
    u32 i;
    as_init(dst);

    /* Duplicate the VMA reservations. */
    dst->nvma = src->nvma;
    dst->brk_base = src->brk_base;
    dst->brk_cur = src->brk_cur;
    dst->mmap_next = src->mmap_next;
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
        cow_walk_pdpt(src, ((u64)i << 39), table(entry_phys(e)), table(f));
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
    volatile u64 *p;
    /* Flush MAP_SHARED dirty file pages before page tables go away. */
    for (i = 0; i < as->nvma; i++) {
        struct vma *v = &as->vmas[i];
        if (v->kind == VMA_FILE && v->shared != 0) {
            unmap_present_pages(as, v->start, v->end - v->start, v);
        }
    }
    p = table(as->pml4);
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
    kprintf("      Anonymous VMAs: demand-zero (shared RO zero page on read)\n");
    kprintf("      COW: refcounted read-only shared leaves, private copy on write\n");
    kprintf("\n[VMA] Running Address Space Self-Tests...\n");

    as_init(&as_parent);
    if (!as_map_anon(&as_parent, RW_A, RW_END - RW_A, PROT_READ | PROT_WRITE, 0)
        || !as_map_anon(&as_parent, RO_A, PAGE_SIZE, PROT_READ, 0)) {
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

    /* Read-only VMA: read demand maps shared zero; write fault rejected. */
    u8 rd = va_ptr(RO_A)[10];
    (void)rd;
    {
        u64 ro_pte = as_translate(&as_parent, RO_A);
        if (zero_frame == 0 || (ro_pte & ~0xFFFull) != zero_frame) {
            kprintf("      [FAIL] RO demand read did not map shared zero page\n");
            ok = 0;
        }
        if ((ro_pte & PTE_W) != 0) {
            kprintf("      [FAIL] shared zero mapped writable\n");
            ok = 0;
        }
    }
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

    /* MAP_SHARED anonymous: fork keeps W; child store visible to parent. */
    {
        static const u64 SHM_A = 0x0000000000400000ULL;
        u64 sp, sc;
        as_init(&as_parent);
        if (!as_map_anon(&as_parent, SHM_A, PAGE_SIZE, PROT_READ | PROT_WRITE, 1)) {
            kprintf("      [FAIL] MAP_SHARED anon reservation failed\n");
            ok = 0;
        } else {
            as_activate(&as_parent);
            va_ptr(SHM_A)[0] = 0xA5;
            va_ptr(SHM_A)[1] = 0x5A;
            sp = as_translate(&as_parent, SHM_A);
            if ((sp & PTE_P) == 0 || (sp & PTE_W) == 0) {
                kprintf("      [FAIL] MAP_SHARED anon not present/writable after touch\n");
                ok = 0;
            }
            as_cow_clone(&as_child, &as_parent);
            sp = as_translate(&as_parent, SHM_A);
            sc = as_translate(&as_child, SHM_A);
            if ((sc & ~0xFFFull) != (sp & ~0xFFFull)) {
                kprintf("      [FAIL] MAP_SHARED anon child frame mismatch\n");
                ok = 0;
            }
            if ((sc & PTE_W) == 0 || (sp & PTE_W) == 0) {
                kprintf("      [FAIL] MAP_SHARED anon lost W across fork\n");
                ok = 0;
            }
            if (as_frame_refs(sp & ~0xFFFull) != 2) {
                kprintf("      [FAIL] MAP_SHARED anon refcount != 2\n");
                ok = 0;
            }
            as_activate(&as_child);
            va_ptr(SHM_A)[0] = 0x3C;
            as_activate(&as_parent);
            if (va_ptr(SHM_A)[0] != 0x3C || va_ptr(SHM_A)[1] != 0x5A) {
                kprintf("      [FAIL] MAP_SHARED anon write not visible to parent\n");
                ok = 0;
            }
            as_activate(0);
            as_destroy(&as_child);
            as_destroy(&as_parent);
        }
    }

    if (ok) {
        kprintf("      [PASS] Demand paging: first-touch allocation, data, zero-on-alloc\n");
        kprintf("      [PASS] Permissions: read-only VMA rejects writes, guard unmapped\n");
        kprintf("      [PASS] COW clone: shared RO frames (ref 2), private copy diverges, parent intact\n");
        kprintf("      [PASS] MAP_SHARED anon: keep W across fork, child store visible to parent\n");
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
    /* pml4 + vmas[MAX_VMA] (48B each) + nvma/pad + brk_base/cur + mmap_next + refs */
    struct address_space *as = (struct address_space *)kmalloc(
        8 + MAX_VMA * 48 + 8 + 8 + 8 + 8 + 8);
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
    return as_map_anon(as_of(handle), start, size, prot, 0);
}

/* brk(): lazily reserve the fixed 64 MiB heap VMA on first use and move the
 * program break. A newbrk of 0 queries the current break. The break only
 * moves; touched pages demand-allocate through the ordinary #PF path. */
u64 as_brk_h(u64 handle, u64 newbrk) {
    struct address_space *as = as_of(handle);
    if (as == 0) {
        return (u64)(-1);
    }
    if (as->brk_base == 0) {
        if (!as_map_anon(as, HEAP_BASE, HEAP_SIZE, PROT_READ | PROT_WRITE, 0)) {
            return (u64)(-1);
        }
        as->brk_base = HEAP_BASE;
        as->brk_cur = HEAP_BASE;
    }
    if (newbrk == 0) {
        return as->brk_cur;
    }
    if (newbrk < as->brk_base || newbrk > as->brk_base + HEAP_SIZE) {
        return (u64)(-1);
    }
    as->brk_cur = newbrk;
    return newbrk;
}

/* Anonymous mmap: page-aligned reservation grown upward from MMAP_BASE.
 * shared!=0 => MAP_SHARED (fork keeps W; shm-style). Returns start VA or 0. */
u64 as_mmap_anon_h(u64 handle, u64 len, u32 prot, u32 shared) {
    struct address_space *as = as_of(handle);
    u64 hint;
    if (as == 0 || len == 0) {
        return 0;
    }
    len = (len + 0xFFF) & ~0xFFFULL;
    if (len == 0) {
        return 0;                    /* rounding overflowed the address space */
    }
    hint = as->mmap_next != 0 ? as->mmap_next : MMAP_BASE;
    if (hint + len < hint || hint + len > MMAP_LIMIT) {
        return 0;
    }
    if (!as_map_anon(as, hint, len, prot, shared)) {
        return 0;
    }
    as->mmap_next = hint + len;
    return hint;
}

/* MAP_FIXED anonymous reservation at a caller-chosen page-aligned address.
 * Used by the fakecc runtime to reserve its 16 TiB ASAN shadow window at
 * 0x100000000000: the range is huge but costs nothing until a shadow byte
 * is touched, at which point ordinary demand paging backs the single page.
 * Overlapping an existing VMA is rejected (Linux would unmap it; no in-tree
 * caller needs that). */
u64 as_mmap_fixed_h(u64 handle, u64 addr, u64 len, u32 prot) {
    struct address_space *as = as_of(handle);
    u64 end;
    if (as == 0 || len == 0 || (addr & 0xFFF) != 0) {
        return 0;
    }
    len = (len + 0xFFF) & ~0xFFFULL;
    if (len == 0) {
        return 0;
    }
    end = addr + len;
    if (end <= addr || end > 0x0000800000000000ULL) {
        return 0;                    /* wrap or non-canonical user address */
    }
    if (!as_map_anon(as, addr, len, prot, 0)) {
        return 0;
    }
    return addr;
}

/* File-backed mmap reservation (VMA_FILE). Pages fault in via as_file_page_fill.
 * shared!=0 => MAP_SHARED (fork keeps W). addr==0 picks the next mmap hint;
 * addr!=0 is MAP_FIXED (page-aligned canonical user VA; overlaps rejected;
 * does not advance mmap_next). Returns start VA or 0. */
u64 as_mmap_file_h(u64 handle, u64 addr, u64 len, u32 prot, u32 shared,
                   u32 fs_kind, u32 ino, u64 file_off) {
    struct address_space *as = as_of(handle);
    u64 hint;
    u64 end;
    u32 fixed;
    if (as == 0 || len == 0 || (len & 0xFFF) != 0 || ino == 0) {
        return 0;
    }
    if ((file_off & 0xFFF) != 0) {
        return 0;
    }
    fixed = addr != 0 ? 1 : 0;
    if (fixed) {
        if ((addr & 0xFFF) != 0) {
            return 0;
        }
        end = addr + len;
        if (end <= addr || end > 0x0000800000000000ULL) {
            return 0;
        }
        hint = addr;
    } else {
        hint = as->mmap_next != 0 ? as->mmap_next : MMAP_BASE;
        if (hint + len < hint || hint + len > MMAP_LIMIT) {
            return 0;
        }
        end = hint + len;
    }
    if (as->nvma >= MAX_VMA) {
        return 0;
    }
    {
        u32 i;
        for (i = 0; i < as->nvma; i++) {
            if (hint < as->vmas[i].end && as->vmas[i].start < end) {
                return 0;
            }
        }
    }
    as->vmas[as->nvma].start = hint;
    as->vmas[as->nvma].end = end;
    as->vmas[as->nvma].prot = prot | PROT_READ;
    as->vmas[as->nvma].kind = VMA_FILE;
    as->vmas[as->nvma].shared = shared != 0 ? 1 : 0;
    as->vmas[as->nvma].fs_kind = fs_kind;
    as->vmas[as->nvma].ino = ino;
    as->vmas[as->nvma].file_off = file_off;
    as->nvma++;
    if (!fixed) {
        as->mmap_next = end;
    }
    return hint;
}

/* Tear down present leaf PTEs over [va, va+len) and release frame ownership.
 * Page-table frames themselves stay behind (empty tables are harmless and
 * reused by later demand faults). Shared COW frames drop one reference and
 * are returned to the buddy only when the last owner goes away. */
static void unmap_present_pages(struct address_space *as, u64 va, u64 len,
                                 struct vma *vma) {
    u64 off = 0;
    while (off < len) {
        u64 page_va = (va + off) & ~0xFFFULL;
        volatile u64 *pt = resolve_pt(as->pml4, page_va, 0, PTE_U);
        if (pt != 0) {
            u64 e = pt[pt_i(page_va)];
            if (e & PTE_P) {
                u64 frame = entry_phys(e);
                /* MAP_SHARED file: write dirty (or any W-mapped) pages back. */
                if (vma != 0 && vma->kind == VMA_FILE && vma->shared != 0
                    && (e & PTE_W) != 0
                    && (e & PTE_D) != 0) {
                    u64 foff = vma->file_off + (page_va - vma->start);
                    (void)as_file_page_writeback(vma->fs_kind, vma->ino, foff,
                                                 HHDM_BASE + frame);
                }
                pt[pt_i(page_va)] = 0;
                invlpg_page(page_va);
                if (frame < (u64)MAX_FRAMES_AS * PAGE_SIZE
                    && frame_refs[(u32)(frame / PAGE_SIZE)] > 0) {
                    frame_refs[(u32)(frame / PAGE_SIZE)]--;
                    if (frame_refs[(u32)(frame / PAGE_SIZE)] == 0) {
                        if (frame != zero_frame) {
                            pmm_free_page(frame);
                        } else {
                            frame_refs[(u32)(frame / PAGE_SIZE)] = 1;
                        }
                    }
                }
            }
        }
        off += PAGE_SIZE;
    }
}

/* Apply new VMA prot bits to present leaf PTEs over [va, va+len).
 * Dropping WRITE clears PTE_W. Granting WRITE only lifts W when the frame is
 * exclusive (or MAP_SHARED and not the shared zero); COW-shared private
 * pages stay RO until the next write fault. NX tracks PROT_EXEC. */
static void apply_prot_present(struct address_space *as, u64 va, u64 len,
                               u32 prot, u32 shared) {
    u64 off = 0;
    u64 nx = ((u64)1 << 63);
    while (off < len) {
        u64 page_va = (va + off) & ~0xFFFULL;
        volatile u64 *pt = resolve_pt(as->pml4, page_va, 0, PTE_U);
        if (pt != 0) {
            u64 e = pt[pt_i(page_va)];
            if (e & PTE_P) {
                u64 frame = entry_phys(e);
                u32 pfn = (u32)(frame / PAGE_SIZE);
                u64 ne = e & ~((u64)PTE_W);
                ne &= ~nx;
                if ((prot & PROT_WRITE) != 0 && frame != zero_frame) {
                    if (shared != 0 || (pfn < (u32)MAX_FRAMES_AS
                                        && frame_refs[pfn] <= 1)) {
                        ne |= PTE_W;
                    }
                }
                if ((prot & PROT_EXEC) == 0) {
                    ne |= nx;
                }
                pt[pt_i(page_va)] = ne;
                invlpg_page(page_va);
            }
        }
        off += PAGE_SIZE;
    }
}

/* mprotect: change prot on a page-aligned range fully contained in one VMA,
 * splitting the reservation when the range is a proper subinterval (same
 * shape as munmap). Present PTEs are updated immediately. Returns 1/0. */
int as_mprotect_h(u64 handle, u64 uva, u64 len, u32 prot) {
    struct address_space *as = as_of(handle);
    u64 start;
    u64 end;
    u32 i;
    u32 idx = (u32)-1;
    struct vma *v;
    u32 new_prot;

    if (as == 0 || len == 0 || (uva & 0xFFF) != 0 || (len & 0xFFF) != 0) {
        return 0;
    }
    start = uva;
    end = uva + len;
    if (end <= start) {
        return 0;
    }
    for (i = 0; i < as->nvma; i++) {
        if (start >= as->vmas[i].start && end <= as->vmas[i].end) {
            idx = i;
            break;
        }
    }
    if (idx == (u32)-1) {
        return 0;
    }
    v = &as->vmas[idx];
    new_prot = prot | PROT_READ;
    apply_prot_present(as, start, end - start, new_prot, v->shared);

    if (start == v->start && end == v->end) {
        v->prot = new_prot;
        return 1;
    }
    if (start == v->start) {
        /* Prefix: shrink old VMA from the left, insert new prot VMA ahead. */
        struct vma head;
        if (as->nvma >= MAX_VMA) {
            return 0;
        }
        head.start = start;
        head.end = end;
        head.prot = new_prot;
        head.kind = v->kind;
        head.shared = v->shared;
        head.fs_kind = v->fs_kind;
        head.ino = v->ino;
        head.file_off = v->file_off;
        if (v->kind == VMA_FILE) {
            v->file_off += (end - start);
        }
        v->start = end;
        /* Shift right to make room at idx. */
        for (i = as->nvma; i > idx; i--) {
            as->vmas[i] = as->vmas[i - 1];
        }
        as->vmas[idx] = head;
        as->nvma++;
        return 1;
    }
    if (end == v->end) {
        /* Suffix: shrink old end, append new prot VMA. */
        struct vma tail;
        if (as->nvma >= MAX_VMA) {
            return 0;
        }
        v->end = start;
        tail.start = start;
        tail.end = end;
        tail.prot = new_prot;
        tail.kind = v->kind;
        tail.shared = v->shared;
        tail.fs_kind = v->fs_kind;
        tail.ino = v->ino;
        if (v->kind == VMA_FILE) {
            tail.file_off = v->file_off + (start - v->start);
        } else {
            tail.file_off = 0;
        }
        as->vmas[as->nvma] = tail;
        as->nvma++;
        return 1;
    }
    /* Hole in the middle: keep left, change middle via new VMA, append right. */
    {
        struct vma mid;
        struct vma right;
        u64 old_end = v->end;
        if (as->nvma + 1 >= MAX_VMA) {
            return 0;
        }
        mid.start = start;
        mid.end = end;
        mid.prot = new_prot;
        mid.kind = v->kind;
        mid.shared = v->shared;
        mid.fs_kind = v->fs_kind;
        mid.ino = v->ino;
        if (v->kind == VMA_FILE) {
            mid.file_off = v->file_off + (start - v->start);
        } else {
            mid.file_off = 0;
        }
        right.start = end;
        right.end = old_end;
        right.prot = v->prot;
        right.kind = v->kind;
        right.shared = v->shared;
        right.fs_kind = v->fs_kind;
        right.ino = v->ino;
        if (v->kind == VMA_FILE) {
            right.file_off = v->file_off + (end - v->start);
        } else {
            right.file_off = 0;
        }
        v->end = start;
        as->vmas[as->nvma] = mid;
        as->nvma++;
        as->vmas[as->nvma] = right;
        as->nvma++;
        return 1;
    }
}

/* munmap: remove a page-aligned range fully contained in one VMA, splitting
 * the reservation when unmap hits its middle. Pages are unmapped up front. */
int as_munmap_h(u64 handle, u64 uva, u64 len) {
    struct address_space *as = as_of(handle);
    u64 start;
    u64 end;
    u32 i;
    u32 idx = (u32)-1;
    struct vma *v;

    if (as == 0 || len == 0 || (uva & 0xFFF) != 0 || (len & 0xFFF) != 0) {
        return 0;
    }
    start = uva;
    end = uva + len;
    if (end <= start) {
        return 0;                       /* wrap */
    }
    for (i = 0; i < as->nvma; i++) {
        if (start >= as->vmas[i].start && end <= as->vmas[i].end) {
            idx = i;
            break;
        }
    }
    if (idx == (u32)-1) {
        return 0;                      /* not fully covered by one VMA */
    }
    v = &as->vmas[idx];
    unmap_present_pages(as, start, end - start, v);

    if (start == v->start && end == v->end) {
        /* Whole VMA: drop the reservation, keeping the array compact. */
        for (i = idx; i + 1 < as->nvma; i++) {
            as->vmas[i] = as->vmas[i + 1];
        }
        as->nvma--;
    } else if (start == v->start) {
        if (v->kind == VMA_FILE) {
            v->file_off += (end - start);
        }
        v->start = end;
    } else if (end == v->end) {
        v->end = start;
    } else {
        /* Hole in the middle: keep [start, uva), append [end, oldend). */
        struct vma tail;
        u64 old_end = v->end;
        u32 prot = v->prot;
        if (as->nvma >= MAX_VMA) {
            return 0;                  /* cannot represent the split */
        }
        v->end = start;
        tail.start = end;
        tail.end = old_end;
        tail.prot = prot;
        tail.kind = v->kind;
        tail.shared = v->shared;
        tail.fs_kind = v->fs_kind;
        tail.ino = v->ino;
        if (v->kind == VMA_FILE) {
            tail.file_off = v->file_off + (end - v->start);
        } else {
            tail.file_off = 0;
        }
        as->vmas[as->nvma] = tail;
        as->nvma++;
    }
    return 1;
}

/* Force-allocate the frame backing uva's page in a (not necessarily active)
 * address space, honoring its VMA permissions. Used to seed a freshly built
 * user stack during process creation / execve. Returns the physical frame or
 * 0 when the VA is outside every VMA or tables cannot be grown. */
u64 as_force_page_h(u64 handle, u64 uva) {
    struct address_space *as = as_of(handle);
    u64 page_va = uva & ~0xFFFULL;
    struct vma *vma = find_vma(as, page_va);
    volatile u64 *pt;
    u64 pte;
    if (as == 0 || vma == 0) {
        return 0;
    }
    pt = resolve_pt(as->pml4, uva, 1, PTE_U);
    if (pt == 0) {
        return 0;
    }
    pte = pt[pt_i(uva)];
    if (pte & PTE_P) {
        return entry_phys(pte);
    }
    {
        u64 frame = alloc_zero_frame();
        if (frame == 0) {
            return 0;
        }
        frame_refs[(u32)(frame / PAGE_SIZE)] = 1;
        pt[pt_i(uva)] = frame | leaf_perms(vma);
        return frame;
    }
}

/* Copy kernel bytes into user VA of a possibly inactive space, force-paging
 * each touched frame and writing through the HHDM. Returns 1 on success. */
int as_copy_in_h(u64 handle, u64 uva, const char *src, u64 len) {
    u64 off = 0;
    if (as_of(handle) == 0) {
        return 0;
    }
    while (off < len) {
        u64 frame = as_force_page_h(handle, uva + off);
        u64 poff = (uva + off) & 0xFFF;
        u64 chunk = PAGE_SIZE - poff;
        volatile u8 *dst8;
        u64 k;
        if (frame == 0) {
            return 0;
        }
        if (chunk > len - off) {
            chunk = len - off;
        }
        dst8 = (volatile u8 *)(HHDM_BASE + frame);
        for (k = 0; k < chunk; k++) {
            dst8[poff + k] = (u8)src[off + k];
        }
        off += chunk;
    }
    return 1;
}

/* Record a VMA covering [va, va+memsz) and pre-populate the pages that carry
 * file content (frames allocated up front, bytes copied through the HHDM, so
 * RX code pages never need a kernel write through the user mapping). Pages
 * past filesz (e.g. .bss) stay unmapped and demand-zero fault in later. */
int as_map_preload(u64 handle, u64 va, u64 memsz, u32 prot, u64 src_kva, u64 filesz) {
    struct address_space *as = as_of(handle);
    if (!as_map_anon(as, va, memsz, prot, 0)) {
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

/* CLONE_VM: bump share count so exit of one thread does not free the AS. */
void as_retain_h(u64 handle) {
    struct address_space *as;
    if (handle == 0) {
        return;
    }
    as = as_of(handle);
    if (as->refs < 0xFFFFFFFFu) {
        as->refs++;
    }
}

/* Drop one CLONE_VM reference; destroy when the last sharer exits. */
void as_release_h(u64 handle) {
    struct address_space *as;
    if (handle == 0) {
        return;
    }
    as = as_of(handle);
    if (as->refs > 1) {
        as->refs--;
        return;
    }
    as_destroy_h(handle);
}

/* True when [va, va+len) is fully covered by one VMA of the space (used by
 * syscall argument validation before the kernel dereferences user memory).
 * Rejects va+len wrap-around so a huge crafted len cannot pass the check and
 * reach back into kernel-mapped addresses. */
int as_user_range_ok(u64 handle, u64 va, u64 len) {
    struct address_space *as = as_of(handle);
    u64 end;
    struct vma *vma;
    if (as == 0 || len == 0) {
        return 0;
    }
    end = va + len;
    if (end < va) {
        return 0;                    /* address wrap */
    }
    vma = find_vma(as, va);
    if (vma == 0) {
        return 0;
    }
    return end <= vma->end;
}

u64 as_pml4_of(u64 handle) {
    struct address_space *as = as_of(handle);
    return as ? as->pml4 : vmm_kernel_root();
}
