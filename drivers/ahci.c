package drivers;
import arch;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);
extern u32 pci_find_class(u8 base_class, u8 subclass, u8 *obus, u8 *oslot, u8 *ofunc);
extern u32 pci_bar(u8 bus, u8 slot, u8 func, u8 index, u32 *addr, u32 *is_io);
extern void pci_enable_busmaster(u8 bus, u8 slot, u8 func);
extern void pci_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value);
extern u16 pci_read16(u8 bus, u8 slot, u8 func, u8 offset);
extern void pci_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 value);
extern u8 pci_read8(u8 bus, u8 slot, u8 func, u8 offset);
extern u32 pci_msi_enable(u8 bus, u8 slot, u8 func, u8 vector);
extern u32 pci_msix_enable_one(u8 bus, u8 slot, u8 func, u8 vector);
extern void ioapic_route_irq_level(u32 irq, u32 vector);
extern void cpu_pause(void);
extern void sched_sleep(u64 chan);
extern void sched_wakeup(u64 chan);
extern u32 sched_is_active(void);
extern void spin_lock(volatile u32 *lock);
extern void spin_unlock(volatile u32 *lock);

/* ---------------------------------------------------------------------------
 * AHCI (SATA) driver: one port, multi-slot command list + cross-caller bio
 * queue.
 *
 * Target: QEMU `-device ahci,id=ahci0 -device ide-hd,drive=...,bus=ahci0.0`
 * with PCI class 01.06 ABAR (BAR5). DMA via HHDM + low physical frames.
 * Completion: MSI/MSI-X (vector 42) or INTx with sched_sleep; poll fallback.
 *
 * Async NCQ: CAP.SNCQ + IDENTIFY NCQ → up to ASYNC_DEPTH FPDMA commands with
 * per-HW-slot bounce. Shared bio queue lets sync callers share the NCQ window.
 * blk_async / ahci_async_rw adds true non-blocking submit with end_io
 * callbacks (blk_async_poll / sync-waiter pump harvests + fires; IRQ only
 * wakes). Sync blk_read/write still wait. See docs/blk-modern-path.md.
 * ------------------------------------------------------------------------- */

static const u64 HHDM_BASE = 0xFFFF800000000000ULL;

enum {
    SECTOR_SIZE     = 512,

    GHC_CAP         = 0x00,
    GHC_GHC         = 0x04,
    GHC_IS          = 0x08,
    GHC_PI          = 0x0C,
    GHC_VS          = 0x10,

    GHC_AE          = 0x80000000u,
    GHC_HR          = 0x00000001u,

    PORT_SIZE       = 0x80,
    PORT_BASE       = 0x100,

    PxCLB           = 0x00,
    PxCLBU          = 0x04,
    PxFB            = 0x08,
    PxFBU           = 0x0C,
    PxIS            = 0x10,
    PxIE            = 0x14,
    PxCMD           = 0x18,
    PxTFD           = 0x20,
    PxSIG           = 0x24,
    PxSSTS          = 0x28,
    PxSCTL          = 0x2C,
    PxSERR          = 0x30,
    PxSACT          = 0x34,
    PxCI            = 0x38,

    PxCMD_ST        = 0x0001,
    PxCMD_FRE       = 0x0010,
    PxCMD_FR        = 0x4000,
    PxCMD_CR        = 0x8000,

    SSTS_DET_MASK   = 0x0F,
    SSTS_DET_PRESENT = 0x03,

    CAP_NCS_MASK    = 0x1F00u,       /* bits 12:8 = slots-1 */
    CAP_SNCQ        = 0x40000000u,   /* bit 30 */

    ATA_CMD_IDENT   = 0xEC,
    ATA_CMD_READ_DMA_EXT = 0x25,
    ATA_CMD_WRITE_DMA_EXT = 0x35,
    ATA_CMD_READ_FPDMA = 0x60,
    ATA_CMD_WRITE_FPDMA = 0x61,
    ATA_CMD_FLUSH   = 0xE7,

    FIS_REG_H2D     = 0x27,
    CFL_H2D         = 5,

    /* Arena: CL(1KiB) + FIS(256) + CT*32(8KiB) + IDENT(512) + bounce*ASYNC */
    ARENA_ORDER     = 4,             /* 16 pages = 64 KiB */
    N_SLOTS         = 32,
    CT_SIZE         = 256,
    CL_OFF          = 0,
    FIS_OFF         = 1024,
    CT_BASE         = 1024 + 256,
    IDENT_OFF       = 1024 + 256 + 32 * 256,
    BOUNCE_OFF      = 1024 + 256 + 32 * 256 + 512,
    BOUNCE_MAX_SEC  = 16,
    BOUNCE_SLOT_BYTES = 16 * 512,    /* BOUNCE_MAX_SEC * SECTOR_SIZE */
    ASYNC_DEPTH     = 4,             /* per-slot bounce windows */
    BIO_Q           = 8,             /* cross-caller bio queue depth */

    BIO_FREE        = 0,
    BIO_ACTIVE      = 1,
    BIO_DONE_OK     = 2,
    BIO_DONE_FAIL   = 3,

    POLL_SPINS      = 5000000,

    AHCI_IRQ_VECTOR = 42,
    PCI_INTERRUPT_LINE = 0x3C,

    PxIE_MASK       = 0x07C0007Fu,
    GHC_IE          = 0x00000002u,

    /* Selftest: R/W last TEST_SECS of the disk (save/restore). */
    AHCI_TEST_SECS  = 64
};

static const u64 AHCI_WAIT_CHAN = 0x4148434900000000ULL;

struct ahci {
    u32 present;
    u32 port;
    u64 abar_phys;
    u64 arena_phys;
    u64 capacity;
    u32 msi;
    u32 irq_armed;
    u32 ncq;                         /* FPDMA path armed */
    u32 nslots;                      /* 1..32 from CAP.NCS */
    u32 depth;                       /* active async window (1..ASYNC_DEPTH) */
    u32 inflight;                    /* current outstanding cmds */
    u32 peak_inflight;               /* high-water for selftest/log */
    u32 hw_active;                   /* bitmask of HW slots owned by bios */
    u8  pci_irq;
};

/* Local alias — same shape as drivers.blk_end_io_t (per-TU typedef). */
typedef void (*blk_end_io_t)(void *private, u32 ok);

/* One transfer enqueued by a caller; multiple may share NCQ slots.
 * async=1 → auto-free on complete and require end_io (blk_async path). */
struct ahci_bio {
    u32 state;
    u32 write;
    u32 nsec;
    u32 submitted;
    u32 completed;
    u32 async;
    u64 sector;
    u8 *kbuf;
    blk_end_io_t end_io;
    void *private;
};

struct ahci_cb_snap {
    blk_end_io_t fn;
    void *priv;
    u32 ok;
};

static struct ahci ah;
static struct ahci_bio bios[BIO_Q];
static volatile u32 ahci_qlock;
static struct ahci_cb_snap cb_pending[BIO_Q];
static u32 cb_pending_n;

/* Per-HW-slot bookkeeping (indices < ASYNC_DEPTH). */
static u32 slot_nsec[ASYNC_DEPTH];
static u32 slot_off[ASYNC_DEPTH];
static u8  slot_cmd[ASYNC_DEPTH];
static u32 slot_bio[ASYNC_DEPTH];    /* owning bio index */

static volatile u8 *hhdm8(u64 phys) {
    return (volatile u8 *)(HHDM_BASE + phys);
}

static volatile u32 *hhdm32(u64 phys) {
    return (volatile u32 *)(HHDM_BASE + phys);
}

static volatile u32 *ghc_reg(u32 off) {
    return hhdm32(ah.abar_phys + (u64)off);
}

static volatile u32 *port_reg(u32 off) {
    return hhdm32(ah.abar_phys + (u64)PORT_BASE
                  + (u64)ah.port * (u64)PORT_SIZE + (u64)off);
}

static u64 ct_phys_for(u32 slot) {
    return ah.arena_phys + CT_BASE + (u64)slot * (u64)CT_SIZE;
}

static u64 bounce_phys_for(u32 slot) {
    return ah.arena_phys + BOUNCE_OFF + (u64)slot * (u64)BOUNCE_SLOT_BYTES;
}

static u32 ahci_popcount(u32 mask) {
    u32 n = 0;
    while (mask != 0) {
        n = n + (mask & 1u);
        mask = mask >> 1;
    }
    return n;
}

static void ahci_stop_port(void) {
    u32 cmd;
    u32 spins;
    cmd = *port_reg(PxCMD);
    cmd = cmd & ~(u32)(PxCMD_ST | PxCMD_FRE);
    *port_reg(PxCMD) = cmd;
    spins = 0;
    while ((*port_reg(PxCMD) & (PxCMD_FR | PxCMD_CR)) != 0) {
        spins++;
        if (spins > POLL_SPINS) {
            break;
        }
        cpu_pause();
    }
}

static u32 ahci_start_port(void) {
    u32 cmd;
    u32 spins;
    spins = 0;
    while ((*port_reg(PxCMD) & PxCMD_CR) != 0) {
        spins++;
        if (spins > POLL_SPINS) {
            return 0;
        }
        cpu_pause();
    }
    cmd = *port_reg(PxCMD);
    cmd = cmd | PxCMD_FRE;
    *port_reg(PxCMD) = cmd;
    cmd = *port_reg(PxCMD);
    cmd = cmd | PxCMD_ST;
    *port_reg(PxCMD) = cmd;
    return 1;
}

static u32 ahci_slot_busy(u32 slot) {
    u32 bit = 1u << slot;
    if ((*port_reg(PxCI) & bit) != 0) {
        return 1;
    }
    if (ah.ncq && (*port_reg(PxSACT) & bit) != 0) {
        return 1;
    }
    return 0;
}

static u32 ahci_check_fatal(u8 ata_cmd) {
    u32 tfd = *port_reg(PxTFD);
    u32 is = *port_reg(PxIS);
    if ((tfd & 0x01) != 0) {
        kprintf("[AHCI] issue ERR cmd=0x%x TFD=0x%x IS=0x%x\n",
                (u64)ata_cmd, (u64)tfd, (u64)is);
        *port_reg(PxIS) = 0xFFFFFFFFu;
        *ghc_reg(GHC_IS) = 0xFFFFFFFFu;
        return 1;
    }
    if ((is & 0x40000000u) != 0) {
        kprintf("[AHCI] TFES cmd=0x%x IS=0x%x\n", (u64)ata_cmd, (u64)is);
        *port_reg(PxIS) = 0xFFFFFFFFu;
        *ghc_reg(GHC_IS) = 0xFFFFFFFFu;
        return 1;
    }
    return 0;
}

/* Wait for slot bit to clear in CI (and SACT when NCQ). */
static u32 ahci_wait_slot(u8 ata_cmd, u32 slot) {
    u32 spins = 0;
    while (ahci_slot_busy(slot)) {
        if (ahci_check_fatal(ata_cmd)) {
            return 0;
        }
        spins++;
        if (ah.irq_armed && sched_is_active() && (spins & 63) == 0) {
            sched_sleep(AHCI_WAIT_CHAN);
        } else {
            arch.inb(0x80);
            cpu_pause();
        }
        if (spins > POLL_SPINS) {
            kprintf("[AHCI] timeout cmd=0x%x CI=0x%x SACT=0x%x\n",
                    (u64)ata_cmd, (u64)(*port_reg(PxCI)),
                    (u64)(*port_reg(PxSACT)));
            return 0;
        }
    }
    *port_reg(PxIS) = 0xFFFFFFFFu;
    *ghc_reg(GHC_IS) = 0xFFFFFFFFu;
    return 1;
}

/* Harvest completed HW slots and submit new chunks from active bios. */
static void ahci_bio_pump_locked(void);

/* Copy + clear deferred end_io snaps; caller must hold ahci_qlock. */
static u32 ahci_take_cbs_locked(struct ahci_cb_snap *out) {
    u32 n = cb_pending_n;
    u32 i;
    for (i = 0; i < n; i++) {
        out[i] = cb_pending[i];
    }
    cb_pending_n = 0;
    return n;
}

static void ahci_fire_cbs(struct ahci_cb_snap *s, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        if (s[i].fn != 0) {
            s[i].fn(s[i].priv, s[i].ok);
        }
    }
}

/* Mark bio done; queue end_io if any; auto-free async bios. */
static void ahci_bio_mark_done_locked(u32 bid, u32 ok) {
    blk_end_io_t fn;
    void *priv;
    if (bid >= BIO_Q) {
        return;
    }
    if (bios[bid].state != BIO_ACTIVE) {
        return;
    }
    bios[bid].state = ok ? BIO_DONE_OK : BIO_DONE_FAIL;
    fn = bios[bid].end_io;
    priv = bios[bid].private;
    if (fn != 0 && cb_pending_n < BIO_Q) {
        cb_pending[cb_pending_n].fn = fn;
        cb_pending[cb_pending_n].priv = priv;
        cb_pending[cb_pending_n].ok = ok;
        cb_pending_n = cb_pending_n + 1;
    }
    if (bios[bid].async) {
        bios[bid].state = BIO_FREE;
        bios[bid].kbuf = 0;
        bios[bid].end_io = 0;
        bios[bid].private = 0;
        bios[bid].async = 0;
    }
}

u32 ahci_irq(void) {
    u32 is;
    u32 pis;
    if (!ah.present) {
        return 0;
    }
    is = *ghc_reg(GHC_IS);
    pis = *port_reg(PxIS);
    if (is == 0 && pis == 0) {
        return 0;
    }
    *port_reg(PxIS) = pis;
    *ghc_reg(GHC_IS) = is;
    /* Wake sync waiters / async pollers. Do not take ahci_qlock here:
     * spin_lock is not IRQ-safe. end_io runs from wait/poll/flush pump. */
    sched_wakeup(AHCI_WAIT_CHAN);
    return 1;
}

static u32 ahci_pick_slot(void) {
    u32 busy = *port_reg(PxCI) | *port_reg(PxSACT);
    u32 i;
    u32 lim = ah.depth != 0 ? ah.depth : 1;
    if (lim > N_SLOTS) {
        lim = N_SLOTS;
    }
    if (lim > ASYNC_DEPTH) {
        lim = ASYNC_DEPTH;
    }
    for (i = 0; i < lim; i++) {
        if (((busy >> i) & 1u) == 0 && ((ah.hw_active >> i) & 1u) == 0) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

/* Program H2D FIS + PRDT on `slot` and ring CI (and SACT for NCQ). No wait.
 * If force_slot != 0xFFFFFFFF, use that slot (must be free). */
static u32 ahci_submit(u8 ata_cmd, u64 lba, u16 count, u64 buf_phys, u32 write,
                       u32 force_slot, u32 *oslot) {
    u64 cl_phys;
    u64 ct_phys;
    u64 fis_phys;
    u64 prdt_phys;
    u32 prdtl;
    u32 hdr0;
    u32 byte_count;
    u32 i;
    u32 slot;
    u32 use_ncq;
    u8 cmd;

    if (count == 0) {
        return 0;
    }

    if (force_slot != 0xFFFFFFFFu) {
        slot = force_slot;
        if (ahci_slot_busy(slot)) {
            kprintf("[AHCI] forced slot %u busy\n", (u64)slot);
            return 0;
        }
    } else {
        slot = ahci_pick_slot();
        if (slot == 0xFFFFFFFFu) {
            kprintf("[AHCI] no free command slot\n");
            return 0;
        }
    }

    use_ncq = ah.ncq
              && (ata_cmd == (u8)ATA_CMD_READ_DMA_EXT
                  || ata_cmd == (u8)ATA_CMD_WRITE_DMA_EXT);
    if (use_ncq) {
        cmd = write ? (u8)ATA_CMD_WRITE_FPDMA : (u8)ATA_CMD_READ_FPDMA;
    } else {
        cmd = ata_cmd;
    }

    cl_phys = ah.arena_phys + CL_OFF + (u64)slot * 32;
    ct_phys = ct_phys_for(slot);
    fis_phys = ct_phys;
    prdt_phys = ct_phys + 0x80;

    *port_reg(PxIS) = 0xFFFFFFFFu;

    for (i = 0; i < 256; i++) {
        hhdm8(ct_phys)[i] = 0;
    }

    hhdm8(fis_phys)[0] = FIS_REG_H2D;
    hhdm8(fis_phys)[1] = 0x80;
    hhdm8(fis_phys)[2] = cmd;
    if (use_ncq) {
        /* Feature = sector count; Sector Count = NCQ tag << 3. */
        hhdm8(fis_phys)[3] = (u8)(count & 0xFF);
        hhdm8(fis_phys)[11] = (u8)((count >> 8) & 0xFF);
        hhdm8(fis_phys)[12] = (u8)((slot & 0x1F) << 3);
        hhdm8(fis_phys)[13] = 0;
    } else {
        hhdm8(fis_phys)[3] = 0;
        hhdm8(fis_phys)[11] = 0;
        hhdm8(fis_phys)[12] = (u8)(count & 0xFF);
        hhdm8(fis_phys)[13] = (u8)((count >> 8) & 0xFF);
    }
    hhdm8(fis_phys)[4] = (u8)(lba & 0xFF);
    hhdm8(fis_phys)[5] = (u8)((lba >> 8) & 0xFF);
    hhdm8(fis_phys)[6] = (u8)((lba >> 16) & 0xFF);
    hhdm8(fis_phys)[7] = 0x40;
    hhdm8(fis_phys)[8] = (u8)((lba >> 24) & 0xFF);
    hhdm8(fis_phys)[9] = (u8)((lba >> 32) & 0xFF);
    hhdm8(fis_phys)[10] = (u8)((lba >> 40) & 0xFF);
    hhdm8(fis_phys)[14] = 0;
    hhdm8(fis_phys)[15] = 0;

    byte_count = (u32)count * SECTOR_SIZE;
    prdtl = 1;
    *hhdm32(prdt_phys + 0) = (u32)(buf_phys & 0xFFFFFFFFu);
    *hhdm32(prdt_phys + 4) = (u32)(buf_phys >> 32);
    *hhdm32(prdt_phys + 8) = 0;
    *hhdm32(prdt_phys + 12) = (byte_count - 1) | 0x80000000u;

    for (i = 0; i < 32; i++) {
        hhdm8(cl_phys)[i] = 0;
    }
    hdr0 = CFL_H2D | (prdtl << 16);
    if (write) {
        hdr0 = hdr0 | (1u << 6);
    }
    *hhdm32(cl_phys + 0) = hdr0;
    *hhdm32(cl_phys + 4) = 0;
    *hhdm32(cl_phys + 8) = (u32)(ct_phys & 0xFFFFFFFFu);
    *hhdm32(cl_phys + 12) = (u32)(ct_phys >> 32);

    if (use_ncq) {
        *port_reg(PxSACT) = 1u << slot;
    }
    *port_reg(PxCI) = 1u << slot;

    ah.inflight = ah.inflight + 1;
    if (ah.inflight > ah.peak_inflight) {
        ah.peak_inflight = ah.inflight;
    }

    if (oslot != 0) {
        *oslot = slot;
    }
    if (slot < ASYNC_DEPTH) {
        slot_cmd[slot] = cmd;
    }
    return 1;
}

/* Submit + wait (IDENTIFY / non-pipelined / flush helpers). */
static u32 ahci_issue(u8 ata_cmd, u64 lba, u16 count, u64 buf_phys, u32 write) {
    u32 slot = 0;
    u8 cmd;
    if (!ahci_submit(ata_cmd, lba, count, buf_phys, write, 0xFFFFFFFFu, &slot)) {
        return 0;
    }
    cmd = ata_cmd;
    if (slot < ASYNC_DEPTH) {
        cmd = slot_cmd[slot];
    }
    if (!ahci_wait_slot(cmd, slot)) {
        if (ah.inflight > 0) {
            ah.inflight = ah.inflight - 1;
        }
        return 0;
    }
    if (ah.inflight > 0) {
        ah.inflight = ah.inflight - 1;
    }
    return 1;
}

/* Drain every outstanding command (best-effort). */
static void ahci_drain(void) {
    u32 spins = 0;
    while ((*port_reg(PxCI) | (ah.ncq ? *port_reg(PxSACT) : 0)) != 0) {
        if (ahci_check_fatal((u8)ATA_CMD_READ_FPDMA)) {
            break;
        }
        spins++;
        if (ah.irq_armed && sched_is_active() && (spins & 63) == 0) {
            sched_sleep(AHCI_WAIT_CHAN);
        } else {
            cpu_pause();
        }
        if (spins > POLL_SPINS) {
            break;
        }
    }
    ah.inflight = 0;
    ah.hw_active = 0;
    *port_reg(PxIS) = 0xFFFFFFFFu;
    *ghc_reg(GHC_IS) = 0xFFFFFFFFu;
}

static void ahci_bio_fail_all_active(void) {
    u32 i;
    for (i = 0; i < BIO_Q; i++) {
        if (bios[i].state == BIO_ACTIVE) {
            ahci_bio_mark_done_locked(i, 0);
        }
    }
    ah.hw_active = 0;
}

/* Harvest completed HW slots and submit new chunks from active bios.
 * Caller must hold ahci_qlock. */
static void ahci_bio_pump_locked(void) {
    u32 i;
    u32 depth;
    u32 progressed;
    u32 spins_guard;

    depth = ah.ncq ? ah.depth : 1;
    if (depth < 1) {
        depth = 1;
    }
    if (depth > ASYNC_DEPTH) {
        depth = ASYNC_DEPTH;
    }

    spins_guard = 0;
    while (1) {
        progressed = 0;

        /* ---- harvest ---- */
        for (i = 0; i < ASYNC_DEPTH; i++) {
            u32 bid;
            u32 n;
            u32 off;
            u32 b;
            u64 bounce;
            if (((ah.hw_active >> i) & 1u) == 0) {
                continue;
            }
            if (ahci_slot_busy(i)) {
                continue;
            }
            if (ahci_check_fatal(slot_cmd[i])) {
                ahci_drain();
                ahci_bio_fail_all_active();
                return;
            }
            bid = slot_bio[i];
            n = slot_nsec[i];
            off = slot_off[i];
            bounce = bounce_phys_for(i);
            if (bid < BIO_Q && bios[bid].state == BIO_ACTIVE && !bios[bid].write) {
                for (b = 0; b < n * SECTOR_SIZE; b++) {
                    bios[bid].kbuf[(u64)off * SECTOR_SIZE + b] = hhdm8(bounce)[b];
                }
            }
            ah.hw_active = ah.hw_active & ~(1u << i);
            if (ah.inflight > 0) {
                ah.inflight = ah.inflight - 1;
            }
            if (bid < BIO_Q && bios[bid].state == BIO_ACTIVE) {
                bios[bid].completed = bios[bid].completed + n;
                if (bios[bid].completed >= bios[bid].nsec) {
                    ahci_bio_mark_done_locked(bid, 1);
                }
            }
            progressed = 1;
        }

        /* ---- submit ---- */
        for (i = 0; i < BIO_Q; i++) {
            while (bios[i].state == BIO_ACTIVE
                   && bios[i].submitted < bios[i].nsec
                   && ahci_popcount(ah.hw_active) < depth) {
                u32 n = bios[i].nsec - bios[i].submitted;
                u32 slot;
                u64 bounce;
                u32 b;
                u32 write = bios[i].write;
                if (n > BOUNCE_MAX_SEC) {
                    n = BOUNCE_MAX_SEC;
                }
                slot = ahci_pick_slot();
                if (slot == 0xFFFFFFFFu) {
                    break;
                }
                bounce = bounce_phys_for(slot);
                if (write) {
                    for (b = 0; b < n * SECTOR_SIZE; b++) {
                        hhdm8(bounce)[b] =
                            bios[i].kbuf[(u64)bios[i].submitted * SECTOR_SIZE + b];
                    }
                }
                if (!ahci_submit((u8)(write ? ATA_CMD_WRITE_DMA_EXT
                                            : ATA_CMD_READ_DMA_EXT),
                                 bios[i].sector + (u64)bios[i].submitted,
                                 (u16)n, bounce, write, slot, &slot)) {
                    ahci_drain();
                    ahci_bio_fail_all_active();
                    return;
                }
                slot_nsec[slot] = n;
                slot_off[slot] = bios[i].submitted;
                slot_bio[slot] = i;
                ah.hw_active = ah.hw_active | (1u << slot);
                bios[i].submitted = bios[i].submitted + n;
                progressed = 1;
            }
        }

        if (!progressed) {
            break;
        }
        spins_guard++;
        if (spins_guard > 100000) {
            break;
        }
    }
}

static u32 ahci_bio_alloc_locked(void) {
    u32 i;
    for (i = 0; i < BIO_Q; i++) {
        if (bios[i].state == BIO_FREE) {
            bios[i].state = BIO_ACTIVE;
            bios[i].write = 0;
            bios[i].nsec = 0;
            bios[i].submitted = 0;
            bios[i].completed = 0;
            bios[i].async = 0;
            bios[i].sector = 0;
            bios[i].kbuf = 0;
            bios[i].end_io = 0;
            bios[i].private = 0;
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

static void ahci_bio_free_locked(u32 idx) {
    if (idx < BIO_Q) {
        bios[idx].state = BIO_FREE;
        bios[idx].kbuf = 0;
        bios[idx].end_io = 0;
        bios[idx].private = 0;
        bios[idx].async = 0;
    }
}

static u32 ahci_bios_busy(void) {
    u32 i;
    for (i = 0; i < BIO_Q; i++) {
        if (bios[i].state == BIO_ACTIVE) {
            return 1;
        }
    }
    return ah.hw_active != 0;
}

/* Wait until bio `idx` leaves ACTIVE. Releases qlock around sleep/poll. */
static u32 ahci_bio_wait(u32 idx) {
    u32 spins = 0;
    u32 st;
    struct ahci_cb_snap local[BIO_Q];
    u32 n;
    while (1) {
        spin_lock(&ahci_qlock);
        cb_pending_n = 0;
        ahci_bio_pump_locked();
        st = bios[idx].state;
        n = ahci_take_cbs_locked(local);
        spin_unlock(&ahci_qlock);
        ahci_fire_cbs(local, n);
        if (st != BIO_ACTIVE) {
            return st == BIO_DONE_OK ? 1 : 0;
        }

        spins++;
        if (ah.irq_armed && sched_is_active() && (spins & 63) == 0) {
            sched_sleep(AHCI_WAIT_CHAN);
        } else {
            arch.inb(0x80);
            cpu_pause();
        }
        if (spins > POLL_SPINS) {
            kprintf("[AHCI] bio wait timeout idx=%u CI=0x%x SACT=0x%x\n",
                    (u64)idx, (u64)(*port_reg(PxCI)),
                    (u64)(*port_reg(PxSACT)));
            spin_lock(&ahci_qlock);
            if (bios[idx].state == BIO_ACTIVE) {
                ahci_bio_mark_done_locked(idx, 0);
            }
            n = ahci_take_cbs_locked(local);
            spin_unlock(&ahci_qlock);
            ahci_fire_cbs(local, n);
            return 0;
        }
    }
    return 0;
}

/* Enqueue one transfer; returns bio index or 0xFFFFFFFF. Does not wait.
 * async=1 + end_io → non-blocking blk_async path (auto-free on complete). */
static u32 ahci_bio_enqueue_ex(u64 sector, void *kbuf, u32 count, u32 write,
                               blk_end_io_t end_io, void *private, u32 async) {
    u32 idx;
    u32 spins = 0;
    struct ahci_cb_snap local[BIO_Q];
    u32 n;

    if (!ah.present || count == 0) {
        return 0xFFFFFFFFu;
    }
    if (sector + (u64)count > ah.capacity) {
        return 0xFFFFFFFFu;
    }
    if (async && end_io == 0) {
        return 0xFFFFFFFFu;
    }

    while (1) {
        spin_lock(&ahci_qlock);
        cb_pending_n = 0;
        idx = ahci_bio_alloc_locked();
        if (idx != 0xFFFFFFFFu) {
            bios[idx].write = write;
            bios[idx].nsec = count;
            bios[idx].submitted = 0;
            bios[idx].completed = 0;
            bios[idx].async = async;
            bios[idx].sector = sector;
            bios[idx].kbuf = (u8 *)kbuf;
            bios[idx].end_io = end_io;
            bios[idx].private = private;
            ahci_bio_pump_locked();
            n = ahci_take_cbs_locked(local);
            spin_unlock(&ahci_qlock);
            ahci_fire_cbs(local, n);
            return idx;
        }
        ahci_bio_pump_locked();
        n = ahci_take_cbs_locked(local);
        spin_unlock(&ahci_qlock);
        ahci_fire_cbs(local, n);
        spins++;
        if (ah.irq_armed && sched_is_active() && (spins & 63) == 0) {
            sched_sleep(AHCI_WAIT_CHAN);
        } else {
            cpu_pause();
        }
        if (spins > POLL_SPINS) {
            kprintf("[AHCI] bio queue full timeout\n");
            return 0xFFFFFFFFu;
        }
    }
    return 0xFFFFFFFFu;
}

static u32 ahci_bio_enqueue(u64 sector, void *kbuf, u32 count, u32 write) {
    return ahci_bio_enqueue_ex(sector, kbuf, count, write, 0, 0, 0);
}

/* Non-blocking submit; end_io(private, ok) on completion. Returns 1 if queued. */
u32 ahci_async_rw(u64 sector, void *kbuf, u32 count, u32 write,
                  blk_end_io_t end_io, void *private) {
    u32 idx;
    if (end_io == 0) {
        return 0;
    }
    idx = ahci_bio_enqueue_ex(sector, kbuf, count, write, end_io, private, 1);
    return idx != 0xFFFFFFFFu;
}

/* Progress outstanding async bios (poll fallback / early boot selftest). */
void ahci_async_poll(void) {
    struct ahci_cb_snap local[BIO_Q];
    u32 n;
    if (!ah.present) {
        return;
    }
    spin_lock(&ahci_qlock);
    cb_pending_n = 0;
    ahci_bio_pump_locked();
    n = ahci_take_cbs_locked(local);
    spin_unlock(&ahci_qlock);
    ahci_fire_cbs(local, n);
}

u32 ahci_present_p(void) {
    return ah.present;
}

u64 ahci_capacity(void) {
    return ah.capacity;
}

/* Sync transfer via bio queue (may overlap other callers' bios). */
static u32 ahci_xfer(u64 sector, void *kbuf, u32 count, u32 write) {
    u32 idx;
    u32 ok;
    idx = ahci_bio_enqueue(sector, kbuf, count, write);
    if (idx == 0xFFFFFFFFu) {
        return 0;
    }
    ok = ahci_bio_wait(idx);
    spin_lock(&ahci_qlock);
    ahci_bio_free_locked(idx);
    spin_unlock(&ahci_qlock);
    return ok;
}

u32 ahci_read(u64 sector, void *kbuf, u32 count) {
    return ahci_xfer(sector, kbuf, count, 0);
}

u32 ahci_write(u64 sector, const void *kbuf, u32 count) {
    return ahci_xfer(sector, (void *)kbuf, count, 1);
}

u32 ahci_flush(void) {
    u64 bounce;
    u32 spins = 0;
    struct ahci_cb_snap local[BIO_Q];
    u32 n;
    if (!ah.present) {
        return 0;
    }
    /* Drain bio queue before issuing non-NCQ FLUSH on slot 0. */
    while (1) {
        spin_lock(&ahci_qlock);
        cb_pending_n = 0;
        ahci_bio_pump_locked();
        if (!ahci_bios_busy()) {
            n = ahci_take_cbs_locked(local);
            spin_unlock(&ahci_qlock);
            ahci_fire_cbs(local, n);
            break;
        }
        n = ahci_take_cbs_locked(local);
        spin_unlock(&ahci_qlock);
        ahci_fire_cbs(local, n);
        spins++;
        if (ah.irq_armed && sched_is_active() && (spins & 63) == 0) {
            sched_sleep(AHCI_WAIT_CHAN);
        } else {
            cpu_pause();
        }
        if (spins > POLL_SPINS) {
            kprintf("[AHCI] flush drain timeout\n");
            return 0;
        }
    }
    spin_lock(&ahci_qlock);
    ahci_drain();
    bounce = ah.arena_phys + IDENT_OFF;
    {
        u64 cl_phys = ah.arena_phys + CL_OFF;
        u64 ct_phys = ct_phys_for(0);
        u64 fis_phys = ct_phys;
        u64 prdt_phys = ct_phys + 0x80;
        u32 i;
        *port_reg(PxIS) = 0xFFFFFFFFu;
        for (i = 0; i < 256; i++) {
            hhdm8(ct_phys)[i] = 0;
        }
        hhdm8(fis_phys)[0] = FIS_REG_H2D;
        hhdm8(fis_phys)[1] = 0x80;
        hhdm8(fis_phys)[2] = (u8)ATA_CMD_FLUSH;
        hhdm8(fis_phys)[7] = 0x40;
        *hhdm32(prdt_phys + 0) = (u32)(bounce & 0xFFFFFFFFu);
        *hhdm32(prdt_phys + 4) = (u32)(bounce >> 32);
        *hhdm32(prdt_phys + 8) = 0;
        *hhdm32(prdt_phys + 12) = 0 | 0x80000000u;
        for (i = 0; i < 32; i++) {
            hhdm8(cl_phys)[i] = 0;
        }
        *hhdm32(cl_phys + 0) = CFL_H2D | (1u << 16);
        *hhdm32(cl_phys + 8) = (u32)(ct_phys & 0xFFFFFFFFu);
        *hhdm32(cl_phys + 12) = (u32)(ct_phys >> 32);
        *port_reg(PxCI) = 1;
        spin_unlock(&ahci_qlock);
        if (!ahci_wait_slot((u8)ATA_CMD_FLUSH, 0)) {
            return 0;
        }
    }
    return 1;
}

/* Save/restore tail sectors; prove multi-outstanding + cross-caller bio queue. */
/* blk_async selftest completion counters (pre-sched; polled). */
static volatile u32 async_cb_done;
static volatile u32 async_cb_ok;

static void ahci_async_selftest_cb(void *private, u32 ok) {
    (void)private;
    if (ok) {
        async_cb_ok = async_cb_ok + 1;
    }
    async_cb_done = async_cb_done + 1;
}

void ahci_selftest(void) {
    u64 lba;
    u32 secs = AHCI_TEST_SECS;
    u64 save_phys;
    u64 pat_phys;
    u8 *save;
    u8 *pat;
    u32 i;
    u32 ok;
    u32 peak;
    u32 half;
    u32 b0;
    u32 b1;
    u32 peak_x;

    if (!ah.present) {
        return;
    }
    if (!ah.ncq || ah.depth < 2) {
        kprintf("[AHCI] async NCQ selftest skipped (ncq=%u depth=%u)\n",
                (u64)ah.ncq, (u64)ah.depth);
        return;
    }
    if (ah.capacity < (u64)secs + 8) {
        kprintf("[AHCI] async NCQ selftest skipped (tiny disk)\n");
        return;
    }

    /* Two order-3 blocks: 32 KiB each covers 64 sectors. */
    save_phys = mem.pmm_alloc_pages(3);
    pat_phys = mem.pmm_alloc_pages(3);
    if (save_phys == 0 || pat_phys == 0) {
        kprintf("[AHCI] [FAIL] async NCQ selftest alloc\n");
        return;
    }
    save = (u8 *)(void *)hhdm8(save_phys);
    pat = (u8 *)(void *)hhdm8(pat_phys);
    lba = ah.capacity - (u64)secs;

    ah.peak_inflight = 0;
    if (!ahci_read(lba, save, secs)) {
        kprintf("[AHCI] [FAIL] async NCQ save-read\n");
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }

    for (i = 0; i < secs * SECTOR_SIZE; i++) {
        pat[i] = (u8)(0xA5 ^ (u8)i ^ (u8)(i >> 8));
    }

    ah.peak_inflight = 0;
    if (!ahci_write(lba, pat, secs)) {
        kprintf("[AHCI] [FAIL] async NCQ multi-write\n");
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }
    peak = ah.peak_inflight;

    for (i = 0; i < secs * SECTOR_SIZE; i++) {
        pat[i] = 0;
    }
    if (!ahci_read(lba, pat, secs)) {
        kprintf("[AHCI] [FAIL] async NCQ multi-read\n");
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }

    ok = 1;
    for (i = 0; i < secs * SECTOR_SIZE; i++) {
        u8 expect = (u8)(0xA5 ^ (u8)i ^ (u8)(i >> 8));
        if (pat[i] != expect) {
            ok = 0;
            break;
        }
    }

    if (!ok) {
        kprintf("[AHCI] [FAIL] async NCQ data mismatch\n");
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }
    if (peak < 2) {
        kprintf("[AHCI] [FAIL] async NCQ peak_inflight=%u (want >=2)\n",
                (u64)peak);
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }
    kprintf("[AHCI] [PASS] async NCQ multi-outstanding (peak=%u depth=%u secs=%u)\n",
            (u64)peak, (u64)ah.depth, (u64)secs);

    /* Cross-caller bio queue: two overlapping bios share the NCQ window.
     * Same thread enqueues both before waiting — proves queue concurrency
     * without requiring sched (selftest runs before sched_init). */
    half = secs / 2;
    for (i = 0; i < secs * SECTOR_SIZE; i++) {
        pat[i] = (u8)(0x5A ^ (u8)i ^ (u8)(i >> 7));
    }
    ah.peak_inflight = 0;
    b0 = ahci_bio_enqueue(lba, pat, half, 1);
    b1 = ahci_bio_enqueue(lba + (u64)half, pat + (u64)half * SECTOR_SIZE, half, 1);
    if (b0 == 0xFFFFFFFFu || b1 == 0xFFFFFFFFu) {
        kprintf("[AHCI] [FAIL] bio queue enqueue\n");
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }
    if (!ahci_bio_wait(b0) || !ahci_bio_wait(b1)) {
        kprintf("[AHCI] [FAIL] bio queue dual-write wait\n");
        spin_lock(&ahci_qlock);
        ahci_bio_free_locked(b0);
        ahci_bio_free_locked(b1);
        spin_unlock(&ahci_qlock);
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }
    peak_x = ah.peak_inflight;
    spin_lock(&ahci_qlock);
    ahci_bio_free_locked(b0);
    ahci_bio_free_locked(b1);
    spin_unlock(&ahci_qlock);

    for (i = 0; i < secs * SECTOR_SIZE; i++) {
        pat[i] = 0;
    }
    /* Overlapping dual-read similarly. */
    ah.peak_inflight = 0;
    b0 = ahci_bio_enqueue(lba, pat, half, 0);
    b1 = ahci_bio_enqueue(lba + (u64)half, pat + (u64)half * SECTOR_SIZE, half, 0);
    if (b0 == 0xFFFFFFFFu || b1 == 0xFFFFFFFFu) {
        kprintf("[AHCI] [FAIL] bio queue read enqueue\n");
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }
    if (!ahci_bio_wait(b0) || !ahci_bio_wait(b1)) {
        kprintf("[AHCI] [FAIL] bio queue dual-read wait\n");
        spin_lock(&ahci_qlock);
        ahci_bio_free_locked(b0);
        ahci_bio_free_locked(b1);
        spin_unlock(&ahci_qlock);
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }
    if (ah.peak_inflight > peak_x) {
        peak_x = ah.peak_inflight;
    }
    spin_lock(&ahci_qlock);
    ahci_bio_free_locked(b0);
    ahci_bio_free_locked(b1);
    spin_unlock(&ahci_qlock);

    ok = 1;
    for (i = 0; i < secs * SECTOR_SIZE; i++) {
        u8 expect = (u8)(0x5A ^ (u8)i ^ (u8)(i >> 7));
        if (pat[i] != expect) {
            ok = 0;
            break;
        }
    }

    if (!ok) {
        kprintf("[AHCI] [FAIL] bio queue data mismatch\n");
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }
    if (peak_x < 2) {
        kprintf("[AHCI] [FAIL] bio queue peak_inflight=%u (want >=2)\n",
                (u64)peak_x);
        ahci_write(lba, save, secs);
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        return;
    }
    kprintf("[AHCI] [PASS] bio queue cross-caller (peak=%u depth=%u half=%u)\n",
            (u64)peak_x, (u64)ah.depth, (u64)half);

    /* Non-blocking blk_async: dual submit + end_io callbacks, no bio_wait.
     * Progress via ahci_async_poll (selftest runs before sched/IRQ path). */
    {
        u32 peak_a;
        u32 spins_a;
        for (i = 0; i < secs * SECTOR_SIZE; i++) {
            pat[i] = (u8)(0x3C ^ (u8)i ^ (u8)(i >> 6));
        }
        async_cb_done = 0;
        async_cb_ok = 0;
        ah.peak_inflight = 0;
        if (!ahci_async_rw(lba, pat, half, 1, ahci_async_selftest_cb, 0)
            || !ahci_async_rw(lba + (u64)half,
                              pat + (u64)half * SECTOR_SIZE, half, 1,
                              ahci_async_selftest_cb, 0)) {
            kprintf("[AHCI] [FAIL] blk_async dual-write submit\n");
            ahci_write(lba, save, secs);
            mem.pmm_free_pages(save_phys, 3);
            mem.pmm_free_pages(pat_phys, 3);
            return;
        }
        spins_a = 0;
        while (async_cb_done < 2 && spins_a < POLL_SPINS) {
            ahci_async_poll();
            arch.inb(0x80);
            cpu_pause();
            spins_a = spins_a + 1;
        }
        peak_a = ah.peak_inflight;
        if (async_cb_done != 2 || async_cb_ok != 2) {
            kprintf("[AHCI] [FAIL] blk_async write cbs done=%u ok=%u\n",
                    (u64)async_cb_done, (u64)async_cb_ok);
            ahci_write(lba, save, secs);
            mem.pmm_free_pages(save_phys, 3);
            mem.pmm_free_pages(pat_phys, 3);
            return;
        }
        for (i = 0; i < secs * SECTOR_SIZE; i++) {
            pat[i] = 0;
        }
        async_cb_done = 0;
        async_cb_ok = 0;
        if (!ahci_async_rw(lba, pat, half, 0, ahci_async_selftest_cb, 0)
            || !ahci_async_rw(lba + (u64)half,
                              pat + (u64)half * SECTOR_SIZE, half, 0,
                              ahci_async_selftest_cb, 0)) {
            kprintf("[AHCI] [FAIL] blk_async dual-read submit\n");
            ahci_write(lba, save, secs);
            mem.pmm_free_pages(save_phys, 3);
            mem.pmm_free_pages(pat_phys, 3);
            return;
        }
        spins_a = 0;
        while (async_cb_done < 2 && spins_a < POLL_SPINS) {
            ahci_async_poll();
            arch.inb(0x80);
            cpu_pause();
            spins_a = spins_a + 1;
        }
        if (ah.peak_inflight > peak_a) {
            peak_a = ah.peak_inflight;
        }
        if (async_cb_done != 2 || async_cb_ok != 2) {
            kprintf("[AHCI] [FAIL] blk_async read cbs done=%u ok=%u\n",
                    (u64)async_cb_done, (u64)async_cb_ok);
            ahci_write(lba, save, secs);
            mem.pmm_free_pages(save_phys, 3);
            mem.pmm_free_pages(pat_phys, 3);
            return;
        }
        ok = 1;
        for (i = 0; i < secs * SECTOR_SIZE; i++) {
            u8 expect = (u8)(0x3C ^ (u8)i ^ (u8)(i >> 6));
            if (pat[i] != expect) {
                ok = 0;
                break;
            }
        }
        if (!ahci_write(lba, save, secs)) {
            kprintf("[AHCI] [FAIL] blk_async restore-write\n");
            mem.pmm_free_pages(save_phys, 3);
            mem.pmm_free_pages(pat_phys, 3);
            return;
        }
        mem.pmm_free_pages(save_phys, 3);
        mem.pmm_free_pages(pat_phys, 3);
        if (!ok) {
            kprintf("[AHCI] [FAIL] blk_async data mismatch\n");
            return;
        }
        if (peak_a < 2) {
            kprintf("[AHCI] [FAIL] blk_async peak_inflight=%u (want >=2)\n",
                    (u64)peak_a);
            return;
        }
        kprintf("[AHCI] [PASS] blk_async end_io (peak=%u depth=%u half=%u)\n",
                (u64)peak_a, (u64)ah.depth, (u64)half);
    }
}

void ahci_init(void) {
    u8 bus = 0;
    u8 slot = 0;
    u8 func = 0;
    u32 abar = 0;
    u32 is_io = 0;
    u32 cap;
    u32 pi;
    u32 vs;
    u32 ghc;
    u32 port;
    u32 found;
    u64 arena;
    u32 z;
    u16 *id;
    u32 ncs;
    u32 i;

    ah.present = 0;
    ah.capacity = 0;
    ah.abar_phys = 0;
    ah.arena_phys = 0;
    ah.port = 0;
    ah.msi = 0;
    ah.irq_armed = 0;
    ah.ncq = 0;
    ah.nslots = 1;
    ah.depth = 1;
    ah.inflight = 0;
    ah.peak_inflight = 0;
    ah.hw_active = 0;
    ah.pci_irq = 0;
    ahci_qlock = 0;
    for (i = 0; i < BIO_Q; i++) {
        bios[i].state = BIO_FREE;
        bios[i].kbuf = 0;
    }
    for (i = 0; i < ASYNC_DEPTH; i++) {
        slot_bio[i] = 0xFFFFFFFFu;
        slot_nsec[i] = 0;
        slot_off[i] = 0;
        slot_cmd[i] = 0;
    }

    if (!pci_find_class(0x01, 0x06, &bus, &slot, &func)) {
        return;
    }
    if (!pci_bar(bus, slot, func, 5, &abar, &is_io) || is_io != 0 || abar == 0) {
        kprintf("[AHCI] PCI 01.06 present but no MMIO ABAR\n");
        return;
    }
    if (abar >= 0x40000000u) {
        u16 cmd;
        u32 low_abar = 0x3E000000u;
        cmd = pci_read16(bus, slot, func, 0x04);
        pci_write16(bus, slot, func, 0x04, (u16)(cmd & ~(u16)0x0002));
        pci_write32(bus, slot, func, 0x24, low_abar);
        pci_write16(bus, slot, func, 0x04, (u16)(cmd | 0x0006));
        abar = low_abar;
        kprintf("[AHCI] relocated ABAR into HHDM at 0x%x\n", (u64)abar);
    }

    pci_enable_busmaster(bus, slot, func);
    ah.abar_phys = (u64)abar;

    ghc = *ghc_reg(GHC_GHC);
    *ghc_reg(GHC_GHC) = ghc | GHC_AE;

    cap = *ghc_reg(GHC_CAP);
    vs = *ghc_reg(GHC_VS);
    pi = *ghc_reg(GHC_PI);
    ncs = ((cap & CAP_NCS_MASK) >> 8) + 1;
    if (ncs == 0 || ncs > N_SLOTS) {
        ncs = 1;
    }
    ah.nslots = ncs;
    ah.depth = 1;
    kprintf("[AHCI] HBA CAP=0x%x VS=0x%x PI=0x%x NCS=%u at %u:%u.%u ABAR=0x%x\n",
            (u64)cap, (u64)vs, (u64)pi, (u64)ncs,
            (u64)bus, (u64)slot, (u64)func, (u64)abar);

    found = 0xFFFFFFFFu;
    for (port = 0; port < 32; port++) {
        u32 ssts;
        if (((pi >> port) & 1u) == 0) {
            continue;
        }
        ah.port = port;
        ssts = *port_reg(PxSSTS);
        if ((ssts & SSTS_DET_MASK) == SSTS_DET_PRESENT) {
            found = port;
            break;
        }
    }
    if (found == 0xFFFFFFFFu) {
        kprintf("[AHCI] no device present on implemented ports\n");
        return;
    }
    ah.port = found;

    arena = mem.pmm_alloc_pages(ARENA_ORDER);
    if (arena == 0) {
        kprintf("[AHCI] arena alloc failed\n");
        return;
    }
    ah.arena_phys = arena;
    for (z = 0; z < (1u << ARENA_ORDER) * 4096u; z++) {
        hhdm8(arena)[z] = 0;
    }

    ahci_stop_port();
    *port_reg(PxCLB) = (u32)((arena + CL_OFF) & 0xFFFFFFFFu);
    *port_reg(PxCLBU) = (u32)((arena + CL_OFF) >> 32);
    *port_reg(PxFB) = (u32)((arena + FIS_OFF) & 0xFFFFFFFFu);
    *port_reg(PxFBU) = (u32)((arena + FIS_OFF) >> 32);
    *port_reg(PxSERR) = 0xFFFFFFFFu;
    *port_reg(PxIS) = 0xFFFFFFFFu;
    if (!ahci_start_port()) {
        kprintf("[AHCI] failed to start port %u\n", (u64)ah.port);
        return;
    }

    /* IDENTIFY always uses non-NCQ DMA path (ah.ncq still 0). */
    if (!ahci_issue((u8)ATA_CMD_IDENT, 0, 1, arena + IDENT_OFF, 0)) {
        kprintf("[AHCI] IDENTIFY failed on port %u\n", (u64)ah.port);
        ahci_stop_port();
        return;
    }

    id = (u16 *)(void *)hhdm8(arena + IDENT_OFF);
    {
        u64 n48 = (u64)id[100] | ((u64)id[101] << 16)
                | ((u64)id[102] << 32) | ((u64)id[103] << 48);
        u64 n28 = (u64)id[60] | ((u64)id[61] << 16);
        if (n48 != 0) {
            ah.capacity = n48;
        } else {
            ah.capacity = n28;
        }
    }
    if (ah.capacity == 0) {
        kprintf("[AHCI] IDENTIFY reported 0 sectors\n");
        ahci_stop_port();
        return;
    }

    /* CAP.SNCQ + IDENTIFY word 76 bit 8 (NCQ supported). */
    if ((cap & CAP_SNCQ) != 0 && (id[76] & 0x0100) != 0) {
        ah.ncq = 1;
        ah.depth = ASYNC_DEPTH;
        if (ah.depth > ah.nslots) {
            ah.depth = ah.nslots;
        }
        if (ah.depth < 1) {
            ah.depth = 1;
        }
    }

    *port_reg(PxIS) = 0xFFFFFFFFu;
    *ghc_reg(GHC_IS) = 0xFFFFFFFFu;
    *port_reg(PxIE) = PxIE_MASK;
    {
        u32 g = *ghc_reg(GHC_GHC);
        *ghc_reg(GHC_GHC) = g | GHC_AE | GHC_IE;
    }

    ah.msi = 0;
    ah.irq_armed = 0;
    ah.pci_irq = 0;
    if (pci_msi_enable(bus, slot, func, (u8)AHCI_IRQ_VECTOR)
        || pci_msix_enable_one(bus, slot, func, (u8)AHCI_IRQ_VECTOR)) {
        ah.msi = 1;
        ah.irq_armed = 1;
        kprintf("[AHCI] MSI/MSI-X armed (vector %u)\n", (u64)AHCI_IRQ_VECTOR);
    } else {
        ah.pci_irq = pci_read8(bus, slot, func, (u8)PCI_INTERRUPT_LINE);
        if (ah.pci_irq != 0 && ah.pci_irq != 0xFF) {
            ioapic_route_irq_level((u32)ah.pci_irq, AHCI_IRQ_VECTOR);
            ah.irq_armed = 1;
            kprintf("[AHCI] INTx armed (irq %u -> vector %u)\n",
                    (u64)ah.pci_irq, (u64)AHCI_IRQ_VECTOR);
        }
    }

    ah.present = 1;
    kprintf("[AHCI] port %u ready: capacity=%u sectors (%u KiB)%s%s%s\n",
            (u64)ah.port, ah.capacity, ah.capacity / 2,
            ah.irq_armed ? " [IRQ]" : " [poll]",
            ah.ncq ? " [NCQ]" : "",
            ah.ncq ? " [async+bioq]" : "");
    if (ah.ncq) {
        kprintf("[AHCI] async NCQ depth=%u bounce=%u secs/slot bioq=%u\n",
                (u64)ah.depth, (u64)BOUNCE_MAX_SEC, (u64)BIO_Q);
    }
}
