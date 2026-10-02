package arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern u8 inb(u16 port);
extern void outb(u16 port, u8 val);
extern u64 msr_read(u32 msr);
extern void msr_write(u32 msr, u64 value);
extern void tlb_flush(void);

/* Boot page table exported by arch/x86_64/boot.asm (.boot.bss, identity
 * mapped in low memory). We extend it with one 4 KiB page table that maps the
 * Local APIC MMIO window into the high-half kernel address space. */
extern u8 boot_pd[];

static const u64 KERNEL_VMA_BASE = 0xFFFFFFFF80000000ULL;
/* Fixed high-half virtual address of the LAPIC MMIO page. Its offset inside
 * the boot 1 GiB PDPT window is 256 MiB -> PD index 128, which is unused by
 * the initial 8 x 2 MiB identity mappings. */
static const u64 LAPIC_VIRT = 0xFFFFFFFF90000000ULL;

enum {
    LAPIC_PD_INDEX     = 128,

    IA32_APIC_BASE     = 0x0000001B,
    APIC_BASE_ENABLE   = 0x00000800,

    LAPIC_REG_ID       = 0x020,
    LAPIC_REG_EOI      = 0x0B0,
    LAPIC_REG_SVR      = 0x0F0,
    LAPIC_REG_LVTT     = 0x320,
    LAPIC_REG_INITCNT  = 0x380,
    LAPIC_REG_CURCNT   = 0x390,
    LAPIC_REG_DIVCONF  = 0x3E0,

    LAPIC_LVT_MASKED   = 0x00010000,
    LAPIC_LVT_PERIODIC = 0x00020000,
    LAPIC_TIMER_VECTOR = 32,

    PIT_FREQ_HZ        = 1193182,
    CALIBRATE_MS       = 40,                 /* <= 55 ms PIT 16-bit limit */
    WAIT_500MS_MS      = 500
};

static const u32 LAPIC_TIMER_MAXCOUNT = 0xFFFFFFFF;

static volatile u32 *lapic_regs;
static volatile u64 lapic_ticks;

/* Oversized, page-aligned at runtime: the first 4 KiB-aligned window inside
 * this storage becomes the APIC page table (fakecc emits 16-byte-aligned
 * BSS, so alignment is performed explicitly in code). */
static u64 lapic_pt_storage[512 + 64];

static u32 lapic_rd(u32 offset) {
    return lapic_regs[offset / 4];
}

static void lapic_wr(u32 offset, u32 value) {
    lapic_regs[offset / 4] = value;
}

/* Program PIT channel 2 in one-shot mode 0 for `counts` input clocks.
 * GATE2 (port 0x61 bit0) gates counting; OUT2 (bit5) rises at terminal count.
 * Channel 2 is deliberately chosen because it needs no IRQ wiring. */
static void pit_ch2_oneshot(u32 counts) {
    u8 p = inb(0x61);
    p = (u8)((p | 0x01) & 0xFD);        /* GATE2=1, speaker data=0 */
    outb(0x61, p);
    outb(0x43, 0xB0);                   /* Ch2, lo/hi access, mode 0, binary */
    outb(0x42, (u8)(counts & 0xFF));
    outb(0x42, (u8)((counts >> 8) & 0xFF));
}

static void pit_wait_terminal(void) {
    while ((inb(0x61) & 0x20) == 0) {
    }
}

/* The 8254 PIT counter is 16-bit (max 65536 input clocks, ~55 ms), so long
 * waits are built from repeated <= 40 ms one-shot rounds. Public so kernel
 * phases can wall-clock themselves while the scheduler preempts. */
void pit_busy_wait_ms(u32 ms) {
    while (ms > 0) {
        u32 chunk = (ms > 40) ? 40 : ms;
        pit_ch2_oneshot((PIT_FREQ_HZ / 1000) * chunk);
        pit_wait_terminal();
        ms -= chunk;
    }
}

void pit_busy_wait_500ms(void) {
    pit_busy_wait_ms(WAIT_500MS_MS);
}

void lapic_irq(void) {
    lapic_wr(LAPIC_REG_EOI, 0);
    lapic_ticks++;
}

/* Returns 1 if the spurious vector (255, ISR word 7 bit 31) is genuinely
 * in-service. A *true* spurious interrupt does not set ISR and needs no EOI;
 * but a stray pulse accepted as a normal fixed interrupt at vector 255 does
 * set ISR and, left open, parks PPR at priority 15 and blocks every lower
 * interrupt (including the timer). */
int lapic_spurious_in_service(void) {
    return (lapic_rd(0x170) & 0x80000000u) ? 1 : 0;
}

void lapic_send_eoi(void) {
    lapic_wr(LAPIC_REG_EOI, 0);
}

u64 lapic_get_ticks(void) {
    return lapic_ticks;
}

u32 lapic_get_id(void) {
    return lapic_rd(LAPIC_REG_ID) >> 24;
}

void lapic_init(void) {
    /* ---- 1. Map the LAPIC MMIO page into high-half virtual memory ---- */
    u64 phys_pt = (u64)(&lapic_pt_storage[0]);
    phys_pt = (phys_pt + 4095) & ~4095ULL;

    u64 *pt = (u64 *)phys_pt;
    pt[0] = 0;

    /* boot_pd symbol's VMA is its low physical address (identity mapped). */
    u64 *pd = (u64 *)(&boot_pd[0]);
    pd[LAPIC_PD_INDEX] = (phys_pt - KERNEL_VMA_BASE) | 0x03; /* Present|RW */

    u64 apic_pa = msr_read(IA32_APIC_BASE);
    apic_pa = apic_pa & 0x0000000FFFFF000ULL;
    pt[0] = apic_pa | 0x1F;   /* Present|RW|PWT|PCD (strong uncacheable) */
    tlb_flush();

    lapic_regs = (volatile u32 *)LAPIC_VIRT;

    /* ---- 2. Software-enable the Local APIC ---- */
    u64 base_msr = msr_read(IA32_APIC_BASE);
    msr_write(IA32_APIC_BASE, base_msr | APIC_BASE_ENABLE);

    /* Mask every unused LVT interrupt source so stray asserts during
     * enable/PIC-mask sequencing cannot arrive as bogus fixed vectors. */
    lapic_wr(0x340, LAPIC_LVT_MASKED | 0xFF);   /* Thermal Monitor */
    lapic_wr(0x350, LAPIC_LVT_MASKED | 0xFF);   /* LINT0   */
    lapic_wr(0x360, LAPIC_LVT_MASKED | 0xFF);   /* LINT1   */
    lapic_wr(0x370, LAPIC_LVT_MASKED | 0xFF);   /* Error   */

    /* Spurious vector 0xFF (matches the IDT stub), APIC enabled. */
    lapic_wr(LAPIC_REG_SVR, 0x000001FF);
    /* Clear any latched APIC error status before interrupts go live. */
    lapic_wr(0x280, 0);
    (void)lapic_rd(0x280);

    /* ---- 3. Calibrate timer frequency against the PIT (100 ms window) ---- */
    lapic_wr(LAPIC_REG_DIVCONF, 0x03);              /* Divide by 16 */
    lapic_wr(LAPIC_REG_LVTT, LAPIC_LVT_MASKED | LAPIC_TIMER_VECTOR);
    lapic_wr(LAPIC_REG_INITCNT, LAPIC_TIMER_MAXCOUNT);

    pit_ch2_oneshot((PIT_FREQ_HZ / 1000) * CALIBRATE_MS);
    pit_wait_terminal();

    u32 remaining = lapic_rd(LAPIC_REG_CURCNT);
    u32 consumed = LAPIC_TIMER_MAXCOUNT - remaining;
    /* consumed covers CALIBRATE_MS; scale to counts per 10 ms (100 Hz). */
    u32 period100hz = consumed / (CALIBRATE_MS / 10);
    if (period100hz == 0) {
        period100hz = 1;
    }

    /* ---- 4. Start periodic 100 Hz timer interrupts on vector 32 ---- */
    lapic_wr(LAPIC_REG_LVTT, LAPIC_LVT_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_wr(LAPIC_REG_INITCNT, period100hz);
}
