package drivers;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* end_io(private, ok): ok=1 success, 0 failure. */
typedef void (*blk_end_io_t)(void *private, u32 ok);

extern void kprintf(const char *fmt, ...);
extern void virtio_blk_init(void);
extern u32 virtio_blk_present(void);
extern u64 virtio_blk_capacity(void);
extern u32 virtio_blk_read(u64 sector, void *kbuf, u32 count);
extern u32 virtio_blk_write(u64 sector, const void *kbuf, u32 count);
extern u32 virtio_blk_flush(void);
extern void ahci_init(void);
extern u32 ahci_present_p(void);
extern u64 ahci_capacity(void);
extern u32 ahci_read(u64 sector, void *kbuf, u32 count);
extern u32 ahci_write(u64 sector, const void *kbuf, u32 count);
extern u32 ahci_flush(void);
extern u32 ahci_async_rw(u64 sector, void *kbuf, u32 count, u32 write,
                         blk_end_io_t end_io, void *private);
extern void ahci_async_poll(void);
extern void ata_init(void);
extern u32 ata_present_p(void);
extern u64 ata_capacity(void);
extern u32 ata_read(u64 sector, void *kbuf, u32 count);
extern u32 ata_write(u64 sector, const void *kbuf, u32 count);
extern u32 ata_flush(void);
extern u32 pci_find_class(u8 base_class, u8 subclass, u8 *obus, u8 *oslot, u8 *ofunc);
extern u32 pci_find_vendor_subsystem(u16 vendor, u16 subsystem,
                                     u8 *obus, u8 *oslot, u8 *ofunc);
extern void pci_log_virtio_modern_caps(u8 bus, u8 slot, u8 func);

enum {
    BLK_NONE = 0,
    BLK_VIRTIO = 1,
    BLK_AHCI = 2,
    BLK_ATA = 3
};

static u32 blk_kind;

void blk_init(void) {
    blk_kind = BLK_NONE;
    /* Log modern virtio caps when transitional device is present. */
    {
        u8 bus = 0;
        u8 slot = 0;
        u8 func = 0;
        if (pci_find_vendor_subsystem(0x1AF4, 0x0002, &bus, &slot, &func)) {
            pci_log_virtio_modern_caps(bus, slot, func);
        }
    }
    {
        u8 bus = 0;
        u8 slot = 0;
        u8 func = 0;
        if (!pci_find_class(0x01, 0x06, &bus, &slot, &func)) {
            kprintf("[BLK] no AHCI (PCI 01.06); using legacy virtio-blk/ATA\n");
        }
    }
    /* Prefer VirtIO when the legacy queue path is healthy; else AHCI; else ATA. */
    virtio_blk_init();
    if (virtio_blk_present()) {
        u8 probe[512];
        u32 i;
        for (i = 0; i < 512; i++) probe[i] = 0;
        if (virtio_blk_read(0, probe, 1)) {
            blk_kind = BLK_VIRTIO;
            kprintf("[BLK] using VirtIO-blk backend\n");
            /* Still probe AHCI so qemu-ahci / dual-disk boots log readiness. */
            ahci_init();
            return;
        }
        kprintf("[BLK] VirtIO-blk present but I/O failed; trying AHCI/ATA\n");
    }
    ahci_init();
    if (ahci_present_p()) {
        u8 probe[512];
        u32 i;
        for (i = 0; i < 512; i++) probe[i] = 0;
        if (ahci_read(0, probe, 1)) {
            blk_kind = BLK_AHCI;
            kprintf("[BLK] using AHCI backend\n");
            return;
        }
        kprintf("[AHCI] present but sector-0 read failed; trying ATA\n");
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
    if (blk_kind == BLK_AHCI) return ahci_capacity();
    if (blk_kind == BLK_ATA) return ata_capacity();
    return 0;
}

u32 blk_read(u64 sector, void *kbuf, u32 count) {
    if (blk_kind == BLK_VIRTIO) return virtio_blk_read(sector, kbuf, count);
    if (blk_kind == BLK_AHCI) return ahci_read(sector, kbuf, count);
    if (blk_kind == BLK_ATA) return ata_read(sector, kbuf, count);
    return 0;
}

u32 blk_write(u64 sector, const void *kbuf, u32 count) {
    if (blk_kind == BLK_VIRTIO) return virtio_blk_write(sector, kbuf, count);
    if (blk_kind == BLK_AHCI) return ahci_write(sector, kbuf, count);
    if (blk_kind == BLK_ATA) return ata_write(sector, kbuf, count);
    return 0;
}

u32 blk_flush(void) {
    if (blk_kind == BLK_VIRTIO) return virtio_blk_flush();
    if (blk_kind == BLK_AHCI) return ahci_flush();
    if (blk_kind == BLK_ATA) return ata_flush();
    return 0;
}

/* Non-blocking R/W. AHCI queues + end_io; virtio/ATA complete inline then cb.
 * Buffer must remain valid until end_io(private, ok) runs. Returns 1 if
 * accepted (queued or sync-completed). */
u32 blk_async_rw(u64 sector, void *kbuf, u32 count, u32 write,
                 blk_end_io_t end_io, void *private) {
    u32 ok;
    if (end_io == 0 || count == 0 || kbuf == 0) {
        return 0;
    }
    if (blk_kind == BLK_AHCI) {
        return ahci_async_rw(sector, kbuf, count, write, end_io, private);
    }
    if (write) {
        ok = blk_write(sector, kbuf, count);
    } else {
        ok = blk_read(sector, kbuf, count);
    }
    end_io(private, ok);
    return ok ? 1 : 0;
}

void blk_async_poll(void) {
    if (blk_kind == BLK_AHCI) {
        ahci_async_poll();
    }
}

/* Batch async writes for Ext2 journal meta-copy + data-path. Callback lives
 * in drivers so callers need no cross-package end_io typedef. Buffers must
 * stay valid until return. Polls to completion (pre-sched safe). Max 8;
 * submits in waves of 4 so AHCI NCQ depth is not exceeded. */
enum {
    BLK_AIO_BATCH_MAX = 8,
    BLK_AIO_WAVE = 4,
    BLK_AIO_POLL_SPINS = 5000000
};

static u32 blk_aio_done;
static u32 blk_aio_ok_n;

static void blk_aio_cb(void *private, u32 ok) {
    (void)private;
    if (ok) blk_aio_ok_n = blk_aio_ok_n + 1;
    blk_aio_done = blk_aio_done + 1;
}

u32 blk_async_write_batch(u64 *sectors, u8 *bufs, u32 buf_bytes,
                          u32 count, u32 n) {
    u32 off;
    if (n == 0) return 1;
    if (sectors == 0 || bufs == 0 || buf_bytes == 0 || count == 0) return 0;
    if (n > BLK_AIO_BATCH_MAX) return 0;
    off = 0;
    while (off < n) {
        u32 wave = n - off;
        u32 i;
        u32 spins;
        u32 accepted;
        if (wave > BLK_AIO_WAVE) wave = BLK_AIO_WAVE;
        blk_aio_done = 0;
        blk_aio_ok_n = 0;
        accepted = 0;
        for (i = 0; i < wave; i++) {
            u8 *kbuf = bufs + (u64)(off + i) * (u64)buf_bytes;
            if (!blk_async_rw(sectors[off + i], kbuf, count, 1, blk_aio_cb, 0)) {
                spins = 0;
                while (blk_aio_done < accepted && spins < BLK_AIO_POLL_SPINS) {
                    blk_async_poll();
                    spins = spins + 1;
                }
                return 0;
            }
            accepted = accepted + 1;
        }
        spins = 0;
        while (blk_aio_done < wave && spins < BLK_AIO_POLL_SPINS) {
            blk_async_poll();
            spins = spins + 1;
        }
        if (blk_aio_done != wave || blk_aio_ok_n != wave) return 0;
        off = off + wave;
    }
    return 1;
}
