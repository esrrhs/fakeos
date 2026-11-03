package drivers;
import arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);

/* ---------------------------------------------------------------------------
 * ATA PIO (primary bus, master) - polling, 28-bit LBA.
 *
 * Enough for QEMU `-drive file=disk.img,format=raw,if=ide` (or `-hda`).
 * Used as the reliable block backend while VirtIO-blk polling is shaken out;
 * Ext2 mounts whichever backend blk_init() selects.
 * ------------------------------------------------------------------------- */

enum {
    ATA_DATA        = 0x1F0,
    ATA_ERROR       = 0x1F1,
    ATA_SECCOUNT    = 0x1F2,
    ATA_LBA0        = 0x1F3,
    ATA_LBA1        = 0x1F4,
    ATA_LBA2        = 0x1F5,
    ATA_DRIVE       = 0x1F6,
    ATA_STATUS      = 0x1F7,
    ATA_CMD         = 0x1F7,
    ATA_ALTSTATUS   = 0x3F6,

    ATA_SR_ERR      = 0x01,
    ATA_SR_DRQ      = 0x08,
    ATA_SR_DF       = 0x20,
    ATA_SR_BSY      = 0x80,

    ATA_CMD_READ    = 0x20,
    ATA_CMD_WRITE   = 0x30,
    ATA_CMD_FLUSH   = 0xE7,
    ATA_CMD_IDENT   = 0xEC,

    SECTOR_SIZE     = 512
};

static u32 ata_present;
static u64 ata_nsectors;               /* sectors */

static void ata_wait_bsy_clear(void) {
    u32 spins = 0;
    while ((arch.inb(ATA_STATUS) & ATA_SR_BSY) != 0) {
        spins++;
        if (spins > 100000000) return;
    }
}

static u32 ata_wait_drq(void) {
    u32 spins = 0;
    while (1) {
        u8 st = arch.inb(ATA_STATUS);
        if (st & ATA_SR_ERR) return 0;
        if (st & ATA_SR_DF) return 0;
        if ((st & ATA_SR_BSY) == 0 && (st & ATA_SR_DRQ) != 0) return 1;
        spins++;
        if (spins > 100000000) return 0;
    }
}

static void ata_select_lba(u64 sector, u8 count) {
    arch.outb(ATA_DRIVE, (u8)(0xE0 | ((sector >> 24) & 0x0F)));
    arch.outb(ATA_SECCOUNT, count);
    arch.outb(ATA_LBA0, (u8)(sector & 0xFF));
    arch.outb(ATA_LBA1, (u8)((sector >> 8) & 0xFF));
    arch.outb(ATA_LBA2, (u8)((sector >> 16) & 0xFF));
}

u32 ata_present_p(void) {
    return ata_present;
}

u64 ata_capacity(void) {
    return ata_nsectors;
}

u32 ata_read(u64 sector, void *kbuf, u32 count) {
    u32 i;
    u16 *dst = (u16 *)kbuf;
    if (!ata_present || count == 0) return 0;
    if (sector + (u64)count > ata_nsectors) return 0;
    if (count > 255) return 0;

    ata_wait_bsy_clear();
    ata_select_lba(sector, (u8)count);
    arch.outb(ATA_CMD, ATA_CMD_READ);

    for (i = 0; i < count; i++) {
        u32 w;
        if (!ata_wait_drq()) return 0;
        for (w = 0; w < 256; w++) {
            dst[i * 256 + w] = arch.inw(ATA_DATA);
        }
    }
    return 1;
}

u32 ata_write(u64 sector, const void *kbuf, u32 count) {
    u32 i;
    const u16 *src = (const u16 *)kbuf;
    if (!ata_present || count == 0) return 0;
    if (sector + (u64)count > ata_nsectors) return 0;
    if (count > 255) return 0;

    ata_wait_bsy_clear();
    ata_select_lba(sector, (u8)count);
    arch.outb(ATA_CMD, ATA_CMD_WRITE);

    for (i = 0; i < count; i++) {
        u32 w;
        if (!ata_wait_drq()) return 0;
        for (w = 0; w < 256; w++) {
            arch.outw(ATA_DATA, src[i * 256 + w]);
        }
        /* Drain status after each sector. */
        ata_wait_bsy_clear();
    }
    return 1;
}

u32 ata_flush(void) {
    if (!ata_present) return 0;
    ata_wait_bsy_clear();
    arch.outb(ATA_DRIVE, 0xE0);
    arch.outb(ATA_CMD, ATA_CMD_FLUSH);
    ata_wait_bsy_clear();
    if (arch.inb(ATA_STATUS) & ATA_SR_ERR) return 0;
    return 1;
}

void ata_init(void) {
    u16 id[256];
    u32 w;
    u8 st;

    ata_present = 0;
    ata_nsectors = 0;

    /* Soft "is there a drive" probe via status; floating bus reads as 0xFF. */
    arch.outb(ATA_DRIVE, 0xE0);
    arch.io_wait();
    st = arch.inb(ATA_STATUS);
    if (st == 0xFF) {
        kprintf("[ATA] no drive on primary master\n");
        return;
    }

    ata_wait_bsy_clear();
    ata_select_lba(0, 0);
    arch.outb(ATA_CMD, ATA_CMD_IDENT);
    arch.io_wait();
    st = arch.inb(ATA_STATUS);
    if (st == 0 || (st & ATA_SR_ERR)) {
        kprintf("[ATA] IDENTIFY failed (status=0x%x)\n", (u64)st);
        return;
    }
    if (!ata_wait_drq()) {
        kprintf("[ATA] IDENTIFY no DRQ\n");
        return;
    }
    for (w = 0; w < 256; w++) {
        id[w] = arch.inw(ATA_DATA);
    }

    /* Words 60-61: total addressable sectors (LBA28). */
    ata_nsectors = (u64)id[60] | ((u64)id[61] << 16);
    if (ata_nsectors == 0) {
        kprintf("[ATA] IDENTIFY reported 0 sectors\n");
        return;
    }
    ata_present = 1;
    kprintf("[ATA] primary master ready: capacity=%u sectors (%u KiB)\n",
            ata_nsectors, ata_nsectors / 2);
}
