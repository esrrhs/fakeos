package drivers;
import arch;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);
extern u32 pci_find_device(u16 vendor, u16 device, u8 *obus, u8 *oslot, u8 *ofunc);
extern u32 pci_find_vendor_subsystem(u16 vendor, u16 subsystem,
                                     u8 *obus, u8 *oslot, u8 *ofunc);
extern void pci_enable_busmaster(u8 bus, u8 slot, u8 func);
extern u32 pci_bar0(u8 bus, u8 slot, u8 func, u32 *addr, u32 *is_io);
extern u16 pci_read16(u8 bus, u8 slot, u8 func, u8 offset);
extern u32 pci_read32(u8 bus, u8 slot, u8 func, u8 offset);
extern u32 pci_virtio_map_cap(u8 bus, u8 slot, u8 func, u8 cfg_type,
                              u64 *mmio_phys, u32 *len, u32 *notify_mult);
extern u32 pci_msi_enable(u8 bus, u8 slot, u8 func, u8 vector);
extern void cpu_pause(void);
extern void io_wait(void);

/* ---------------------------------------------------------------------------
 * VirtIO-blk PCI driver - legacy I/O BAR + modern MMIO, polling completion.
 *
 * Default `make qemu-disk` still uses disable-modern=on (legacy BAR). When the
 * device exposes modern vendor caps (common/notify/ISR/device) and no usable
 * legacy I/O BAR0, or when `make qemu-modern` omits disable-modern, the modern
 * path maps those MMIO windows and programs queue_desc/driver/device. MSI is
 * optional (pci_msi_enable); completion remains polled. Port I/O / MMIO peeks
 * in the poll loop keep QEMU/TCG retiring host AIO.
 * ------------------------------------------------------------------------- */

static const u64 HHDM_BASE = 0xFFFF800000000000ULL;

enum {
    VIRTIO_VENDOR       = 0x1AF4,
    VIRTIO_DEV_BLK_LEGACY = 0x1001,   /* transitional/legacy blk device id */
    VIRTIO_SUBSYS_BLK   = 0x0002,     /* subsystem device id for blk */

    /* Legacy I/O BAR register offsets (VirtIO 1.0 §4.1.4.8). */
    REG_DEV_FEATURES    = 0,
    REG_DRV_FEATURES    = 4,
    REG_QUEUE_PFN       = 8,
    REG_QUEUE_NUM       = 12,
    REG_QUEUE_SEL       = 14,
    REG_QUEUE_NOTIFY    = 16,
    REG_STATUS          = 18,
    REG_ISR             = 19,
    /* Device-specific config starts at 20 (no MSI-X). */
    REG_CFG_CAPACITY_LO = 20,
    REG_CFG_CAPACITY_HI = 24,

    STATUS_ACKNOWLEDGE  = 1,
    STATUS_DRIVER       = 2,
    STATUS_DRIVER_OK    = 4,
    STATUS_FEATURES_OK  = 8,
    STATUS_FAILED       = 128,

    /* Modern common-cfg offsets (VirtIO 1.0 §4.1.4.3). */
    M_DEV_FEAT_SEL      = 0,
    M_DEV_FEAT          = 4,
    M_DRV_FEAT_SEL      = 8,
    M_DRV_FEAT          = 12,
    M_MSIX_CONFIG       = 16,
    M_NUM_QUEUES        = 18,
    M_STATUS            = 20,
    M_CFG_GEN           = 21,
    M_Q_SEL             = 22,
    M_Q_SIZE            = 24,
    M_Q_MSIX            = 26,
    M_Q_ENABLE          = 28,
    M_Q_NOTIFY_OFF      = 30,
    M_Q_DESC            = 32,
    M_Q_DRIVER          = 40,
    M_Q_DEVICE          = 48,

    VIRTIO_PCI_CAP_COMMON_CFG = 1,
    VIRTIO_PCI_CAP_NOTIFY_CFG = 2,
    VIRTIO_PCI_CAP_ISR_CFG    = 3,
    VIRTIO_PCI_CAP_DEVICE_CFG = 4,

    VRING_DESC_F_NEXT   = 1,
    VRING_DESC_F_WRITE  = 2,

    VIRTIO_BLK_T_IN     = 0,          /* read  device -> guest */
    VIRTIO_BLK_T_OUT    = 1,          /* write guest -> device */
    VIRTIO_BLK_T_FLUSH  = 4,

    VIRTIO_BLK_S_OK     = 0,
    VIRTIO_BLK_S_IOERR  = 1,

    SECTOR_SIZE         = 512,
    /* QEMU 10 virtio-blk legacy often reports 1024; legacy QueueNum is
     * read-only so the guest must size the vring to match exactly. */
    QUEUE_MAX           = 1024,
    /* pmm_alloc_pages(order) returns 2^order contiguous pages.
     * order 4 = 16 pages = 64 KiB: queue (~28 KiB at 1024 descs) + req/bounce. */
    ARENA_ORDER         = 4,
    ARENA_BYTES         = 65536,
    POLL_SPINS_MAX      = 2000000
};

/* Virtqueue descriptor (16 bytes) - laid out manually as bytes to avoid any
 * compiler padding surprises under fakecc. */
struct vring_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

struct virtio_blk {
    u32 present;
    u32 modern;                /* 1 = MMIO modern path */
    u16 iobase;                /* legacy I/O BAR */
    u16 queue_size;
    u64 capacity;              /* sectors */
    u64 arena_phys;            /* contiguous DMA pages */
    u64 desc_phys;
    u64 avail_phys;
    u64 used_phys;
    u64 req_phys;              /* request header + status (+ sector bounce) */
    u16 free_head;
    u16 last_used_idx;
    /* Modern MMIO windows (phys, accessed via HHDM). */
    u64 common_phys;
    u64 notify_phys;
    u64 isr_phys;
    u64 devcfg_phys;
    u32 notify_mult;
    u16 notify_off;
};

static struct virtio_blk vb;


static volatile u8 *hhdm8(u64 phys) {
    return (volatile u8 *)(HHDM_BASE + phys);
}

static volatile u16 *hhdm16(u64 phys) {
    return (volatile u16 *)(HHDM_BASE + phys);
}

static volatile u32 *hhdm32(u64 phys) {
    return (volatile u32 *)(HHDM_BASE + phys);
}

static volatile u64 *hhdm64(u64 phys) {
    return (volatile u64 *)(HHDM_BASE + phys);
}

static volatile u8 *m8(u64 phys, u32 off) {
    return hhdm8(phys + (u64)off);
}
static volatile u16 *m16(u64 phys, u32 off) {
    return hhdm16(phys + (u64)off);
}
static volatile u32 *m32(u64 phys, u32 off) {
    return hhdm32(phys + (u64)off);
}
static volatile u64 *m64(u64 phys, u32 off) {
    return hhdm64(phys + (u64)off);
}

static void status_write(u8 v) {
    if (vb.modern) {
        *m8(vb.common_phys, M_STATUS) = v;
    } else {
        arch.outb((u16)(vb.iobase + REG_STATUS), v);
    }
}
static u8 status_read(void) {
    if (vb.modern) {
        return *m8(vb.common_phys, M_STATUS);
    }
    return arch.inb((u16)(vb.iobase + REG_STATUS));
}
static u8 isr_read(void) {
    if (vb.modern) {
        return *m8(vb.isr_phys, 0);
    }
    return arch.inb((u16)(vb.iobase + REG_ISR));
}
static void queue_notify(void) {
    if (vb.modern) {
        u32 off = (u32)vb.notify_off * vb.notify_mult;
        *m16(vb.notify_phys, off) = 0; /* queue index 0 */
    } else {
        arch.outw((u16)(vb.iobase + REG_QUEUE_NOTIFY), 0);
    }
}

static void iow8(u16 off, u8 v) {
    arch.outb((u16)(vb.iobase + off), v);
}

static u8 ior8(u16 off) {
    return arch.inb((u16)(vb.iobase + off));
}

static void iow16(u16 off, u16 v) {
    arch.outw((u16)(vb.iobase + off), v);
}

static u16 ior16(u16 off) {
    return arch.inw((u16)(vb.iobase + off));
}

static void iow32(u16 off, u32 v) {
    arch.outl((u16)(vb.iobase + off), v);
}

static u32 ior32(u16 off) {
    return arch.inl((u16)(vb.iobase + off));
}

static void desc_write(u16 idx, u64 addr, u32 len, u16 flags, u16 next) {
    u64 p = vb.desc_phys + ((u64)idx) * 16;
    *hhdm64(p + 0) = addr;
    *hhdm32(p + 8) = len;
    *hhdm16(p + 12) = flags;
    *hhdm16(p + 14) = next;
}

/* Align `n` up to `a` (a power of two). */
static u64 align_up(u64 n, u64 a) {
    return (n + a - 1) & ~(a - 1);
}

static u32 setup_queue(void) {
    u64 desc_sz;
    u64 avail_sz;
    u64 used_off;
    u64 used_sz;
    u64 total;
    u64 arena;
    u16 i;
    u16 qsz;

    if (vb.modern) {
        *m16(vb.common_phys, M_Q_SEL) = 0;
        qsz = *m16(vb.common_phys, M_Q_SIZE);
        if (qsz == 0 || qsz > QUEUE_MAX) {
            /* Device offered 0 or huge: pick a sane size and program it. */
            qsz = 256;
            *m16(vb.common_phys, M_Q_SIZE) = qsz;
        }
    } else {
        iow16(REG_QUEUE_SEL, 0);
        /* Legacy QueueNum is read-only; never write it (QEMU logs
         * virtio_ioport_write unexpected address 0xc and ignores the store).
         * Use the device size as-is. Cap only by refusing oversized rings. */
        qsz = ior16(REG_QUEUE_NUM);
        if (qsz == 0) {
            kprintf("[VIRTIO] Queue Num is 0\n");
            return 0;
        }
        if (qsz > QUEUE_MAX) {
            kprintf("[VIRTIO] Queue Num %u exceeds driver max %u\n",
                    (u64)qsz, (u64)QUEUE_MAX);
            return 0;
        }
    }
    vb.queue_size = qsz;
    kprintf("[VIRTIO] queue0 size=%u modern=%u\n", (u64)qsz, (u64)vb.modern);

    desc_sz = (u64)qsz * 16;
    /* flags + idx + ring[qsz] + used_event */
    avail_sz = 6 + (u64)qsz * 2;
    /* Legacy Queue Align is 4096: pad between avail and used to a page. */
    used_off = align_up(desc_sz + avail_sz, 4096);
    used_sz = 6 + (u64)qsz * 8;            /* flags + idx + used_elem[qsz] */
    total = used_off + used_sz;

    arena = mem.pmm_alloc_pages(ARENA_ORDER);
    if (arena == 0) {
        return 0;
    }
    vb.arena_phys = arena;
    {
        u64 z;
        for (z = 0; z < (u64)ARENA_BYTES; z++) {
            hhdm8(arena)[z] = 0;
        }
    }

    vb.desc_phys = arena;
    vb.avail_phys = arena + desc_sz;
    vb.used_phys = arena + used_off;
    vb.req_phys = arena + used_off + align_up(used_sz, 4096);
    kprintf("[VIRTIO] vring desc=%u avail=%u used=%u req=%u\n",
            vb.desc_phys, vb.avail_phys, vb.used_phys, vb.req_phys);
    (void)total;

    /* Build a free list of descriptors 0..qsz-1. */
    for (i = 0; i < qsz - 1; i++) {
        desc_write(i, 0, 0, 0, (u16)(i + 1));
    }
    desc_write((u16)(qsz - 1), 0, 0, 0, 0xFFFF);
    vb.free_head = 0;
    vb.last_used_idx = 0;

    if (vb.modern) {
        *m16(vb.common_phys, M_Q_SEL) = 0;
        *m16(vb.common_phys, M_Q_SIZE) = qsz;
        *m64(vb.common_phys, M_Q_DESC) = vb.desc_phys;
        *m64(vb.common_phys, M_Q_DRIVER) = vb.avail_phys;
        *m64(vb.common_phys, M_Q_DEVICE) = vb.used_phys;
        vb.notify_off = *m16(vb.common_phys, M_Q_NOTIFY_OFF);
        *m16(vb.common_phys, M_Q_ENABLE) = 1;
    } else {
        /* Tell the device the PFN (physical / 4096) of the queue. */
        iow32(REG_QUEUE_PFN, (u32)(arena / 4096));
    }
    return 1;
}

static u16 alloc_desc(void) {
    u16 i = vb.free_head;
    if (i == 0xFFFF) {
        return 0xFFFF;
    }
    vb.free_head = *hhdm16(vb.desc_phys + ((u64)i) * 16 + 14);
    return i;
}

static void free_desc_chain(u16 head) {
    u16 i = head;
    while (1) {
        u16 flags = *hhdm16(vb.desc_phys + ((u64)i) * 16 + 12);
        u16 next = *hhdm16(vb.desc_phys + ((u64)i) * 16 + 14);
        *hhdm16(vb.desc_phys + ((u64)i) * 16 + 14) = vb.free_head;
        vb.free_head = i;
        if ((flags & VRING_DESC_F_NEXT) == 0) {
            break;
        }
        i = next;
    }
}

/* Submit a 3-descriptor blk request and poll until completion.
 * type = IN/OUT/FLUSH; for IN/OUT, data_phys points at `len` bytes. */
static u32 blk_request(u32 type, u64 sector, u64 data_phys, u32 len) {
    u16 d0;
    u16 d1;
    u16 d2;
    u64 hdr = vb.req_phys;
    u64 status_phys = vb.req_phys + 16;
    u16 avail_idx;
    u16 used_idx;
    u32 spins;

    if (!vb.present) {
        return 0;
    }

    /* Request header: type(u32) + ioprio(u32) + sector(u64). */
    *hhdm32(hdr + 0) = type;
    *hhdm32(hdr + 4) = 0;
    *hhdm64(hdr + 8) = sector;
    *hhdm8(status_phys) = 0xFF;

    d0 = alloc_desc();
    d1 = alloc_desc();
    d2 = alloc_desc();
    if (d0 == 0xFFFF || d1 == 0xFFFF || d2 == 0xFFFF) {
        if (d0 != 0xFFFF) free_desc_chain(d0);
        if (d1 != 0xFFFF) free_desc_chain(d1);
        if (d2 != 0xFFFF) free_desc_chain(d2);
        return 0;
    }

    /* Header: device-readable. */
    desc_write(d0, hdr, 16, VRING_DESC_F_NEXT, d1);

    if (type == VIRTIO_BLK_T_FLUSH) {
        /* No data descriptor - chain header -> status. */
        desc_write(d0, hdr, 16, VRING_DESC_F_NEXT, d2);
        free_desc_chain(d1);           /* unused */
        desc_write(d2, status_phys, 1, VRING_DESC_F_WRITE, 0);
    } else if (type == VIRTIO_BLK_T_IN) {
        /* Read: data device-writable. */
        desc_write(d1, data_phys, len, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, d2);
        desc_write(d2, status_phys, 1, VRING_DESC_F_WRITE, 0);
    } else {
        /* Write: data device-readable. */
        desc_write(d1, data_phys, len, VRING_DESC_F_NEXT, d2);
        desc_write(d2, status_phys, 1, VRING_DESC_F_WRITE, 0);
    }

    /* Publish into the available ring. */
    avail_idx = *hhdm16(vb.avail_phys + 2);
    *hhdm16(vb.avail_phys + 4 + ((u64)(avail_idx % vb.queue_size)) * 2) = d0;
    /* Memory barrier substitute: a dummy ISR read forces ordering on x86. */
    (void)isr_read();
    *hhdm16(vb.avail_phys + 2) = (u16)(avail_idx + 1);
    (void)isr_read();

    queue_notify();

    /* Poll the used ring; occasional port I/O yields to QEMU/TCG aio. */
    spins = 0;
    while (1) {
        used_idx = *hhdm16(vb.used_phys + 2);
        if (used_idx != vb.last_used_idx) {
            break;
        }
        spins++;
        if ((spins & 255) == 0) {
            io_wait();
        }
        if (spins > POLL_SPINS_MAX) {
            kprintf("[VIRTIO] blk request timeout (type=%u sector=%u)\n",
                    (u64)type, sector);
            free_desc_chain(d0);
            return 0;
        }
    }
    vb.last_used_idx = used_idx;
    (void)isr_read();               /* ack ISR */

    free_desc_chain(d0);

    if (*hhdm8(status_phys) != VIRTIO_BLK_S_OK) {
        kprintf("[VIRTIO] blk status=%u type=%u sector=%u\n",
                (u64)(*hhdm8(status_phys)), (u64)type, sector);
        return 0;
    }
    return 1;
}

u32 virtio_blk_present(void) {
    return vb.present;
}

u64 virtio_blk_capacity(void) {
    return vb.capacity;
}

/* Read `count` 512-byte sectors starting at `sector` into kernel buffer. */
u32 virtio_blk_read(u64 sector, void *kbuf, u32 count) {
    u32 i;
    u8 *dst = (u8 *)kbuf;
    if (!vb.present || count == 0) {
        return 0;
    }
    if (sector + (u64)count > vb.capacity) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        u64 bounce = vb.req_phys + 32;     /* after hdr+status in req page */
        u32 b;
        if (!blk_request(VIRTIO_BLK_T_IN, sector + (u64)i, bounce, SECTOR_SIZE)) {
            return 0;
        }
        for (b = 0; b < SECTOR_SIZE; b++) {
            dst[(u64)i * SECTOR_SIZE + b] = hhdm8(bounce)[b];
        }
    }
    return 1;
}

/* Write `count` 512-byte sectors from kernel buffer. */
u32 virtio_blk_write(u64 sector, const void *kbuf, u32 count) {
    u32 i;
    const u8 *src = (const u8 *)kbuf;
    if (!vb.present || count == 0) {
        return 0;
    }
    if (sector + (u64)count > vb.capacity) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        u64 bounce = vb.req_phys + 32;
        u32 b;
        for (b = 0; b < SECTOR_SIZE; b++) {
            hhdm8(bounce)[b] = src[(u64)i * SECTOR_SIZE + b];
        }
        if (!blk_request(VIRTIO_BLK_T_OUT, sector + (u64)i, bounce, SECTOR_SIZE)) {
            return 0;
        }
    }
    return 1;
}

u32 virtio_blk_flush(void) {
    /* No-op on legacy VirtIO-blk for now. Submitting VIRTIO_BLK_T_FLUSH under
     * QEMU TCG has wedged the used ring so later I/O never completes; write
     * ordering of prior OUT requests is still honored by the device. ATA
     * backend still issues a real FLUSH CACHE. */
    if (!vb.present) {
        return 0;
    }
    return 1;
}

static u32 try_init_legacy(u8 bus, u8 slot, u8 func) {
    u32 bar;
    u32 is_io;
    u32 lo;
    u32 hi;
    u16 dev_id;

    if (!pci_bar0(bus, slot, func, &bar, &is_io) || !is_io || bar == 0) {
        return 0;
    }

    pci_enable_busmaster(bus, slot, func);
    vb.modern = 0;
    vb.iobase = (u16)bar;
    vb.present = 0;

    iow8(REG_STATUS, 0);
    iow8(REG_STATUS, STATUS_ACKNOWLEDGE);
    iow8(REG_STATUS, (u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER));

    (void)ior32(REG_DEV_FEATURES);
    iow32(REG_DRV_FEATURES, 0);

    if (!setup_queue()) {
        iow8(REG_STATUS, STATUS_FAILED);
        kprintf("[VIRTIO] blk legacy queue setup failed\n");
        return 0;
    }

    lo = ior32(REG_CFG_CAPACITY_LO);
    hi = ior32(REG_CFG_CAPACITY_HI);
    vb.capacity = ((u64)hi << 32) | (u64)lo;

    iow8(REG_STATUS, (u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK));
    vb.present = 1;

    dev_id = pci_read16(bus, slot, func, 0x02);
    kprintf("[VIRTIO] blk legacy ready: pci %u:%u.%u io=0x%x id=0x%x capacity=%u sectors (%u KiB)\n",
            (u64)bus, (u64)slot, (u64)func, (u64)vb.iobase, (u64)dev_id,
            vb.capacity, vb.capacity / 2);
    return 1;
}

static u32 try_init_modern(u8 bus, u8 slot, u8 func) {
    u64 common = 0;
    u64 notify = 0;
    u64 isr = 0;
    u64 devcfg = 0;
    u32 clen = 0;
    u32 nlen = 0;
    u32 ilen = 0;
    u32 dlen = 0;
    u32 nmult = 0;
    u32 lo;
    u32 hi;
    u16 dev_id;

    if (!pci_virtio_map_cap(bus, slot, func, VIRTIO_PCI_CAP_COMMON_CFG,
                            &common, &clen, 0)
        || !pci_virtio_map_cap(bus, slot, func, VIRTIO_PCI_CAP_NOTIFY_CFG,
                               &notify, &nlen, &nmult)
        || !pci_virtio_map_cap(bus, slot, func, VIRTIO_PCI_CAP_ISR_CFG,
                               &isr, &ilen, 0)
        || !pci_virtio_map_cap(bus, slot, func, VIRTIO_PCI_CAP_DEVICE_CFG,
                               &devcfg, &dlen, 0)) {
        return 0;
    }
    if (common == 0 || notify == 0 || isr == 0 || devcfg == 0) {
        return 0;
    }

    pci_enable_busmaster(bus, slot, func);
    vb.modern = 1;
    vb.iobase = 0;
    vb.common_phys = common;
    vb.notify_phys = notify;
    vb.isr_phys = isr;
    vb.devcfg_phys = devcfg;
    vb.notify_mult = nmult != 0 ? nmult : 1;
    vb.present = 0;

    status_write(0);
    status_write(STATUS_ACKNOWLEDGE);
    status_write((u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER));

    /* Feature select 0/1: accept nothing optional (vanilla R/W). */
    *m32(vb.common_phys, M_DEV_FEAT_SEL) = 0;
    (void)*m32(vb.common_phys, M_DEV_FEAT);
    *m32(vb.common_phys, M_DRV_FEAT_SEL) = 0;
    *m32(vb.common_phys, M_DRV_FEAT) = 0;
    *m32(vb.common_phys, M_DEV_FEAT_SEL) = 1;
    (void)*m32(vb.common_phys, M_DEV_FEAT);
    *m32(vb.common_phys, M_DRV_FEAT_SEL) = 1;
    *m32(vb.common_phys, M_DRV_FEAT) = 0;
    status_write((u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK));
    if ((status_read() & STATUS_FEATURES_OK) == 0) {
        status_write(STATUS_FAILED);
        kprintf("[VIRTIO] blk modern FEATURES_OK rejected\n");
        return 0;
    }

    if (!setup_queue()) {
        status_write(STATUS_FAILED);
        kprintf("[VIRTIO] blk modern queue setup failed\n");
        return 0;
    }

    lo = *m32(vb.devcfg_phys, 0);
    hi = *m32(vb.devcfg_phys, 4);
    vb.capacity = ((u64)hi << 32) | (u64)lo;

    status_write((u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK
                      | STATUS_DRIVER_OK));
    /* Best-effort MSI on vector 40 (same as virtio-net INTx); still polled. */
    (void)pci_msi_enable(bus, slot, func, 40);
    vb.present = 1;

    dev_id = pci_read16(bus, slot, func, 0x02);
    kprintf("[VIRTIO] blk modern ready: pci %u:%u.%u id=0x%x capacity=%u sectors (%u KiB) notify_mult=%u\n",
            (u64)bus, (u64)slot, (u64)func, (u64)dev_id,
            vb.capacity, vb.capacity / 2, (u64)vb.notify_mult);
    (void)clen; (void)nlen; (void)ilen; (void)dlen;
    return 1;
}

static u32 try_init_at(u8 bus, u8 slot, u8 func) {
    /* Prefer legacy I/O BAR when present (disable-modern=on bring-up). */
    if (try_init_legacy(bus, slot, func)) {
        return 1;
    }
    if (try_init_modern(bus, slot, func)) {
        return 1;
    }
    kprintf("[VIRTIO] blk at %u:%u.%u: neither legacy I/O nor modern caps usable\n",
            (u64)bus, (u64)slot, (u64)func);
    return 0;
}

void virtio_blk_init(void) {
    u8 bus;
    u8 slot;
    u8 func;

    vb.present = 0;
    vb.modern = 0;
    vb.capacity = 0;
    vb.iobase = 0;

    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_DEV_BLK_LEGACY, &bus, &slot, &func)) {
        if (try_init_at(bus, slot, func)) {
            return;
        }
    }
    if (pci_find_vendor_subsystem(VIRTIO_VENDOR, VIRTIO_SUBSYS_BLK, &bus, &slot, &func)) {
        if (try_init_at(bus, slot, func)) {
            return;
        }
    }
    /* Modern-only device ids: 0x1040 + virtio device id (blk = 2 -> 0x1042). */
    if (pci_find_device(VIRTIO_VENDOR, 0x1042, &bus, &slot, &func)) {
        if (try_init_modern(bus, slot, func)) {
            return;
        }
    }
    kprintf("[VIRTIO] no virtio-blk device\n");
}
