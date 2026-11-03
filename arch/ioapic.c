package arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void tlb_flush(void);
extern u8 boot_pd[];
extern u32 lapic_get_id(void);
extern void kprintf(const char *fmt, ...);

/* I/O APIC at the legacy PC default (QEMU/Bochs). Mapped beside the LAPIC
 * window in the high-half kernel address space. ISA IRQs 0..15 are identity
 * routed on the first I/O APIC for the QEMU pc machine. */

static const u64 KERNEL_VMA_BASE = 0xFFFFFFFF80000000ULL;
static const u64 IOAPIC_VIRT     = 0xFFFFFFFF90200000ULL;
static const u64 IOAPIC_PHYS     = 0x00000000FEC00000ULL;

enum {
    IOAPIC_PD_INDEX = 129,           /* next 2 MiB slot after LAPIC (128) */

    IOAPIC_REGSEL   = 0x00,
    IOAPIC_WIN      = 0x10,

    IOAPIC_ID       = 0x00,
    IOAPIC_VER      = 0x01,
    IOAPIC_ARB      = 0x02,
    IOAPIC_REDTBL   = 0x10,

    IOAPIC_MASK     = 0x00010000,
    IOAPIC_DEST_SHIFT = 24
};

static volatile u32 *ioapic_regs;
static u32 ioapic_max_redir;
static u64 ioapic_pt_storage[512 + 64];

static u32 ioapic_read(u32 reg) {
    ioapic_regs[IOAPIC_REGSEL / 4] = reg;
    return ioapic_regs[IOAPIC_WIN / 4];
}

static void ioapic_write(u32 reg, u32 val) {
    ioapic_regs[IOAPIC_REGSEL / 4] = reg;
    ioapic_regs[IOAPIC_WIN / 4] = val;
}

void ioapic_init(void) {
    u64 phys_pt = (u64)(&ioapic_pt_storage[0]);
    u64 *pt;
    u64 *pd;
    u32 ver;
    u32 i;

    phys_pt = (phys_pt + 4095) & ~4095ULL;
    pt = (u64 *)phys_pt;
    pt[0] = IOAPIC_PHYS | 0x1F;          /* Present|RW|PWT|PCD */

    pd = (u64 *)(&boot_pd[0]);
    pd[IOAPIC_PD_INDEX] = (phys_pt - KERNEL_VMA_BASE) | 0x03;
    tlb_flush();

    ioapic_regs = (volatile u32 *)IOAPIC_VIRT;
    ver = ioapic_read(IOAPIC_VER);
    ioapic_max_redir = ((ver >> 16) & 0xFF);

    /* Mask every redirection entry before enabling any device line. */
    for (i = 0; i <= ioapic_max_redir; i++) {
        ioapic_write(IOAPIC_REDTBL + 2 * i, IOAPIC_MASK | 0xFF);
        ioapic_write(IOAPIC_REDTBL + 2 * i + 1, 0);
    }

    kprintf("[INT] I/O APIC mapped at %p (phys %p), %u redirection entries\n",
            IOAPIC_VIRT, IOAPIC_PHYS, (u64)(ioapic_max_redir + 1));
}

/* Route ISA IRQ `irq` to IDT vector `vector` targeting this CPU's LAPIC.
 * Edge-triggered, active-high, fixed delivery, physical destination. */
void ioapic_route_irq(u32 irq, u32 vector) {
    u32 dest;
    u32 low;
    u32 high;
    if (ioapic_regs == 0 || irq > ioapic_max_redir || vector < 32 || vector > 255) {
        return;
    }
    dest = lapic_get_id();
    high = dest << IOAPIC_DEST_SHIFT;
    low = (vector & 0xFF);               /* unmasked, edge, active-high */
    /* Program destination first, then unmask with the vector. */
    ioapic_write(IOAPIC_REDTBL + 2 * irq + 1, high);
    ioapic_write(IOAPIC_REDTBL + 2 * irq, low);
}

void ioapic_mask_irq(u32 irq) {
    u32 low;
    if (ioapic_regs == 0 || irq > ioapic_max_redir) {
        return;
    }
    low = ioapic_read(IOAPIC_REDTBL + 2 * irq);
    ioapic_write(IOAPIC_REDTBL + 2 * irq, low | IOAPIC_MASK);
}

/* PCI INTx: level-triggered, active-low (IOAPIC bits 15 and 13). */
void ioapic_route_irq_level(u32 irq, u32 vector) {
    u32 dest;
    u32 low;
    u32 high;
    if (ioapic_regs == 0 || irq > ioapic_max_redir || vector < 32 || vector > 255) {
        return;
    }
    dest = lapic_get_id();
    high = dest << IOAPIC_DEST_SHIFT;
    low = (vector & 0xFF) | (1u << 13) | (1u << 15);  /* active-low + level */
    ioapic_write(IOAPIC_REDTBL + 2 * irq + 1, high);
    ioapic_write(IOAPIC_REDTBL + 2 * irq, low);
}
