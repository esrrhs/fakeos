package mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern u64 pmm_alloc_page(void);
extern void tlb_flush(void);
extern void invlpg_page(u64 vaddr);
extern u64 msr_read(u32 msr);
extern void msr_write(u32 msr, u64 value);
extern u64 cr3_read(void);
extern void cr3_write(u64 value);

/* Boot PML4 physical frame. The symbol's low VMA numerically equals its
 * physical address; we only need the constant (never dereferenced here). */
extern u8 boot_pml4[];

/* ---------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

static const u64 HHDM_BASE  = 0xFFFF800000000000ULL;
static const u64 PAGE_SIZE  = 4096;
static const u64 ADDR_MASK  = 0x0000FFFFFFFFFFFFULL; /* 48-bit canonical mask */

/* Page table entry permission / attribute bits (Intel SDM Vol.3A 4.5). */
enum {
    VMM_PRESENT  = 1u << 0,
    VMM_WRITABLE = 1u << 1,
    VMM_USER     = 1u << 2,
    VMM_PWT      = 1u << 3,   /* Page-Level Write-Through          */
    VMM_PCD      = 1u << 4,   /* Page-Level Cache-Disable          */
    VMM_ACCESSED = 1u << 5,
    VMM_HUGE     = 1u << 7,   /* PS: 2 MiB page at PD / 1 GiB PDPT */
    VMM_NX       = (u32)1u << 31  /* Stored in bit 63 of the entry  */
};

enum {
    IA32_EFER    = 0xC0000080,
    EFER_NXE     = 1u << 11,
    ENTRIES      = 512
};

/* ---------------------------------------------------------------------------
 * Low-level table access (every table frame is reached through HHDM)
 * ------------------------------------------------------------------------- */

static volatile u64 *table_at(u64 phys) {
    return (volatile u64 *)(HHDM_BASE + (phys & ADDR_MASK));
}

static u64 entry_addr(u64 entry) {
    return entry & 0x000FFFFFFFFFF000ULL;
}

/* Allocate and zero a fresh page-table frame. */
static u64 alloc_table(void) {
    u64 frame = pmm_alloc_page();
    if (frame == 0) {
        return 0;
    }
    volatile u64 *t = table_at(frame);
    int i;
    for (i = 0; i < ENTRIES; i++) {
        t[i] = 0;
    }
    return frame;
}

/* Permission bits every intermediate directory needs so a leaf mapping is
 * reachable: Present|Writable, plus User for user-owned mappings. */
static u64 dir_flags(u64 leaf_flags) {
    u64 f = VMM_PRESENT | VMM_WRITABLE;
    if (leaf_flags & VMM_USER) {
        f |= VMM_USER;
    }
    return f;
}

static u32 pml4_idx(u64 vaddr) { return (u32)((vaddr >> 39) & 511); }
static u32 pdpt_idx(u64 vaddr) { return (u32)((vaddr >> 30) & 511); }
static u32 pd_idx(u64 vaddr)   { return (u32)((vaddr >> 21) & 511); }
static u32 pt_idx(u64 vaddr)   { return (u32)((vaddr >> 12) & 511); }

/* Ensure the directory chain root -> PML4 -> PDPT -> PD exists and return
 * the PD directory frame. The terminal PD entry (PT pointer or 2 MiB leaf)
 * is deliberately left untouched for the callers to install. Returns 0 on
 * out-of-memory or when a 1 GiB huge leaf already occupies the path. */
static u64 ensure_pd(u64 root, u64 vaddr, u64 leaf_flags) {
    volatile u64 *pml4 = table_at(root);
    u64 e = pml4[pml4_idx(vaddr)];
    if (!(e & VMM_PRESENT)) {
        u64 f = alloc_table();
        if (f == 0) return 0;
        pml4[pml4_idx(vaddr)] = f | dir_flags(leaf_flags);
        e = f | dir_flags(leaf_flags);
    }

    volatile u64 *pdpt = table_at(entry_addr(e));
    e = pdpt[pdpt_idx(vaddr)];
    if (!(e & VMM_PRESENT)) {
        u64 f = alloc_table();
        if (f == 0) return 0;
        pdpt[pdpt_idx(vaddr)] = f | dir_flags(leaf_flags);
        e = f | dir_flags(leaf_flags);
    }
    if (e & VMM_HUGE) {
        return 0;                 /* Existing 1 GiB leaf blocks the path */
    }

    return entry_addr(e);         /* This IS the PD directory frame */
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void vmm_init(void) {
    /* Enable the No-Execute feature so VMM_NX leaf bits are valid. */
    u64 efer = msr_read(IA32_EFER);
    msr_write(IA32_EFER, efer | EFER_NXE);
}

u64 vmm_kernel_root(void) {
    return (u64)(&boot_pml4[0]);
}

/* Map a single 4 KiB page. flags combines VMM_PRESENT/WRITABLE/USER/
 * PWT/PCD/NX. Overwriting an existing leaf is rejected (unmap first). */
int vmm_map(u64 root, u64 vaddr, u64 paddr, u64 flags) {
    u64 pd = ensure_pd(root, vaddr, flags);
    if (pd == 0) {
        return 0;
    }
    u32 i = pt_idx(vaddr);

    /* Read the PD entry; a 4 KiB mapping needs a PT child allocated here. */
    volatile u64 *pml4 = table_at(root);
    volatile u64 *pdpt = table_at(entry_addr(pml4[pml4_idx(vaddr)]));
    volatile u64 *pdp  = table_at(entry_addr(pdpt[pdpt_idx(vaddr)]));
    u64 pde = pdp[pd_idx(vaddr)];
    u64 pt_frame;
    if (pde & VMM_HUGE) {
        return 0;                        /* Collides with a 2 MiB leaf */
    }
    if (!(pde & VMM_PRESENT)) {
        u64 f = alloc_table();
        if (f == 0) return 0;
        pdp[pd_idx(vaddr)] = f | dir_flags(flags);
        pt_frame = f;
    } else {
        pt_frame = entry_addr(pde);
    }

    volatile u64 *pt = table_at(pt_frame);
    if (pt[i] & VMM_PRESENT) {
        return 0;
    }
    u64 leaf = (paddr & 0x000FFFFFFFFFF000ULL)
             | (flags & (VMM_PRESENT | VMM_WRITABLE | VMM_USER
                         | VMM_PWT | VMM_PCD | VMM_ACCESSED));
    if (flags & VMM_NX) {
        leaf |= (1ULL << 63);
    }
    pt[i] = leaf;
    invlpg_page(vaddr);
    (void)pd;
    return 1;
}

/* Map a 2 MiB huge page (used by the HHDM extension). */
int vmm_map_2m(u64 root, u64 vaddr, u64 paddr, u64 flags) {
    u64 pd = ensure_pd(root, vaddr, flags);
    if (pd == 0) {
        return 0;
    }
    volatile u64 *pml4 = table_at(root);
    volatile u64 *pdpt = table_at(entry_addr(pml4[pml4_idx(vaddr)]));
    volatile u64 *pdp  = table_at(entry_addr(pdpt[pdpt_idx(vaddr)]));
    u32 i = pd_idx(vaddr);
    if (pdp[i] & VMM_PRESENT) {
        return 0;
    }
    u64 leaf = (paddr & 0x000FFFFFFFE00000ULL)
             | VMM_HUGE
             | (flags & (VMM_PRESENT | VMM_WRITABLE | VMM_USER
                         | VMM_PWT | VMM_PCD | VMM_ACCESSED));
    if (flags & VMM_NX) {
        leaf |= (1ULL << 63);
    }
    pdp[i] = leaf;
    invlpg_page(vaddr);
    return 1;
}

/* Translate a virtual address. Returns 0 when not present; otherwise returns
 * the physical address OR-ed with the leaf permission flags (caller masks
 * with 0xFFF to inspect them). Huge pages at PDPT/PD level are handled. */
u64 vmm_translate(u64 root, u64 vaddr) {
    volatile u64 *pml4 = table_at(root);
    u64 e = pml4[pml4_idx(vaddr)];
    if (!(e & VMM_PRESENT)) return 0;

    volatile u64 *pdpt = table_at(entry_addr(e));
    e = pdpt[pdpt_idx(vaddr)];
    if (!(e & VMM_PRESENT)) return 0;
    if (e & VMM_HUGE) {
        return (e & 0x000FFFFFC0000000ULL) | (vaddr & 0x3FFFFFFF)
               | (e & 0xFFF) | ((e >> 32) << 32);
    }

    volatile u64 *pd = table_at(entry_addr(e));
    e = pd[pd_idx(vaddr)];
    if (!(e & VMM_PRESENT)) return 0;
    if (e & VMM_HUGE) {
        return (e & 0x000FFFFFFFE00000ULL) | (vaddr & 0x1FFFFF) | (e & 0xFFF)
               | (e & (1ULL << 63));
    }

    volatile u64 *pt = table_at(entry_addr(e));
    e = pt[pt_idx(vaddr)];
    if (!(e & VMM_PRESENT)) return 0;
    return (e & 0x000FFFFFFFFFF000ULL) | (vaddr & 0xFFF) | (e & 0xFFF)
           | (e & (1ULL << 63));
}

/* Remove a 4 KiB mapping. Returns 1 if a leaf was cleared. Empty page
 * tables are intentionally kept (cheap and avoids racing other CPUs later). */
int vmm_unmap(u64 root, u64 vaddr) {
    u64 e;
    volatile u64 *pml4 = table_at(root);
    e = pml4[pml4_idx(vaddr)];
    if (!(e & VMM_PRESENT)) return 0;

    volatile u64 *pdpt = table_at(entry_addr(e));
    e = pdpt[pdpt_idx(vaddr)];
    if (!(e & VMM_PRESENT) || (e & VMM_HUGE)) return 0;

    volatile u64 *pd = table_at(entry_addr(e));
    e = pd[pd_idx(vaddr)];
    if (!(e & VMM_PRESENT) || (e & VMM_HUGE)) return 0;

    volatile u64 *pt = table_at(entry_addr(e));
    u32 i = pt_idx(vaddr);
    if (!(pt[i] & VMM_PRESENT)) return 0;
    pt[i] = 0;
    invlpg_page(vaddr);
    return 1;
}

/* Grow the HHDM direct map with 2 MiB pages over a physical RAM interval.
 * Called by the PMM once the first-gigabyte buddy is online; allocates the
 * extra PDPT/PD frames from that buddy. The interval is expanded to full
 * 2 MiB boundaries (floor start, ceil end) so every usable frame is
 * reachable; partially-covered boundary pages may alias reserved holes,
 * which is harmless for a kernel-only direct map. */
void vmm_hhdm_extend(u64 root, u64 start_pa, u64 end_pa) {
    u64 cur = start_pa & ~0x1FFFFFULL;
    u64 end = (end_pa + 0x1FFFFF) & ~0x1FFFFFULL;
    u64 flags = VMM_PRESENT | VMM_WRITABLE | VMM_NX | VMM_PWT;
    while (cur < end) {
        vmm_map_2m(root, HHDM_BASE + cur, cur, flags);
        cur += 0x200000;
    }
}

u64 vmm_read_cr3(void) {
    return cr3_read();
}

void vmm_write_cr3(u64 root) {
    cr3_write(root);
}
