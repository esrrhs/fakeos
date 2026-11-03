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
extern u8 pci_read8(u8 bus, u8 slot, u8 func, u8 offset);
extern u32 pci_virtio_map_cap(u8 bus, u8 slot, u8 func, u8 cfg_type,
                              u64 *mmio_phys, u32 *len, u32 *notify_mult);
extern u32 pci_msi_enable(u8 bus, u8 slot, u8 func, u8 vector);
extern u32 pci_msix_enable_one(u8 bus, u8 slot, u8 func, u8 vector);
extern void ioapic_route_irq_level(u32 irq, u32 vector);

/* ---------------------------------------------------------------------------
 * VirtIO-net PCI driver - legacy I/O BAR + modern MMIO, IRQ RX + polled TX.
 *
 * Default `make qemu` keeps disable-modern=on (legacy BAR + INTx). When the
 * device exposes modern vendor caps (or `make qemu-modern` / disable-legacy),
 * the modern path maps common/notify/ISR/device cfg and programs both RX/TX
 * queues. MSI-X (preferred) or MSI on vector 40 drives RX; INTx remains the
 * fallback. Idle net_poll is still a backup drain.
 * ------------------------------------------------------------------------- */

static const u64 HHDM_BASE = 0xFFFF800000000000ULL;

enum {
    VIRTIO_VENDOR         = 0x1AF4,
    VIRTIO_DEV_NET_LEGACY = 0x1000,
    VIRTIO_SUBSYS_NET     = 0x0001,
    VIRTIO_DEV_NET_MODERN = 0x1041, /* 0x1040 + virtio net id 1 */

    REG_DEV_FEATURES      = 0,
    REG_DRV_FEATURES      = 4,
    REG_QUEUE_PFN         = 8,
    REG_QUEUE_NUM         = 12,
    REG_QUEUE_SEL         = 14,
    REG_QUEUE_NOTIFY      = 16,
    REG_STATUS            = 18,
    REG_ISR               = 19,
    REG_CFG_MAC           = 20,   /* 6 bytes (legacy) */
    REG_CFG_STATUS        = 26,

    STATUS_ACKNOWLEDGE    = 1,
    STATUS_DRIVER         = 2,
    STATUS_DRIVER_OK      = 4,
    STATUS_FEATURES_OK    = 8,
    STATUS_FAILED         = 128,

    /* Modern common-cfg offsets (VirtIO 1.0 §4.1.4.3). */
    M_DEV_FEAT_SEL        = 0,
    M_DEV_FEAT            = 4,
    M_DRV_FEAT_SEL        = 8,
    M_DRV_FEAT            = 12,
    M_MSIX_CONFIG         = 16,
    M_NUM_QUEUES          = 18,
    M_STATUS              = 20,
    M_CFG_GEN             = 21,
    M_Q_SEL               = 22,
    M_Q_SIZE              = 24,
    M_Q_MSIX              = 26,
    M_Q_ENABLE            = 28,
    M_Q_NOTIFY_OFF        = 30,
    M_Q_DESC              = 32,
    M_Q_DRIVER            = 40,
    M_Q_DEVICE            = 48,

    VIRTIO_PCI_CAP_COMMON_CFG = 1,
    VIRTIO_PCI_CAP_NOTIFY_CFG = 2,
    VIRTIO_PCI_CAP_ISR_CFG    = 3,
    VIRTIO_PCI_CAP_DEVICE_CFG = 4,

    VRING_DESC_F_NEXT     = 1,
    VRING_DESC_F_WRITE    = 2,

    VIRTIO_NET_F_MAC      = (1u << 5),
    VIRTIO_F_VERSION_1    = (1u << 0), /* feature dword 1 bit 0 = bit 32 */

    VIRTIO_MSI_NO_VECTOR  = 0xFFFF,

    NET_HDR_SIZE          = 10,
    ETH_MAX               = 1514,
    PKT_BUF_SIZE          = 2048,
    RX_BUFS               = 16,
    QUEUE_MAX             = 1024,
    VNET_IRQ_VECTOR       = 40,
    PCI_INTERRUPT_LINE    = 0x3C,
    ARENA_ORDER           = 6,
    ARENA_BYTES           = 262144
};

struct virtq {
    u16 size;
    u64 desc_phys;
    u64 avail_phys;
    u64 used_phys;
    u16 free_head;
    u16 last_used_idx;
    u16 notify_off;           /* modern: queue_notify_off */
};

struct virtio_net {
    u32 present;
    u32 modern;
    u32 msi;                  /* 1 = MSI/MSI-X armed (no INTx needed) */
    u16 iobase;
    u8  mac[6];
    u8  pci_irq;
    u32 irq_armed;
    struct virtq rx;
    struct virtq tx;
    u64 arena_phys;
    u64 rx_buf_phys;
    u64 tx_buf_phys;
    u16 rx_desc[RX_BUFS];
    u64 common_phys;
    u64 notify_phys;
    u64 isr_phys;
    u64 devcfg_phys;
    u32 notify_mult;
};

static volatile u64 vnet_irq_count;
static struct virtio_net vn;

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

static void iow8(u16 off, u8 v) { arch.outb((u16)(vn.iobase + off), v); }
static u8  ior8(u16 off) { return arch.inb((u16)(vn.iobase + off)); }
static void iow16(u16 off, u16 v) { arch.outw((u16)(vn.iobase + off), v); }
static u16 ior16(u16 off) { return arch.inw((u16)(vn.iobase + off)); }
static void iow32(u16 off, u32 v) { arch.outl((u16)(vn.iobase + off), v); }
static u32 ior32(u16 off) { return arch.inl((u16)(vn.iobase + off)); }

static void status_write(u8 v) {
    if (vn.modern) {
        *m8(vn.common_phys, M_STATUS) = v;
    } else {
        iow8(REG_STATUS, v);
    }
}
static u8 status_read(void) {
    if (vn.modern) {
        return *m8(vn.common_phys, M_STATUS);
    }
    return ior8(REG_STATUS);
}
static u8 isr_read(void) {
    if (vn.modern) {
        return *m8(vn.isr_phys, 0);
    }
    return ior8(REG_ISR);
}
static void queue_notify(u16 qidx) {
    if (vn.modern) {
        u16 noff = (qidx == 0) ? vn.rx.notify_off : vn.tx.notify_off;
        u32 off = (u32)noff * vn.notify_mult;
        *m16(vn.notify_phys, off) = qidx;
    } else {
        iow16(REG_QUEUE_NOTIFY, qidx);
    }
}

static u64 align_up(u64 n, u64 a) {
    return (n + a - 1) & ~(a - 1);
}

static void desc_write(struct virtq *q, u16 idx, u64 addr, u32 len, u16 flags, u16 next) {
    u64 p = q->desc_phys + ((u64)idx) * 16;
    *hhdm64(p + 0) = addr;
    *hhdm32(p + 8) = len;
    *hhdm16(p + 12) = flags;
    *hhdm16(p + 14) = next;
}

static u16 alloc_desc(struct virtq *q) {
    u16 i = q->free_head;
    if (i == 0xFFFF) {
        return 0xFFFF;
    }
    q->free_head = *hhdm16(q->desc_phys + ((u64)i) * 16 + 14);
    return i;
}

static void free_desc(struct virtq *q, u16 i) {
    *hhdm16(q->desc_phys + ((u64)i) * 16 + 14) = q->free_head;
    q->free_head = i;
}

static void free_desc_chain(struct virtq *q, u16 head) {
    u16 i = head;
    while (1) {
        u16 flags = *hhdm16(q->desc_phys + ((u64)i) * 16 + 12);
        u16 next = *hhdm16(q->desc_phys + ((u64)i) * 16 + 14);
        free_desc(q, i);
        if ((flags & VRING_DESC_F_NEXT) == 0) {
            break;
        }
        i = next;
    }
}

static u32 setup_one_queue(struct virtq *q, u16 sel, u64 *cursor) {
    u16 qsz;
    u64 desc_sz;
    u64 avail_sz;
    u64 used_off;
    u64 used_sz;
    u16 i;
    u64 base;

    if (vn.modern) {
        *m16(vn.common_phys, M_Q_SEL) = sel;
        qsz = *m16(vn.common_phys, M_Q_SIZE);
        if (qsz == 0 || qsz > QUEUE_MAX) {
            qsz = 256;
            *m16(vn.common_phys, M_Q_SIZE) = qsz;
        }
    } else {
        iow16(REG_QUEUE_SEL, sel);
        qsz = ior16(REG_QUEUE_NUM);
        if (qsz == 0 || qsz > QUEUE_MAX) {
            kprintf("[VNET] queue%u size %u unsupported\n", (u64)sel, (u64)qsz);
            return 0;
        }
    }
    q->size = qsz;
    desc_sz = (u64)qsz * 16;
    avail_sz = 6 + (u64)qsz * 2;
    used_off = align_up(desc_sz + avail_sz, 4096);
    used_sz = 6 + (u64)qsz * 8;

    base = align_up(*cursor, 4096);
    q->desc_phys = base;
    q->avail_phys = base + desc_sz;
    q->used_phys = base + used_off;
    *cursor = base + used_off + align_up(used_sz, 4096);

    for (i = 0; i < qsz - 1; i++) {
        desc_write(q, i, 0, 0, 0, (u16)(i + 1));
    }
    desc_write(q, (u16)(qsz - 1), 0, 0, 0, 0xFFFF);
    q->free_head = 0;
    q->last_used_idx = 0;
    q->notify_off = 0;

    if (vn.modern) {
        *m16(vn.common_phys, M_Q_SEL) = sel;
        *m16(vn.common_phys, M_Q_SIZE) = qsz;
        *m64(vn.common_phys, M_Q_DESC) = q->desc_phys;
        *m64(vn.common_phys, M_Q_DRIVER) = q->avail_phys;
        *m64(vn.common_phys, M_Q_DEVICE) = q->used_phys;
        q->notify_off = *m16(vn.common_phys, M_Q_NOTIFY_OFF);
        /* MSI-X table entry 0 for both queues when MSI-X is armed. */
        if (vn.msi) {
            *m16(vn.common_phys, M_Q_MSIX) = 0;
        }
        *m16(vn.common_phys, M_Q_ENABLE) = 1;
    } else {
        iow32(REG_QUEUE_PFN, (u32)(base / 4096));
    }
    kprintf("[VNET] queue%u size=%u desc=%u modern=%u\n",
            (u64)sel, (u64)qsz, base, (u64)vn.modern);
    return 1;
}

static void rx_post(u16 slot) {
    u16 d;
    u64 buf = vn.rx_buf_phys + ((u64)slot) * PKT_BUF_SIZE;
    d = alloc_desc(&vn.rx);
    if (d == 0xFFFF) {
        return;
    }
    vn.rx_desc[slot] = d;
    desc_write(&vn.rx, d, buf, PKT_BUF_SIZE, VRING_DESC_F_WRITE, 0);
    {
        u16 avail_idx = *hhdm16(vn.rx.avail_phys + 2);
        *hhdm16(vn.rx.avail_phys + 4 + ((u64)(avail_idx % vn.rx.size)) * 2) = d;
        (void)isr_read();
        *hhdm16(vn.rx.avail_phys + 2) = (u16)(avail_idx + 1);
        (void)isr_read();
    }
    queue_notify(0);
}

static void rx_fill(void) {
    u16 i;
    for (i = 0; i < RX_BUFS; i++) {
        rx_post(i);
    }
}

u32 virtio_net_present(void) {
    return vn.present;
}

void virtio_net_mac(u8 *out) {
    u32 i;
    for (i = 0; i < 6; i++) {
        out[i] = vn.mac[i];
    }
}

u32 virtio_net_tx(const void *frame, u32 len) {
    u16 d0;
    u16 avail_idx;
    u16 used_idx;
    u32 spins;
    u32 i;
    const u8 *src;
    u64 buf;

    if (!vn.present || frame == 0 || len == 0 || len > ETH_MAX) {
        return 0;
    }
    buf = vn.tx_buf_phys;
    for (i = 0; i < NET_HDR_SIZE; i++) {
        hhdm8(buf)[i] = 0;
    }
    src = (const u8 *)frame;
    for (i = 0; i < len; i++) {
        hhdm8(buf)[NET_HDR_SIZE + i] = src[i];
    }

    d0 = alloc_desc(&vn.tx);
    if (d0 == 0xFFFF) {
        return 0;
    }
    desc_write(&vn.tx, d0, buf, NET_HDR_SIZE + len, 0, 0);

    avail_idx = *hhdm16(vn.tx.avail_phys + 2);
    *hhdm16(vn.tx.avail_phys + 4 + ((u64)(avail_idx % vn.tx.size)) * 2) = d0;
    (void)isr_read();
    *hhdm16(vn.tx.avail_phys + 2) = (u16)(avail_idx + 1);
    (void)isr_read();
    queue_notify(1);

    spins = 0;
    while (1) {
        used_idx = *hhdm16(vn.tx.used_phys + 2);
        if (used_idx != vn.tx.last_used_idx) {
            break;
        }
        spins++;
        if (spins > 2000000) {
            kprintf("[VNET] tx timeout\n");
            free_desc(&vn.tx, d0);
            return 0;
        }
    }
    vn.tx.last_used_idx = used_idx;
    (void)isr_read();
    free_desc(&vn.tx, d0);
    return 1;
}

u32 virtio_net_rx(void *frame, u32 maxlen) {
    u16 used_idx;
    u16 ring_i;
    u32 id;
    u32 len;
    u64 elem;
    u32 i;
    u64 buf;
    u16 slot;
    u8 *dst;

    if (!vn.present || frame == 0 || maxlen == 0) {
        return 0;
    }
    used_idx = *hhdm16(vn.rx.used_phys + 2);
    if (used_idx == vn.rx.last_used_idx) {
        return 0;
    }
    ring_i = (u16)(vn.rx.last_used_idx % vn.rx.size);
    elem = vn.rx.used_phys + 4 + ((u64)ring_i) * 8;
    id = *hhdm32(elem + 0);
    len = *hhdm32(elem + 4);
    vn.rx.last_used_idx = (u16)(vn.rx.last_used_idx + 1);
    (void)isr_read();

    slot = 0xFFFF;
    for (i = 0; i < RX_BUFS; i++) {
        if (vn.rx_desc[i] == (u16)id) {
            slot = (u16)i;
            break;
        }
    }
    if (slot == 0xFFFF || len <= NET_HDR_SIZE) {
        if (slot != 0xFFFF) {
            free_desc(&vn.rx, (u16)id);
            rx_post(slot);
        }
        return 0;
    }
    buf = vn.rx_buf_phys + ((u64)slot) * PKT_BUF_SIZE;
    len = len - NET_HDR_SIZE;
    if (len > maxlen) {
        len = maxlen;
    }
    if (len > ETH_MAX) {
        len = ETH_MAX;
    }
    dst = (u8 *)frame;
    for (i = 0; i < len; i++) {
        dst[i] = hhdm8(buf)[NET_HDR_SIZE + i];
    }
    free_desc(&vn.rx, (u16)id);
    rx_post(slot);
    return len;
}

/* Ack device interrupt. MSI/MSI-X deliveries are edge and need no ISR ack;
 * INTx / legacy ISR is read-to-clear. */
u32 virtio_net_irq(void) {
    if (!vn.present) {
        return 0;
    }
    if (vn.msi) {
        vnet_irq_count++;
        return 1;
    }
    {
        u8 isr = isr_read();
        if (isr == 0) {
            return 0;
        }
        vnet_irq_count++;
        return 1;
    }
}

u64 virtio_net_irq_count_total(void) {
    return vnet_irq_count;
}

static void arm_irq(u8 bus, u8 slot, u8 func) {
    vn.msi = 0;
    vn.irq_armed = 0;
    /* Prefer MSI-X, then MSI; both land on VNET_IRQ_VECTOR. */
    /* MSI first (config-space only). MSI-X needs a BAR inside boot HHDM. */
    if (pci_msi_enable(bus, slot, func, VNET_IRQ_VECTOR)
        || pci_msix_enable_one(bus, slot, func, VNET_IRQ_VECTOR)) {
        vn.msi = 1;
        vn.irq_armed = 1;
        kprintf("[VNET] MSI/MSI-X RX armed (vector %u)\n", (u64)VNET_IRQ_VECTOR);
        return;
    }
    vn.pci_irq = pci_read8(bus, slot, func, PCI_INTERRUPT_LINE);
    if (vn.pci_irq != 0 && vn.pci_irq != 0xFF) {
        ioapic_route_irq_level((u32)vn.pci_irq, VNET_IRQ_VECTOR);
        vn.irq_armed = 1;
        kprintf("[VNET] IRQ-driven RX armed (PCI INTx -> vector %u)\n",
                (u64)VNET_IRQ_VECTOR);
    } else {
        kprintf("[VNET] no PCI IRQ line; RX remains poll-only\n");
    }
}

static u32 finish_queues_and_mac(u8 bus, u8 slot, u8 func) {
    u64 cursor;
    u32 i;
    u16 dev_id;

    /* Arm MSI-X/MSI before queue enable so M_Q_MSIX can be programmed. */
    arm_irq(bus, slot, func);

    vn.arena_phys = mem.pmm_alloc_pages(ARENA_ORDER);
    if (vn.arena_phys == 0) {
        status_write(STATUS_FAILED);
        kprintf("[VNET] arena alloc failed\n");
        return 0;
    }
    for (i = 0; i < (u32)ARENA_BYTES; i++) {
        hhdm8(vn.arena_phys)[i] = 0;
    }
    cursor = vn.arena_phys;

    if (!setup_one_queue(&vn.rx, 0, &cursor)) {
        status_write(STATUS_FAILED);
        return 0;
    }
    if (!setup_one_queue(&vn.tx, 1, &cursor)) {
        status_write(STATUS_FAILED);
        return 0;
    }

    cursor = align_up(cursor, 4096);
    vn.rx_buf_phys = cursor;
    cursor += (u64)RX_BUFS * PKT_BUF_SIZE;
    vn.tx_buf_phys = align_up(cursor, 4096);
    if (vn.tx_buf_phys + PKT_BUF_SIZE > vn.arena_phys + ARENA_BYTES) {
        status_write(STATUS_FAILED);
        kprintf("[VNET] arena too small for RX/TX buffers\n");
        return 0;
    }

    if (vn.modern) {
        status_write((u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK
                          | STATUS_DRIVER_OK));
    } else {
        status_write((u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK));
    }
    vn.present = 1;
    rx_fill();

    if (vn.modern && vn.msi) {
        *m16(vn.common_phys, M_MSIX_CONFIG) = (u16)VIRTIO_MSI_NO_VECTOR;
    }

    dev_id = pci_read16(bus, slot, func, 0x02);
    kprintf("[VNET] ready: pci %u:%u.%u io=0x%x id=0x%x modern=%u msi=%u irq=%u vec=%u mac=%x:%x:%x:%x:%x:%x\n",
            (u64)bus, (u64)slot, (u64)func, (u64)vn.iobase, (u64)dev_id,
            (u64)vn.modern, (u64)vn.msi, (u64)vn.pci_irq, (u64)VNET_IRQ_VECTOR,
            (u64)vn.mac[0], (u64)vn.mac[1], (u64)vn.mac[2],
            (u64)vn.mac[3], (u64)vn.mac[4], (u64)vn.mac[5]);
    return 1;
}

static u32 try_init_legacy(u8 bus, u8 slot, u8 func) {
    u32 bar;
    u32 is_io;
    u32 feats;
    u32 drv;
    u32 i;

    if (!pci_bar0(bus, slot, func, &bar, &is_io) || !is_io || bar == 0) {
        return 0;
    }
    pci_enable_busmaster(bus, slot, func);
    vn.modern = 0;
    vn.iobase = (u16)bar;
    vn.present = 0;

    iow8(REG_STATUS, 0);
    iow8(REG_STATUS, STATUS_ACKNOWLEDGE);
    iow8(REG_STATUS, (u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER));

    feats = ior32(REG_DEV_FEATURES);
    drv = 0;
    if (feats & VIRTIO_NET_F_MAC) {
        drv |= VIRTIO_NET_F_MAC;
    }
    iow32(REG_DRV_FEATURES, drv);

    for (i = 0; i < 6; i++) {
        vn.mac[i] = ior8((u16)(REG_CFG_MAC + i));
    }
    return finish_queues_and_mac(bus, slot, func);
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
    u32 feats0;
    u32 feats1;
    u32 drv0;
    u32 drv1;
    u32 i;

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
    vn.modern = 1;
    vn.iobase = 0;
    vn.common_phys = common;
    vn.notify_phys = notify;
    vn.isr_phys = isr;
    vn.devcfg_phys = devcfg;
    vn.notify_mult = nmult != 0 ? nmult : 1;
    vn.present = 0;

    status_write(0);
    status_write(STATUS_ACKNOWLEDGE);
    status_write((u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER));

    *m32(vn.common_phys, M_DEV_FEAT_SEL) = 0;
    feats0 = *m32(vn.common_phys, M_DEV_FEAT);
    drv0 = 0;
    if (feats0 & VIRTIO_NET_F_MAC) {
        drv0 |= VIRTIO_NET_F_MAC;
    }
    *m32(vn.common_phys, M_DRV_FEAT_SEL) = 0;
    *m32(vn.common_phys, M_DRV_FEAT) = drv0;

    *m32(vn.common_phys, M_DEV_FEAT_SEL) = 1;
    feats1 = *m32(vn.common_phys, M_DEV_FEAT);
    drv1 = feats1 & VIRTIO_F_VERSION_1;
    *m32(vn.common_phys, M_DRV_FEAT_SEL) = 1;
    *m32(vn.common_phys, M_DRV_FEAT) = drv1;

    status_write((u8)(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK));
    if ((status_read() & STATUS_FEATURES_OK) == 0) {
        status_write(STATUS_FAILED);
        kprintf("[VNET] modern FEATURES_OK rejected\n");
        return 0;
    }

    for (i = 0; i < 6; i++) {
        vn.mac[i] = *m8(vn.devcfg_phys, i);
    }
    (void)clen; (void)nlen; (void)ilen; (void)dlen;
    return finish_queues_and_mac(bus, slot, func);
}

static u32 try_init_at(u8 bus, u8 slot, u8 func) {
    if (try_init_legacy(bus, slot, func)) {
        return 1;
    }
    if (try_init_modern(bus, slot, func)) {
        return 1;
    }
    kprintf("[VNET] at %u:%u.%u: neither legacy I/O nor modern caps usable\n",
            (u64)bus, (u64)slot, (u64)func);
    return 0;
}

void virtio_net_init(void) {
    u8 bus;
    u8 slot;
    u8 func;
    u32 i;

    vn.present = 0;
    vn.modern = 0;
    vn.msi = 0;
    vn.iobase = 0;
    vn.pci_irq = 0;
    vn.irq_armed = 0;
    vnet_irq_count = 0;
    for (i = 0; i < 6; i++) {
        vn.mac[i] = 0;
    }
    for (i = 0; i < RX_BUFS; i++) {
        vn.rx_desc[i] = 0xFFFF;
    }

    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_DEV_NET_LEGACY, &bus, &slot, &func)) {
        if (try_init_at(bus, slot, func)) {
            return;
        }
    }
    if (pci_find_vendor_subsystem(VIRTIO_VENDOR, VIRTIO_SUBSYS_NET, &bus, &slot, &func)) {
        if (try_init_at(bus, slot, func)) {
            return;
        }
    }
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_DEV_NET_MODERN, &bus, &slot, &func)) {
        if (try_init_modern(bus, slot, func)) {
            return;
        }
    }
    kprintf("[VNET] no virtio-net device\n");
}
