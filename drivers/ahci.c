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

/* ---------------------------------------------------------------------------
 * AHCI (SATA) driver: one port, multi-slot command list.
 *
 * Target: QEMU `-device ahci,id=ahci0 -device ide-hd,drive=...,bus=ahci0.0`
 * with PCI class 01.06 ABAR (BAR5). DMA via HHDM + low physical frames.
 * Completion: MSI/MSI-X (vector 42) or INTx with sched_sleep; poll fallback.
 *
 * NCQ-lite (coherent with sync blk API): when CAP.SNCQ and IDENTIFY NCQ are
 * set, issue READ/WRITE FPDMA QUEUED with SACT+CI on a free slot. Public
 * read/write still serialize on one shared bounce buffer (one outstanding),
 * so multi-slot is foundation for later async I/O rather than parallel DMA.
 * See docs/blk-modern-path.md.
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

    /* Arena: CL(1KiB) + FIS(256) + CT*32(8KiB) + IDENT(512) + bounce(8KiB) */
    ARENA_ORDER     = 4,             /* 16 pages = 64 KiB */
    N_SLOTS         = 32,
    CT_SIZE         = 256,
    CL_OFF          = 0,
    FIS_OFF         = 1024,
    CT_BASE         = 1024 + 256,
    IDENT_OFF       = 1024 + 256 + 32 * 256,
    BOUNCE_OFF      = 1024 + 256 + 32 * 256 + 512,
    BOUNCE_MAX_SEC  = 16,

    POLL_SPINS      = 5000000,

    AHCI_IRQ_VECTOR = 42,
    PCI_INTERRUPT_LINE = 0x3C,

    PxIE_MASK       = 0x07C0007Fu,
    GHC_IE          = 0x00000002u
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
    u32 busy;                        /* bounce-buffer serialize */
    u8  pci_irq;
};

static struct ahci ah;

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

/* Wait for slot bit to clear in CI (and SACT when NCQ). */
static u32 ahci_wait_slot(u8 ata_cmd, u32 slot) {
    u32 spins = 0;
    u32 bit = 1u << slot;
    while (((*port_reg(PxCI) & bit) != 0)
           || (ah.ncq && (*port_reg(PxSACT) & bit) != 0)) {
        u32 tfd = *port_reg(PxTFD);
        u32 is = *port_reg(PxIS);
        if ((tfd & 0x01) != 0) {
            kprintf("[AHCI] issue ERR cmd=0x%x TFD=0x%x IS=0x%x\n",
                    (u64)ata_cmd, (u64)tfd, (u64)is);
            *port_reg(PxIS) = 0xFFFFFFFFu;
            *ghc_reg(GHC_IS) = 0xFFFFFFFFu;
            return 0;
        }
        if ((is & 0x40000000u) != 0) {
            kprintf("[AHCI] TFES cmd=0x%x IS=0x%x\n", (u64)ata_cmd, (u64)is);
            *port_reg(PxIS) = 0xFFFFFFFFu;
            *ghc_reg(GHC_IS) = 0xFFFFFFFFu;
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
    sched_wakeup(AHCI_WAIT_CHAN);
    return 1;
}

static u32 ahci_pick_slot(void) {
    u32 busy = *port_reg(PxCI) | *port_reg(PxSACT);
    u32 i;
    u32 lim = ah.nslots != 0 ? ah.nslots : 1;
    if (lim > N_SLOTS) {
        lim = N_SLOTS;
    }
    for (i = 0; i < lim; i++) {
        if (((busy >> i) & 1u) == 0) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

/* Build H2D FIS + PRDT on `slot` and ring CI (and SACT for NCQ). */
static u32 ahci_issue(u8 ata_cmd, u64 lba, u16 count, u64 buf_phys, u32 write) {
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

    slot = ahci_pick_slot();
    if (slot == 0xFFFFFFFFu) {
        kprintf("[AHCI] no free command slot\n");
        return 0;
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

    if (!ahci_wait_slot(cmd, slot)) {
        return 0;
    }
    return 1;
}

u32 ahci_present_p(void) {
    return ah.present;
}

u64 ahci_capacity(void) {
    return ah.capacity;
}

u32 ahci_read(u64 sector, void *kbuf, u32 count) {
    u8 *dst = (u8 *)kbuf;
    u32 done = 0;
    if (!ah.present || count == 0) {
        return 0;
    }
    if (sector + (u64)count > ah.capacity) {
        return 0;
    }
    while (done < count) {
        u32 n = count - done;
        u64 bounce = ah.arena_phys + BOUNCE_OFF;
        u32 b;
        if (n > BOUNCE_MAX_SEC) {
            n = BOUNCE_MAX_SEC;
        }
        while (ah.busy) {
            cpu_pause();
        }
        ah.busy = 1;
        if (!ahci_issue((u8)ATA_CMD_READ_DMA_EXT, sector + (u64)done, (u16)n,
                        bounce, 0)) {
            ah.busy = 0;
            return 0;
        }
        for (b = 0; b < n * SECTOR_SIZE; b++) {
            dst[(u64)done * SECTOR_SIZE + b] = hhdm8(bounce)[b];
        }
        ah.busy = 0;
        done += n;
    }
    return 1;
}

u32 ahci_write(u64 sector, const void *kbuf, u32 count) {
    const u8 *src = (const u8 *)kbuf;
    u32 done = 0;
    if (!ah.present || count == 0) {
        return 0;
    }
    if (sector + (u64)count > ah.capacity) {
        return 0;
    }
    while (done < count) {
        u32 n = count - done;
        u64 bounce = ah.arena_phys + BOUNCE_OFF;
        u32 b;
        if (n > BOUNCE_MAX_SEC) {
            n = BOUNCE_MAX_SEC;
        }
        for (b = 0; b < n * SECTOR_SIZE; b++) {
            hhdm8(bounce)[b] = src[(u64)done * SECTOR_SIZE + b];
        }
        while (ah.busy) {
            cpu_pause();
        }
        ah.busy = 1;
        if (!ahci_issue((u8)ATA_CMD_WRITE_DMA_EXT, sector + (u64)done, (u16)n,
                        bounce, 1)) {
            ah.busy = 0;
            return 0;
        }
        ah.busy = 0;
        done += n;
    }
    return 1;
}

u32 ahci_flush(void) {
    u64 bounce;
    if (!ah.present) {
        return 0;
    }
    bounce = ah.arena_phys + IDENT_OFF;
    {
        u64 cl_phys = ah.arena_phys + CL_OFF;
        u64 ct_phys = ct_phys_for(0);
        u64 fis_phys = ct_phys;
        u64 prdt_phys = ct_phys + 0x80;
        u32 i;
        while (ah.busy) {
            cpu_pause();
        }
        ah.busy = 1;
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
        if (!ahci_wait_slot((u8)ATA_CMD_FLUSH, 0)) {
            ah.busy = 0;
            return 0;
        }
        ah.busy = 0;
    }
    return 1;
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

    ah.present = 0;
    ah.capacity = 0;
    ah.abar_phys = 0;
    ah.arena_phys = 0;
    ah.port = 0;
    ah.msi = 0;
    ah.irq_armed = 0;
    ah.ncq = 0;
    ah.nslots = 1;
    ah.busy = 0;
    ah.pci_irq = 0;

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
    kprintf("[AHCI] port %u ready: capacity=%u sectors (%u KiB)%s%s\n",
            (u64)ah.port, ah.capacity, ah.capacity / 2,
            ah.irq_armed ? " [IRQ]" : " [poll]",
            ah.ncq ? " [NCQ]" : "");
}
