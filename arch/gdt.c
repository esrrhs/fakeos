package arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void gdt_flush(void);
extern void tss_load(u16 selector);
extern u32 smp_cpuid(void);

/* Global Descriptor Table (referenced by gdtr in desc.asm):
 *   0x00 null
 *   0x08 kernel 64-bit code (DPL0, L=1)
 *   0x10 kernel data        (DPL0)
 *   0x18 user data          (DPL3)
 *   0x20 user 64-bit code   (DPL3, L=1)
 *   0x28 + 16*cpu  64-bit TSS descriptor (two slots each)
 *
 * One shared GDT, per-CPU TSS: LTR is busy-bit hostile across CPUs, and
 * AP timer IRQs need a valid TR/IST1 or a nested fault triple-faults. */
enum {
    GDT_MAX_CPUS   = 8,
    GDT_SEG_SLOTS  = 5,                 /* null + 4 code/data */
    GDT_TSS0_SLOT  = 5,                 /* first TSS low qword index */
    GDT_ENTRIES    = GDT_SEG_SLOTS + GDT_MAX_CPUS * 2
};

u64 gdt[GDT_ENTRIES];

/* Per-CPU 64-bit TSS (104 bytes) as raw 32-bit words for exact RSP0/IST
 * placement. Indexed by logical CPU (MADT order); BSP is CPU 0 on QEMU. */
static u32 cpu_tss[GDT_MAX_CPUS][26];

/* Ring0 privilege-transition stacks (CPL3 -> CPL0). */
static u64 cpu_rsp0_stack[GDT_MAX_CPUS][4096];

/* IST1: dedicated #DF guard stacks (16 KiB each). */
static u64 cpu_ist1_stack[GDT_MAX_CPUS][2048];

static u64 cpu_rsp0_value[GDT_MAX_CPUS];
static u64 cpu_ist1_value[GDT_MAX_CPUS];

static void gdt_install_tss(u32 cpu) {
    u64 base;
    u64 limit;
    u32 slot;
    u64 rsp0;
    u64 ist1;
    int i;

    if (cpu >= GDT_MAX_CPUS) {
        return;
    }
    for (i = 0; i < 26; i++) {
        cpu_tss[cpu][i] = 0;
    }

    rsp0 = (u64)(&cpu_rsp0_stack[cpu][4096]);
    ist1 = (u64)(&cpu_ist1_stack[cpu][2048]);
    cpu_rsp0_value[cpu] = rsp0;
    cpu_ist1_value[cpu] = ist1;

    /* RSP0 at TSS offset 4 (words [1],[2]); IST1 at offset 36 ([9],[10]). */
    cpu_tss[cpu][1] = (u32)(rsp0 & 0xFFFFFFFF);
    cpu_tss[cpu][2] = (u32)(rsp0 >> 32);
    cpu_tss[cpu][9]  = (u32)(ist1 & 0xFFFFFFFF);
    cpu_tss[cpu][10] = (u32)(ist1 >> 32);
    /* I/O Map Base Address points past the TSS: no permission bitmap. */
    cpu_tss[cpu][25] = 104;

    /* 64-bit TSS system descriptor: P DPL0 type=0x9 (available 64-bit TSS). */
    base = (u64)(&cpu_tss[cpu][0]);
    limit = 103;
    slot = GDT_TSS0_SLOT + cpu * 2;
    gdt[slot] = (limit & 0xFFFF)
              | ((base & 0x00FFFFFF) << 16)
              | ((u64)0x89 << 40)
              | (((limit >> 16) & 0x0F) << 48)
              | ((base & 0xFF000000) << 32);
    gdt[slot + 1] = base >> 32;
}

u16 gdt_tss_selector(u32 cpu) {
    if (cpu >= GDT_MAX_CPUS) {
        cpu = 0;
    }
    return (u16)((GDT_TSS0_SLOT + cpu * 2) << 3);
}

void gdt_init(void) {
    int i;
    u32 cpu;

    for (i = 0; i < GDT_ENTRIES; i++) {
        gdt[i] = 0;
    }

    gdt[1] = 0x00209A0000000000ULL; /* Kernel code: P DPL0 S Exec/Read, L=1 */
    gdt[2] = 0x0000920000000000ULL; /* Kernel data: P DPL0 S Data/Write      */
    gdt[3] = 0x00CFF20000000000ULL; /* User data:   P DPL3 S Data/Write, G|D */
    gdt[4] = 0x0020FA0000000000ULL; /* User code:   P DPL3 S Exec/Read, L=1  */

    for (cpu = 0; cpu < GDT_MAX_CPUS; cpu++) {
        gdt_install_tss(cpu);
    }

    gdt_flush();
    /* BSP early path: logical CPU 0 TSS. APs call gdt_load_cpu_tss later. */
    tss_load(gdt_tss_selector(0));
}

/* LTR this CPU's TSS selector. Each CPU must load a distinct descriptor;
 * sharing one TSS busy-bits #GP on the second LTR. */
void gdt_load_cpu_tss(u32 cpu) {
    if (cpu >= GDT_MAX_CPUS) {
        cpu = 0;
    }
    tss_load(gdt_tss_selector(cpu));
}

u64 gdt_get_tss_base(void) {
    return (u64)(&cpu_tss[0][0]);
}

u64 gdt_get_rsp0(void) {
    return cpu_rsp0_value[0];
}

/* Repoint the ring-3 -> ring-0 transition stack for *this* CPU's TSS (used
 * when the CPU takes an interrupt/trap from user mode). The scheduler updates
 * this on every context switch so each thread traps onto its own kernel stack. */
void gdt_set_rsp0(u64 value) {
    u32 cpu = smp_cpuid();
    if (cpu >= GDT_MAX_CPUS) {
        cpu = 0;
    }
    cpu_rsp0_value[cpu] = value;
    cpu_tss[cpu][1] = (u32)(value & 0xFFFFFFFF);
    cpu_tss[cpu][2] = (u32)(value >> 32);
}

u64 gdt_get_ist1(void) {
    return cpu_ist1_value[0];
}

u32 gdt_entry_count(void) {
    return (u32)GDT_ENTRIES;
}
