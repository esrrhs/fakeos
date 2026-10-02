package mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void tlb_flush(void);

/* Kernel image end (high-half VMA); physical end = VMA - KERNEL_VMA. */
extern char _kernel_end;

/* Boot PML4 physical base lives in .boot.bss at a low VMA equal to its
 * physical address, so &boot_pml4 is already the frame number 0x102000. */
extern u8 boot_pml4[];

/* Same-package VMM helpers (mem/vmm.c). */
extern u64 vmm_kernel_root(void);
extern void vmm_hhdm_extend(u64 root, u64 start_pa, u64 end_pa);

/* ---------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

static const u64 HHDM_BASE      = 0xFFFF800000000000ULL;
static const u64 KERNEL_VMA     = 0xFFFFFFFF80000000ULL;
static const u64 PAGE_SIZE      = 4096;
static const u64 KERNEL_LMA     = 0x00100000;     /* Kernel loads at 1 MiB   */
static const u64 BOOT_WINDOW    = 0x40000000;     /* Static HHDM covers 1 GiB */
static const u64 MAX_PHYS       = 0x100000000ULL; /* Managed RAM ceiling: 4 GiB */

/* Buddy geometry: orders 0..10 => blocks of 4 KiB .. 4 MiB (1024 frames). */
enum {
    MAX_ORDER     = 10,
    MAX_FRAMES    = 1048576,          /* 4 GiB / 4 KiB */
    NIL           = 0xFFFFFFFF,
    ST_RESERVED   = 0xFF,             /* Hole, BIOS/device range or in use   */
    ST_AVAIL      = 0xFC,             /* RAM available, not yet handed to    */
                                      /* the buddy allocator                 */
    ST_INTERIOR   = 0xFD,             /* Non-head frame of a free block      */
    ST_ALLOCATED  = 0xFE,             /* Head frame of an allocated block    */
    MAX_REGIONS   = 32
};

/* ---------------------------------------------------------------------------
 * Allocator state
 * ------------------------------------------------------------------------- */

/* One state byte per physical frame. A free block head stores its order
 * (0..10); everything else uses one of the ST_* sentinels above. */
static u8 frame_state[MAX_FRAMES];

/* Free list head per order (PFN, NIL when empty). Lists are kept sorted by
 * address in ascending order so allocation always returns the lowest fit,
 * and merge behaviour is deterministic. The singly-linked "next PFN" of a
 * free block is stored in the first 4 bytes of the free frame itself,
 * reached through the HHDM direct map. */
static u32 free_head[MAX_ORDER + 1];

static u64 managed_total;             /* Frames the allocator owns          */
static u64 free_count;                /* Free frames across all orders      */
static u32 highest_pfn;               /* One past highest tracked frame      */

/* Multiboot memory map snapshot (for diagnostics). */
static u64 region_base[MAX_REGIONS];
static u64 region_len[MAX_REGIONS];
static u32 region_type[MAX_REGIONS];
static u32 region_count;

/* ---------------------------------------------------------------------------
 * Raw physical memory access through HHDM
 * ------------------------------------------------------------------------- */

static volatile u32 *phys32(u64 paddr) {
    return (volatile u32 *)(HHDM_BASE + paddr);
}

static u32 rd32(u64 paddr) {
    return *phys32(paddr);
}

static u64 rd64(u64 paddr) {
    return (u64)rd32(paddr) | ((u64)rd32(paddr + 4) << 32);
}

static u32 list_next(u32 pfn) {
    return *phys32((u64)pfn * PAGE_SIZE);
}

static void list_set_next(u32 pfn, u32 next) {
    *phys32((u64)pfn * PAGE_SIZE) = next;
}

u64 pmm_phys_to_virt(u64 paddr) {
    return HHDM_BASE + paddr;
}

/* ---------------------------------------------------------------------------
 * Address-ordered intrusive free lists
 * ------------------------------------------------------------------------- */

static void list_insert_sorted(u32 order, u32 pfn) {
    u32 head = free_head[order];
    if (head == NIL || pfn < head) {
        list_set_next(pfn, head);
        free_head[order] = pfn;
        return;
    }
    u32 cur = head;
    for (;;) {
        u32 nx = list_next(cur);
        if (nx == NIL || pfn < nx) {
            list_set_next(pfn, nx);
            list_set_next(cur, pfn);
            return;
        }
        cur = nx;
    }
}

/* Remove a known block head from an order list. */
static void list_remove(u32 order, u32 pfn) {
    u32 cur = free_head[order];
    if (cur == pfn) {
        free_head[order] = list_next(cur);
        return;
    }
    while (cur != NIL) {
        u32 nx = list_next(cur);
        if (nx == pfn) {
            list_set_next(cur, list_next(nx));
            return;
        }
        cur = nx;
    }
}

/* Pop the lowest-address block of the given order. */
static u32 list_pop(u32 order) {
    u32 pfn = free_head[order];
    if (pfn != NIL) {
        free_head[order] = list_next(pfn);
    }
    return pfn;
}

/* ---------------------------------------------------------------------------
 * Buddy core
 * ------------------------------------------------------------------------- */

/* Hand one naturally-aligned contiguous run [start,end) to the allocator,
 * breaking it into the largest buddy blocks that fit alignment and length. */
static void add_run(u32 start, u32 end) {
    u32 pfn = start;
    while (pfn < end) {
        u32 remaining = end - pfn;
        u32 order = 0;
        u32 o;
        for (o = 0; o <= MAX_ORDER; o++) {
            u32 block = (u32)1 << o;
            if (block > remaining) break;
            if ((pfn & (block - 1)) != 0) break;
            order = o;
        }
        u32 block = (u32)1 << order;
        frame_state[pfn] = (u8)order;
        u32 k;
        for (k = 1; k < block; k++) {
            frame_state[pfn + k] = ST_INTERIOR;
        }
        list_insert_sorted(order, pfn);
        managed_total += block;
        free_count += block;
        pfn += block;
    }
}

u64 pmm_alloc_pages(u32 order) {
    if (order > MAX_ORDER) {
        return 0;
    }
    u32 fit = order;
    while (fit <= MAX_ORDER && free_head[fit] == NIL) {
        fit++;
    }
    if (fit > MAX_ORDER) {
        return 0;                    /* Out of physical memory */
    }

    u32 pfn = list_pop(fit);

    /* Split down until the block matches the requested order. The right
     * buddy is returned to the (lower) order's address-sorted list; those
     * frames stay free, so only the delivered 2^order block is charged at
     * the end (charging the popped 2^fit here would double-count the
     * re-inserted buddies). */
    while (fit > order) {
        fit--;
        u32 buddy = pfn + ((u32)1 << fit);
        frame_state[buddy] = (u8)fit;
        list_insert_sorted(fit, buddy);
    }

    free_count -= (u64)1 << order;
    frame_state[pfn] = ST_ALLOCATED;
    return (u64)pfn * PAGE_SIZE;
}

u64 pmm_alloc_page(void) {
    return pmm_alloc_pages(0);
}

static int block_is_free_head(u32 pfn) {
    u8 st = frame_state[pfn];
    return st <= MAX_ORDER;
}

void pmm_free_pages(u64 paddr, u32 order) {
    if (order > MAX_ORDER) {
        return;
    }
    u32 pfn = (u32)(paddr / PAGE_SIZE);
    if (pfn >= MAX_FRAMES) {
        return;
    }
    /* Only a currently allocated block head may be released. Interior frames
     * and double frees are rejected to keep the free-set invariant. */
    if (frame_state[pfn] != ST_ALLOCATED) {
        return;
    }

    frame_state[pfn] = (u8)order;
    free_count += (u64)1 << order;

    /* Merge with an identically-sized free buddy while possible. */
    while (order < MAX_ORDER) {
        u32 buddy = pfn ^ ((u32)1 << order);
        u32 span = (u32)1 << (order + 1);
        if (buddy >= highest_pfn) break;               /* Outside tracked RAM */
        if (frame_state[buddy] != (u8)order) break;   /* Buddy not free here  */
        if ((pfn & (span - 1)) != 0) break;           /* Misaligned pair      */

        list_remove(order, buddy);
        if (buddy < pfn) {
            pfn = buddy;
        }
        order++;
        frame_state[pfn] = (u8)order;
    }

    list_insert_sorted(order, pfn);
}

void pmm_free_page(u64 paddr) {
    pmm_free_pages(paddr, 0);
}

/* ---------------------------------------------------------------------------
 * Multiboot 1 memory map parsing
 * ------------------------------------------------------------------------- */

static void record_region(u64 base, u64 length, u32 type) {
    if (region_count >= MAX_REGIONS) {
        return;
    }
    region_base[region_count] = base;
    region_len[region_count] = length;
    region_type[region_count] = type;
    region_count++;
}

static void parse_mb1(u64 mbi_paddr) {
    u8 *mbi = (u8 *)(HHDM_BASE + mbi_paddr);
    u32 flags = *(volatile u32 *)(mbi + 0);

    if (flags & (1u << 6)) {
        u32 mmap_len  = *(volatile u32 *)(mbi + 44);
        u32 mmap_addr = *(volatile u32 *)(mbi + 48);
        u32 cur = mmap_addr;
        u32 end = mmap_addr + mmap_len;
        while (cur < end) {
            u32 size   = rd32(cur);
            u64 base   = rd64(cur + 4);
            u64 length = rd64(cur + 12);
            u32 type   = rd32(cur + 20);
            record_region(base, length, type);
            if (size == 0) break;
            cur += size + 4;
        }
        return;
    }

    /* Fallback: basic lower/upper memory information (KiB counters). */
    u32 mem_lower = *(volatile u32 *)(mbi + 4);
    u32 mem_upper = *(volatile u32 *)(mbi + 8);
    record_region(0, (u64)mem_lower * 1024, 1);
    record_region(KERNEL_LMA, (u64)mem_upper * 1024, 1);
}

/* ---------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------- */

static void mark_range_available(u64 start, u64 end) {
    if (start >= MAX_PHYS) return;
    if (end > MAX_PHYS) end = MAX_PHYS;
    u32 pfn0 = (u32)(start / PAGE_SIZE);
    u32 pfn1 = (u32)(end / PAGE_SIZE);
    if (pfn0 > highest_pfn) highest_pfn = pfn0;
    if (pfn1 > highest_pfn) highest_pfn = pfn1;
    u32 pfn;
    for (pfn = pfn0; pfn < pfn1; pfn++) {
        frame_state[pfn] = ST_AVAIL;
    }
}

static void mark_range_reserved(u64 start, u64 end) {
    if (start >= MAX_PHYS) return;
    if (end > MAX_PHYS) end = MAX_PHYS;
    u32 pfn0 = (u32)(start / PAGE_SIZE);
    u32 pfn1 = (u32)(end / PAGE_SIZE);
    u32 pfn;
    for (pfn = pfn0; pfn < pfn1; pfn++) {
        frame_state[pfn] = ST_RESERVED;
    }
}

/* Hand every AVAIL run in [lo_pfn, hi_pfn) to the buddy allocator. */
static void buddy_consume(u32 lo_pfn, u32 hi_pfn) {
    u32 pfn = lo_pfn;
    while (pfn < hi_pfn) {
        if (frame_state[pfn] != ST_AVAIL) {
            pfn++;
            continue;
        }
        u32 start = pfn;
        while (pfn < hi_pfn && frame_state[pfn] == ST_AVAIL) {
            pfn++;
        }
        add_run(start, pfn);
    }
}

void pmm_init(u64 mbi_paddr) {
    u32 i;
    u32 order;

    managed_total = 0;
    free_count = 0;
    highest_pfn = 0;
    region_count = 0;
    for (i = 0; i <= MAX_ORDER; i++) {
        free_head[i] = NIL;
    }
    for (i = 0; i < MAX_FRAMES; i++) {
        frame_state[i] = ST_RESERVED;
    }

    parse_mb1(mbi_paddr);

    /* Pass 1: every type-1 (available RAM) region becomes AVAIL, clipped to
     * the 4 GiB manager ceiling. */
    for (i = 0; i < region_count; i++) {
        if (region_type[i] == 1) {
            mark_range_available(region_base[i],
                                 region_base[i] + region_len[i]);
        }
    }

    /* Pass 2: carve out ranges the allocator must never hand out.
     *  - the first 1 MiB (BIOS data, IVT, VGA buffer, Multiboot info);
     *  - the whole kernel image (bootstrap tables + text/data/bss + PMM/VMM
     *    metadata), from its 1 MiB load address through _kernel_end.
     * Both live inside the first gigabyte. */
    u64 kernel_phys_end = (u64)(&_kernel_end) - KERNEL_VMA;
    kernel_phys_end = (kernel_phys_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    mark_range_reserved(0, KERNEL_LMA);
    mark_range_reserved(KERNEL_LMA, kernel_phys_end);

    /* Pass 3a: build the buddy allocator over the statically-mapped first
     * gigabyte so page-table frames can be allocated immediately. */
    u32 window_pfn = (u32)(BOOT_WINDOW / PAGE_SIZE);   /* 262144 */
    if (highest_pfn < window_pfn) window_pfn = highest_pfn;
    buddy_consume(0, window_pfn);

    /* Pass 3b: grow the HHDM direct map (2 MiB pages, page-table frames
     * served by the just-built buddy) to cover all RAM above 1 GiB, then
     * hand those frames to the buddy allocator as well. */
    if (highest_pfn > (u32)(BOOT_WINDOW / PAGE_SIZE)) {
        u64 root = vmm_kernel_root();
        for (i = 0; i < region_count; i++) {
            if (region_type[i] != 1) continue;
            u64 base = region_base[i];
            u64 end = base + region_len[i];
            if (end <= BOOT_WINDOW) continue;
            if (base < BOOT_WINDOW) base = BOOT_WINDOW;
            if (end > MAX_PHYS) end = MAX_PHYS;
            if (base < end) {
                vmm_hhdm_extend(root, base, end);
            }
        }
        buddy_consume((u32)(BOOT_WINDOW / PAGE_SIZE), highest_pfn);
    }

    /* Tear down the temporary identity mapping. All runtime accesses to
     * physical memory now go through HHDM; the low canonical half is left
     * entirely for future user space. */
    u64 pml4_phys = (u64)(&boot_pml4[0]);
    *phys32(pml4_phys) = 0;
    tlb_flush();
}

/* ---------------------------------------------------------------------------
 * Diagnostics / statistics
 * ------------------------------------------------------------------------- */

u32 pmm_region_count(void) {
    return region_count;
}

u64 pmm_region_base(u32 i) {
    if (i >= region_count) return 0;
    return region_base[i];
}

u64 pmm_region_len(u32 i) {
    if (i >= region_count) return 0;
    return region_len[i];
}

u32 pmm_region_type(u32 i) {
    if (i >= region_count) return 0;
    return region_type[i];
}

u64 pmm_total_pages(void) {
    return managed_total;
}

u64 pmm_free_pages_count(void) {
    return free_count;
}

u64 pmm_used_pages(void) {
    return managed_total - free_count;
}

u32 pmm_max_order(void) {
    return MAX_ORDER;
}
