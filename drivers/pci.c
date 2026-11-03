package drivers;
import arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* ---------------------------------------------------------------------------
 * PCI configuration space via legacy ports 0xCF8 / 0xCFC (type-1).
 *
 * Enough to enumerate bus 0 and locate a VirtIO-blk function by vendor /
 * device id, then read BARs and enable I/O + bus-master in the command
 * register. No bridges, MSI or capabilities walk yet.
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
u32 pci_bar0(u8 bus, u8 slot, u8 func, u32 *addr, u32 *is_io) {
    u32 bar = pci_read32(bus, slot, func, PCI_BAR0);
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
