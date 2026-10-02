package mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern u64 pmm_alloc_page(void);
extern u64 pmm_alloc_pages(u32 order);
extern void pmm_free_pages(u64 paddr, u32 order);

/* ---------------------------------------------------------------------------
 * Slab kernel heap
 *
 * Two allocation paths, all pointers returned are HHDM virtual addresses:
 *
 *   size <= 2048  -> one of 8 size-class caches. Each cache is a chain of
 *                    single-page slabs; the 32-byte slab header lives at the
 *                    page base, objects follow, and free objects thread an
 *                    intrusive singly-linked list through their first 8 bytes.
 *
 *   size >  2048  -> a power-of-two buddy block (order 1..10, 8 KiB .. 4 MiB).
 *
 * A packed per-frame table (`page_info`) gives O(1) kfree resolution: class
 * caches store (class_index + 1), buddy blocks store (0x80000000 | order).
 * ------------------------------------------------------------------------- */

static const u64 HHDM_BASE = 0xFFFF800000000000ULL;
static const u64 PAGE_SIZE = 4096;

enum {
    NCLASSES       = 8,
    MIN_CLASS_SIZE = 16,
    MAX_CLASS_SIZE = 2048,
    HEADER_SIZE    = 32,               /* sizeof(struct slab_header)         */
    INFO_NONE      = 0xFFFFFFFF,
    INFO_BIG       = 0x80000000,
    BIG_ORDER_MAX  = 10,
    SLAB_MAGIC     = 0x534C414242414C53ULL,   /* "SLABBALS" */
    MAX_FRAMES     = 1048576           /* 4 GiB / 4 KiB (must match pmm.c)   */
};

struct slab_header {
    u64 magic;
    u32 cls;
    u32 nfree;
    u64 free_head;     /* Physical address of first free object, or 0 */
    u64 next;          /* Next slab in the class chain (paddr), or 0  */
};

static u16 class_size[NCLASSES] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };

/* Per-frame dispatch table (one u32 per physical frame). */
static u32 page_info[MAX_FRAMES];

/* Slab chain head per class (physical address). */
static u64 slab_head[NCLASSES];

static volatile u64 *hhdm(u64 paddr) {
    return (volatile u64 *)(HHDM_BASE + paddr);
}

static u32 size_to_class(u64 size) {
    u32 c;
    for (c = 0; c < NCLASSES; c++) {
        if (size <= class_size[c]) {
            return c;
        }
    }
    return NCLASSES;
}

static u32 big_order_for(u64 size) {
    u32 order = 1;                    /* 8 KiB minimum for the big path */
    while (order <= BIG_ORDER_MAX) {
        u64 bytes = (u64)1 << (order + 12);
        if (bytes >= size) {
            return order;
        }
        order++;
    }
    return NCLASSES + 1;             /* Unsupported */
}

/* Create a fresh single-page slab for class c and prepend it to the chain. */
static int slab_grow(u32 c) {
    u64 page = pmm_alloc_page();
    if (page == 0) {
        return 0;
    }

    u32 obj_size = class_size[c];
    u32 capacity = (u32)((PAGE_SIZE - HEADER_SIZE) / obj_size);
    u32 pfn = (u32)(page / PAGE_SIZE);

    struct slab_header *h = (struct slab_header *)(HHDM_BASE + page);
    h->magic = SLAB_MAGIC;
    h->cls = c;
    h->nfree = capacity;
    h->next = slab_head[c];

    /* Thread every object onto the free chain (lowest address at the head
     * by building in reverse), intrusive next-pointer in qword 0. */
    u64 prev = 0;
    u32 i;
    for (i = capacity - 1; ; i--) {
        u64 obj = page + HEADER_SIZE + (u64)i * obj_size;
        *hhdm(obj) = prev;
        prev = obj;
        if (i == 0) break;
    }
    h->free_head = prev;

    slab_head[c] = page;
    page_info[pfn] = c + 1;
    return 1;
}

void *kmalloc(u64 size) {
    if (size == 0) {
        size = MIN_CLASS_SIZE;
    }

    u32 c = size_to_class(size);
    if (c < NCLASSES) {
        /* Find a slab with a free object. */
        u64 page = slab_head[c];
        struct slab_header *h = 0;
        while (page != 0) {
            h = (struct slab_header *)(HHDM_BASE + page);
            if (h->nfree > 0) break;
            page = h->next;
        }
        if (page == 0 || h == 0) {
            if (!slab_grow(c)) {
                return 0;
            }
            page = slab_head[c];
            h = (struct slab_header *)(HHDM_BASE + page);
        }

        u64 obj = h->free_head;
        h->free_head = *hhdm(obj);
        h->nfree--;
        return (void *)(HHDM_BASE + obj);
    }

    /* Big allocation: contiguous buddy block. */
    u32 order = big_order_for(size);
    if (order > BIG_ORDER_MAX) {
        return 0;
    }
    u64 block = pmm_alloc_pages(order);
    if (block == 0) {
        return 0;
    }
    u32 frames = (u32)1 << order;
    u32 base_pfn = (u32)(block / PAGE_SIZE);
    u32 i;
    for (i = 0; i < frames; i++) {
        page_info[base_pfn + i] = INFO_BIG | order;
    }
    return (void *)(HHDM_BASE + block);
}

void kfree(void *ptr) {
    if (ptr == 0) {
        return;
    }
    u64 vaddr = (u64)ptr;
    if (vaddr < HHDM_BASE) {
        return;                        /* Not a kernel heap pointer */
    }
    u64 paddr = vaddr - HHDM_BASE;
    u32 pfn = (u32)(paddr / PAGE_SIZE);
    if (pfn >= MAX_FRAMES) {
        return;
    }

    u32 info = page_info[pfn];
    if (info == INFO_NONE) {
        return;                        /* Double free / foreign pointer */
    }

    if (info & INFO_BIG) {
        u32 order = info & 0x7F;
        u32 frames = (u32)1 << order;
        u32 base_pfn = pfn & ~(frames - 1);
        u32 i;
        for (i = 0; i < frames; i++) {
            page_info[base_pfn + i] = INFO_NONE;
        }
        pmm_free_pages((u64)base_pfn * PAGE_SIZE, order);
        return;
    }

    /* Slab object: the owning slab header sits at its single page base. */
    u32 c = info - 1;
    u64 page = (u64)pfn * PAGE_SIZE;
    struct slab_header *h = (struct slab_header *)(HHDM_BASE + page);
    u64 obj = paddr;
    *hhdm(obj) = h->free_head;
    h->free_head = obj;
    h->nfree++;
}

/* ---------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

u64 kmalloc_class_size(u32 c) {
    if (c >= NCLASSES) return 0;
    return class_size[c];
}

u32 kmalloc_class_count(void) {
    return NCLASSES;
}

/* Number of slab pages currently backing each class. */
u32 kmalloc_class_slabs(u32 c) {
    u32 n = 0;
    u64 page = slab_head[c];
    while (page != 0) {
        struct slab_header *h = (struct slab_header *)(HHDM_BASE + page);
        n++;
        page = h->next;
    }
    return n;
}

u32 kmalloc_class_capacity(u32 c) {
    return (u32)((PAGE_SIZE - HEADER_SIZE) / class_size[c]);
}
