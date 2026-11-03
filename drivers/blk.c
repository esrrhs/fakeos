package drivers;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);
extern void virtio_blk_init(void);
extern u32 virtio_blk_present(void);
extern u64 virtio_blk_capacity(void);
extern u32 virtio_blk_read(u64 sector, void *kbuf, u32 count);
extern u32 virtio_blk_write(u64 sector, const void *kbuf, u32 count);
extern u32 virtio_blk_flush(void);
extern void ata_init(void);
extern u32 ata_present_p(void);
extern u64 ata_capacity(void);
extern u32 ata_read(u64 sector, void *kbuf, u32 count);
extern u32 ata_write(u64 sector, const void *kbuf, u32 count);
extern u32 ata_flush(void);
extern u32 pci_find_class(u8 base_class, u8 subclass, u8 *obus, u8 *oslot, u8 *ofunc);
extern u32 pci_bar0(u8 bus, u8 slot, u8 func, u32 *addr, u32 *is_io);

enum {
    BLK_NONE = 0,
    BLK_VIRTIO = 1,
    BLK_ATA = 2
};

static u32 blk_kind;

void blk_init(void) {
    blk_kind = BLK_NONE;
    /* Probe-only: log AHCI if QEMU exposed one (no driver yet). */
    {
        u8 bus = 0;
        u8 slot = 0;
        u8 func = 0;
        if (pci_find_class(0x01, 0x06, &bus, &slot, &func)) {
            u32 bar = 0;
            u32 io = 0;
            pci_bar0(bus, slot, func, &bar, &io);
            kprintf("[BLK] AHCI PCI 01.06 at %u:%u.%u BAR0=0x%x (%s) — not driven yet (see docs/blk-modern-path.md)\n",
                    (u64)bus, (u64)slot, (u64)func, (u64)bar,
                    io ? "I/O" : "MMIO");
        } else {
            kprintf("[BLK] no AHCI (PCI 01.06); using legacy virtio-blk/ATA\n");
        }
    }
    /* Prefer VirtIO when the legacy queue path is healthy; otherwise ATA. */
    virtio_blk_init();
    if (virtio_blk_present()) {
        /* Probe one sector. If the queue path is broken, fall through to ATA. */
        u8 probe[512];
        u32 i;
        for (i = 0; i < 512; i++) probe[i] = 0;
        if (virtio_blk_read(0, probe, 1)) {
            blk_kind = BLK_VIRTIO;
            kprintf("[BLK] using VirtIO-blk backend\n");
            return;
        }
        kprintf("[BLK] VirtIO-blk present but I/O failed; trying ATA\n");
    }
    ata_init();
    if (ata_present_p()) {
        blk_kind = BLK_ATA;
        kprintf("[BLK] using ATA PIO backend\n");
        return;
    }
    kprintf("[BLK] no block device (persistence offline)\n");
}

u32 blk_present(void) {
    return blk_kind != BLK_NONE;
}

u64 blk_capacity(void) {
    if (blk_kind == BLK_VIRTIO) return virtio_blk_capacity();
    if (blk_kind == BLK_ATA) return ata_capacity();
    return 0;
}

u32 blk_read(u64 sector, void *kbuf, u32 count) {
    if (blk_kind == BLK_VIRTIO) return virtio_blk_read(sector, kbuf, count);
    if (blk_kind == BLK_ATA) return ata_read(sector, kbuf, count);
    return 0;
}

u32 blk_write(u64 sector, const void *kbuf, u32 count) {
    if (blk_kind == BLK_VIRTIO) return virtio_blk_write(sector, kbuf, count);
    if (blk_kind == BLK_ATA) return ata_write(sector, kbuf, count);
    return 0;
}

u32 blk_flush(void) {
    if (blk_kind == BLK_VIRTIO) return virtio_blk_flush();
    if (blk_kind == BLK_ATA) return ata_flush();
    return 0;
}
