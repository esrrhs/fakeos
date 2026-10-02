package arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void gdt_flush(void);
extern void tss_load(u16 selector);

/* Global Descriptor Table (referenced by gdtr in desc.asm):
 *   0x00 null
 *   0x08 kernel 64-bit code (DPL0, L=1)
 *   0x10 kernel data        (DPL0)
 *   0x18 user data          (DPL3, reserved for future SYSCALL layout)
 *   0x20 user 64-bit code   (DPL3, L=1)
 *   0x28 64-bit TSS descriptor (occupies selectors 0x28 + 0x30) */
u64 gdt[7];

/* 64-bit Task State Segment, 104 bytes, addressed as raw 32-bit words so the
 * naturally-unaligned RSP0 (offset 4) and IST (offset 36) fields are exact. */
u32 tss[26];

/* Ring0 privilege-transition stack (CPL3 -> CPL0 via future SYSCALL/IRET). */
static u64 tss_rsp0_stack[4096];

/* IST1: dedicated guard stack for #DF so a kernel stack overflow can still be
 * captured. 16 KiB, naturally 16-byte aligned in .bss. */
static u64 tss_ist1_stack[2048];

static u64 rsp0_value;
static u64 ist1_value;

void gdt_init(void) {
    int i;
    for (i = 0; i < 7; i++) {
        gdt[i] = 0;
    }
    for (i = 0; i < 26; i++) {
        tss[i] = 0;
    }

    gdt[1] = 0x00209A0000000000ULL; /* Kernel code: P DPL0 S Exec/Read, L=1 */
    gdt[2] = 0x0000920000000000ULL; /* Kernel data: P DPL0 S Data/Write      */
    gdt[3] = 0x00CFF20000000000ULL; /* User data:   P DPL3 S Data/Write, G|D */
    gdt[4] = 0x0020FA0000000000ULL; /* User code:   P DPL3 S Exec/Read, L=1  */

    /* Program TSS stacks: array top (first free address above the region). */
    rsp0_value = (u64)(&tss_rsp0_stack[4096]);
    ist1_value = (u64)(&tss_ist1_stack[2048]);

    /* RSP0 at TSS offset 4 (words [1],[2]); IST1 at offset 36 ([9],[10]). */
    tss[1] = (u32)(rsp0_value & 0xFFFFFFFF);
    tss[2] = (u32)(rsp0_value >> 32);
    tss[9]  = (u32)(ist1_value & 0xFFFFFFFF);
    tss[10] = (u32)(ist1_value >> 32);
    /* I/O Map Base Address points past the TSS: no permission bitmap. */
    tss[25] = 104;

    /* 64-bit TSS system descriptor: P DPL0 type=0x9 (available 64-bit TSS),
     * 16-byte descriptor split across two GDT slots. */
    u64 base = (u64)(&tss[0]);
    u64 limit = 103;
    gdt[5] = (limit & 0xFFFF)
           | ((base & 0x00FFFFFF) << 16)
           | ((u64)0x89 << 40)
           | (((limit >> 16) & 0x0F) << 48)
           | ((base & 0xFF000000) << 32);
    gdt[6] = base >> 32;

    gdt_flush();
    tss_load(0x28);
}

u64 gdt_get_tss_base(void) {
    return (u64)(&tss[0]);
}

u64 gdt_get_rsp0(void) {
    return rsp0_value;
}

/* Repoint the ring-3 -> ring-0 transition stack (used when the CPU takes an
 * interrupt/trap from user mode). The scheduler updates this on every context
 * switch so each thread traps onto its own kernel stack. */
void gdt_set_rsp0(u64 value) {
    rsp0_value = value;
    tss[1] = (u32)(value & 0xFFFFFFFF);
    tss[2] = (u32)(value >> 32);
}

u64 gdt_get_ist1(void) {
    return ist1_value;
}
