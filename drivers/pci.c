package drivers;
import arch;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* ---------------------------------------------------------------------------
 * PCI configuration space via legacy ports 0xCF8 / 0xCFC (type-1).
 *
 * Enough to enumerate bus 0 and locate a VirtIO-blk function by vendor /
 * device id (or AHCI by class 01.06), then read BARs and enable I/O +
 * bus-master in the command register. VirtIO modern vendor caps, MSI (single-message), and MSI-X (one
 * table entry) can be programmed; no bridges yet — see docs/blk-modern-path.md.
 * ------------------------------------------------------------------------- */

enum {
    PCI_CONFIG_ADDR = 0x0CF8,
    PCI_CONFIG_DATA = 0x0CFC,

    PCI_VENDOR_ID   = 0x00,
    PCI_DEVICE_ID   = 0x02,
    PCI_COMMAND     = 0x04,
    PCI_STATUS      = 0x06,
    PCI_CLASS_REV   = 0x08,
    PCI_HEADER_TYPE = 0x0E,
    PCI_BAR0        = 0x10,
    PCI_BAR1        = 0x14,
    PCI_BAR2        = 0x18,
    PCI_BAR3        = 0x1C,
    PCI_BAR4        = 0x20,
    PCI_BAR5        = 0x24,
    PCI_SUBSYSTEM   = 0x2C,
    PCI_CAP_PTR     = 0x34,
    PCI_INTERRUPT   = 0x3C,

    PCI_CMD_IO      = 0x0001,
    PCI_CMD_MEM     = 0x0002,
    PCI_CMD_BUSMASTER = 0x0004,
    PCI_CMD_INTX_DISABLE = 0x0400,

    PCI_CAP_MSI     = 0x05,
    PCI_CAP_MSIX    = 0x11,
    PCI_CAP_VNDR    = 0x09,

    VIRTIO_PCI_CAP_COMMON_CFG = 1,
    VIRTIO_PCI_CAP_NOTIFY_CFG = 2,
    VIRTIO_PCI_CAP_ISR_CFG    = 3,
    VIRTIO_PCI_CAP_DEVICE_CFG = 4,
    VIRTIO_PCI_CAP_PCI_CFG    = 5,

    PCI_BAR_IO      = 0x01,
    PCI_BAR_TYPE_MASK = 0x06,
    PCI_BAR_TYPE_64 = 0x04,
    PCI_BAR_IO_MASK = 0xFFFFFFFC,
    PCI_BAR_MEM_MASK = 0xFFFFFFF0
};

static u32 pci_addr(u8 bus, u8 slot, u8 func, u8 offset) {
    return 0x80000000u
         | (((u32)bus) << 16)
         | (((u32)slot) << 11)
         | (((u32)func) << 8)
         | ((u32)(offset & 0xFC));
}

u32 pci_read32(u8 bus, u8 slot, u8 func, u8 offset) {
    arch.outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    return arch.inl(PCI_CONFIG_DATA);
}

void pci_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value) {
    arch.outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    arch.outl(PCI_CONFIG_DATA, value);
}

u16 pci_read16(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 v = pci_read32(bus, slot, func, offset);
    if ((offset & 2) != 0) {
        return (u16)(v >> 16);
    }
    return (u16)(v & 0xFFFF);
}

void pci_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 value) {
    u32 cur = pci_read32(bus, slot, func, offset);
    if ((offset & 2) != 0) {
        cur = (cur & 0x0000FFFF) | (((u32)value) << 16);
    } else {
        cur = (cur & 0xFFFF0000) | (u32)value;
    }
    pci_write32(bus, slot, func, offset, cur);
}

u8 pci_read8(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 v = pci_read32(bus, slot, func, offset);
    return (u8)((v >> ((offset & 3) * 8)) & 0xFF);
}

/* Enable I/O space, memory space and bus mastering for the function. */
void pci_enable_busmaster(u8 bus, u8 slot, u8 func) {
    u16 cmd = pci_read16(bus, slot, func, PCI_COMMAND);
    cmd = (u16)(cmd | PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_BUSMASTER);
    pci_write16(bus, slot, func, PCI_COMMAND, cmd);
}

/* Decode BAR0. Sets *is_io to 1 for an I/O BAR (port in *addr) or 0 for a
 * 32-bit memory BAR (physical address in *addr). 64-bit memory BARs return
 * the low 32 bits only (enough for QEMU VirtIO BARs under 4 GiB). Returns 0
 * when the BAR is unused. */
/* Decode BARn (0..5). Sets *is_io / *addr like the old pci_bar0 helper. */
u32 pci_bar(u8 bus, u8 slot, u8 func, u8 index, u32 *addr, u32 *is_io) {
    u8 off;
    u32 bar;
    if (index > 5) {
        return 0;
    }
    off = (u8)(PCI_BAR0 + index * 4);
    bar = pci_read32(bus, slot, func, off);
    if (bar == 0 || bar == 0xFFFFFFFF) {
        return 0;
    }
    if ((bar & PCI_BAR_IO) != 0) {
        *is_io = 1;
        *addr = bar & PCI_BAR_IO_MASK;
        return 1;
    }
    *is_io = 0;
    *addr = bar & PCI_BAR_MEM_MASK;
    return 1;
}

u32 pci_bar0(u8 bus, u8 slot, u8 func, u32 *addr, u32 *is_io) {
    return pci_bar(bus, slot, func, 0, addr, is_io);
}

/* 64-bit memory BAR decode (index 0..5). *addr gets full phys; *is_io set. */
u32 pci_bar64(u8 bus, u8 slot, u8 func, u8 index, u64 *addr, u32 *is_io) {
    u8 off;
    u32 bar;
    u32 bar_hi;
    if (index > 5 || addr == 0 || is_io == 0) {
        return 0;
    }
    off = (u8)(PCI_BAR0 + index * 4);
    bar = pci_read32(bus, slot, func, off);
    if (bar == 0 || bar == 0xFFFFFFFF) {
        return 0;
    }
    if ((bar & PCI_BAR_IO) != 0) {
        *is_io = 1;
        *addr = (u64)(bar & PCI_BAR_IO_MASK);
        return 1;
    }
    *is_io = 0;
    *addr = (u64)(bar & PCI_BAR_MEM_MASK);
    if ((bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64 && index < 5) {
        bar_hi = pci_read32(bus, slot, func, (u8)(off + 4));
        *addr |= ((u64)bar_hi) << 32;
    }
    return 1;
}

/* Find first capability with id. Returns config offset or 0. */
u8 pci_find_cap(u8 bus, u8 slot, u8 func, u8 cap_id) {
    u16 status;
    u8 ptr;
    u32 guard;
    status = pci_read16(bus, slot, func, PCI_STATUS);
    if ((status & 0x10) == 0) {
        return 0;
    }
    ptr = pci_read8(bus, slot, func, PCI_CAP_PTR);
    guard = 0;
    while (ptr >= 0x40 && guard < 48) {
        u8 id = pci_read8(bus, slot, func, ptr);
        u8 next = pci_read8(bus, slot, func, (u8)(ptr + 1));
        if (id == cap_id) {
            return ptr;
        }
        if (next == 0 || next == ptr) {
            break;
        }
        ptr = next;
        guard++;
    }
    return 0;
}

/* Map a VirtIO modern vendor cap (cfg_type) into HHDM. Fills *mmio_phys /
 * *len; for NOTIFY also *notify_mult. Returns 1 on success. */
u32 pci_virtio_map_cap(u8 bus, u8 slot, u8 func, u8 cfg_type,
                       u64 *mmio_phys, u32 *len, u32 *notify_mult) {
    u16 status;
    u8 ptr;
    u32 guard;
    static const u64 HHDM = 0xFFFF800000000000ULL;
    (void)HHDM;
    status = pci_read16(bus, slot, func, PCI_STATUS);
    if ((status & 0x10) == 0) {
        return 0;
    }
    ptr = pci_read8(bus, slot, func, PCI_CAP_PTR);
    guard = 0;
    while (ptr >= 0x40 && guard < 48) {
        u8 id = pci_read8(bus, slot, func, ptr);
        u8 next = pci_read8(bus, slot, func, (u8)(ptr + 1));
        if (id == PCI_CAP_VNDR) {
            u8 typ = pci_read8(bus, slot, func, (u8)(ptr + 3));
            if (typ == cfg_type) {
                u8 bar_i = pci_read8(bus, slot, func, (u8)(ptr + 4));
                u32 off = pci_read32(bus, slot, func, (u8)(ptr + 8));
                u32 length = pci_read32(bus, slot, func, (u8)(ptr + 12));
                u64 bar_addr = 0;
                u32 is_io = 0;
                if (!pci_bar64(bus, slot, func, bar_i, &bar_addr, &is_io) || is_io) {
                    return 0;
                }
                if (mmio_phys) {
                    *mmio_phys = bar_addr + (u64)off;
                }
                if (len) {
                    *len = length;
                }
                /* Ensure the BAR window is in HHDM (PCI hole needs a map). */
                if (!mem.vmm_map_mmio(bar_addr + (u64)off,
                                      length != 0 ? length : 4096)) {
                    return 0;
                }
                if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && notify_mult) {
                    *notify_mult = pci_read32(bus, slot, func, (u8)(ptr + 16));
                } else if (notify_mult) {
                    *notify_mult = 0;
                }
                return 1;
            }
        }
        if (next == 0 || next == ptr) {
            break;
        }
        ptr = next;
        guard++;
    }
    return 0;
}

/* Enable single-vector MSI delivering `vector` to this CPU's LAPIC.
 * Returns 1 if MSI was programmed (INTx may still be left alone). */
u32 pci_msi_enable(u8 bus, u8 slot, u8 func, u8 vector) {
    u8 cap;
    u16 msg_ctl;
    u32 addr_lo;
    u16 data;
    extern u32 lapic_get_id(void);
    extern void kprintf(const char *fmt, ...);
    cap = pci_find_cap(bus, slot, func, PCI_CAP_MSI);
    if (cap == 0) {
        return 0;
    }
    msg_ctl = pci_read16(bus, slot, func, (u8)(cap + 2));
    /* 32-bit addr layout when bit7 (64-bit addr) clear; else +4 for hi. */
    addr_lo = 0xFEE00000u | (lapic_get_id() << 12);
    data = (u16)vector;
    if ((msg_ctl & 0x0080) != 0) {
        /* 64-bit address */
        pci_write32(bus, slot, func, (u8)(cap + 4), addr_lo);
        pci_write32(bus, slot, func, (u8)(cap + 8), 0);
        pci_write16(bus, slot, func, (u8)(cap + 12), data);
    } else {
        pci_write32(bus, slot, func, (u8)(cap + 4), addr_lo);
        pci_write16(bus, slot, func, (u8)(cap + 8), data);
    }
    msg_ctl = (u16)((msg_ctl & ~0x0070) | 0x0001); /* enable, 1 vector */
    pci_write16(bus, slot, func, (u8)(cap + 2), msg_ctl);
    {
        u16 cmd = pci_read16(bus, slot, func, PCI_COMMAND);
        pci_write16(bus, slot, func, PCI_COMMAND,
                    (u16)(cmd | PCI_CMD_INTX_DISABLE));
    }
    kprintf("[PCI] MSI enabled %u:%u.%u vector=%u\n",
            (u64)bus, (u64)slot, (u64)func, (u64)vector);
    return 1;
}

/* Enable MSI-X with a single unmasked table entry delivering `vector`.
 * Returns 1 on success. Disables INTx. Does not touch MSI (caller may prefer
 * MSI-X exclusively; QEMU virtio accepts either). */
u32 pci_msix_enable_one(u8 bus, u8 slot, u8 func, u8 vector) {
    u8 cap;
    u16 msg_ctl;
    u32 table_info;
    u8 bir;
    u32 table_off;
    u64 bar_addr = 0;
    u32 is_io = 0;
    u64 entry_phys;
    volatile u32 *entry;
    u32 addr_lo;
    extern u32 lapic_get_id(void);
    extern void kprintf(const char *fmt, ...);
    static const u64 HHDM = 0xFFFF800000000000ULL;

    cap = pci_find_cap(bus, slot, func, PCI_CAP_MSIX);
    if (cap == 0) {
        return 0;
    }
    msg_ctl = pci_read16(bus, slot, func, (u8)(cap + 2));
    table_info = pci_read32(bus, slot, func, (u8)(cap + 4));
    bir = (u8)(table_info & 0x7);
    table_off = table_info & ~0x7u;
    if (!pci_bar64(bus, slot, func, bir, &bar_addr, &is_io) || is_io || bar_addr == 0) {
        return 0;
    }
    entry_phys = bar_addr + (u64)table_off; /* entry 0 */
    if (!mem.vmm_map_mmio(entry_phys, 16)) {
        return 0;
    }
    entry = (volatile u32 *)(HHDM + entry_phys);
    addr_lo = 0xFEE00000u | (lapic_get_id() << 12);
    entry[0] = addr_lo;
    entry[1] = 0;
    entry[2] = (u32)vector;          /* data: vector, edge, fixed */
    entry[3] = 0;                    /* unmasked */

    msg_ctl = (u16)(msg_ctl | 0x8000); /* MSI-X Enable */
    msg_ctl = (u16)(msg_ctl & ~(u16)0x4000); /* clear Function Mask */
    pci_write16(bus, slot, func, (u8)(cap + 2), msg_ctl);
    {
        u16 cmd = pci_read16(bus, slot, func, PCI_COMMAND);
        pci_write16(bus, slot, func, PCI_COMMAND,
                    (u16)(cmd | PCI_CMD_INTX_DISABLE));
    }
    kprintf("[PCI] MSI-X enabled %u:%u.%u vector=%u table_bir=%u off=0x%x\n",
            (u64)bus, (u64)slot, (u64)func, (u64)vector, (u64)bir, (u64)table_off);
    return 1;
}

/* Walk PCI caps; log VirtIO modern capability ids (9) when present. */
void pci_log_virtio_modern_caps(u8 bus, u8 slot, u8 func) {
    u16 status;
    u8 ptr;
    u32 guard;
    extern void kprintf(const char *fmt, ...);
    status = pci_read16(bus, slot, func, PCI_STATUS);
    if ((status & 0x10) == 0) {
        return; /* capabilities list not present */
    }
    ptr = pci_read8(bus, slot, func, PCI_CAP_PTR);
    guard = 0;
    while (ptr >= 0x40 && guard < 16) {
        u8 id = pci_read8(bus, slot, func, ptr);
        u8 next = pci_read8(bus, slot, func, (u8)(ptr + 1));
        if (id == 0x09) {
            /* Vendor-specific: VirtIO modern uses cfg_type at +3 */
            u8 cfg_type = pci_read8(bus, slot, func, (u8)(ptr + 3));
            kprintf("[PCI] virtio modern cap at 0x%x cfg_type=%u\n",
                    (u64)ptr, (u64)cfg_type);
        }
        if (next == 0 || next == ptr) {
            break;
        }
        ptr = next;
        guard++;
    }
}

/* Scan bus 0 for (vendor, device). On hit, fill bus/slot/func and return 1. */
u32 pci_find_device(u16 vendor, u16 device, u8 *obus, u8 *oslot, u8 *ofunc) {
    u8 slot;
    u8 func;
    for (slot = 0; slot < 32; slot++) {
        u16 ven = pci_read16(0, slot, 0, PCI_VENDOR_ID);
        u8 header;
        u8 max_func;
        if (ven == 0xFFFF) {
            continue;
        }
        header = pci_read8(0, slot, 0, PCI_HEADER_TYPE);
        max_func = ((header & 0x80) != 0) ? 8 : 1;
        for (func = 0; func < max_func; func++) {
            u16 v = pci_read16(0, slot, func, PCI_VENDOR_ID);
            u16 d;
            if (v == 0xFFFF) {
                continue;
            }
            d = pci_read16(0, slot, func, PCI_DEVICE_ID);
            if (v == vendor && d == device) {
                *obus = 0;
                *oslot = slot;
                *ofunc = func;
                return 1;
            }
        }
    }
    return 0;
}

/* Same scan but match vendor + PCI subsystem device id (used by modern
 * VirtIO transitional devices that share device id 0x1001..0x1009 legacy
 * range OR the 0x1040+ modern range; subsystem id identifies the device
 * class: 1 = net, 2 = blk, ...). Returns 1 on hit. */
u32 pci_find_vendor_subsystem(u16 vendor, u16 subsystem,
                              u8 *obus, u8 *oslot, u8 *ofunc) {
    u8 slot;
    u8 func;
    for (slot = 0; slot < 32; slot++) {
        u16 ven = pci_read16(0, slot, 0, PCI_VENDOR_ID);
        u8 header;
        u8 max_func;
        if (ven == 0xFFFF) {
            continue;
        }
        header = pci_read8(0, slot, 0, PCI_HEADER_TYPE);
        max_func = ((header & 0x80) != 0) ? 8 : 1;
        for (func = 0; func < max_func; func++) {
            u16 v = pci_read16(0, slot, func, PCI_VENDOR_ID);
            u16 sub;
            if (v != vendor) {
                continue;
            }
            sub = pci_read16(0, slot, func, PCI_SUBSYSTEM + 2);
            if (sub == subsystem) {
                *obus = 0;
                *oslot = slot;
                *ofunc = func;
                return 1;
            }
        }
    }
    return 0;
}

/* Match PCI base-class / subclass (from CLASS_REV dword). */
u32 pci_find_class(u8 base_class, u8 subclass, u8 *obus, u8 *oslot, u8 *ofunc) {
    u8 slot;
    u8 func;
    for (slot = 0; slot < 32; slot++) {
        u16 ven = pci_read16(0, slot, 0, PCI_VENDOR_ID);
        u8 header;
        u8 max_func;
        if (ven == 0xFFFF) {
            continue;
        }
        header = pci_read8(0, slot, 0, PCI_HEADER_TYPE);
        max_func = ((header & 0x80) != 0) ? 8 : 1;
        for (func = 0; func < max_func; func++) {
            u32 crev;
            u8 cls;
            u8 sub;
            if (pci_read16(0, slot, func, PCI_VENDOR_ID) == 0xFFFF) {
                continue;
            }
            crev = pci_read32(0, slot, func, PCI_CLASS_REV);
            cls = (u8)((crev >> 24) & 0xFF);
            sub = (u8)((crev >> 16) & 0xFF);
            if (cls == base_class && sub == subclass) {
                *obus = 0;
                *oslot = slot;
                *ofunc = func;
                return 1;
            }
        }
    }
    return 0;
}
