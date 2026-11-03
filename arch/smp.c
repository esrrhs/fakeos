package arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);
extern u32 lapic_get_id(void);
extern void lapic_send_init_ipi(u32 apic_id);
extern void lapic_send_startup_ipi(u32 apic_id, u8 vector);
extern void pit_busy_wait_ms(u32 ms);

/* ---------------------------------------------------------------------------
 * SMP bootstrap stubs (BSP-only today).
 *
 * Foundations for future multi-CPU bring-up:
 *  - cpu_info[] table and smp_cpu_count / smp_cpuid
 *  - INIT + SIPI helpers via lapic ICR (not issued unless callers ask)
 *  - smp_start_aps() documents the sequence but returns 0 APs started
 *    until a real low-memory trampoline + per-CPU stacks exist.
 *
 * Still single-CPU: no AP entry, no per-CPU runqueues, no TLB shootdown.
 * ------------------------------------------------------------------------- */

enum {
    MAX_CPUS = 8
};

struct cpu_info {
    u32 apic_id;
    u32 online;
};

static struct cpu_info cpus[MAX_CPUS];
static u32 ncpus;
static u32 bsp_apic;

u32 smp_cpu_count(void) {
    return ncpus != 0 ? ncpus : 1;
}

/* Logical CPU index for the caller. BSP-only world => always 0. */
u32 smp_cpuid(void) {
    return 0;
}

u32 smp_bsp_apic_id(void) {
    return bsp_apic;
}

/* Attempt AP bring-up. Without a trampoline this is a no-op that logs the
 * intended INIT-SIPI-SIPI sequence and returns how many APs went online (0). */
u32 smp_start_aps(void) {
    /* Future: place 16-bit trampoline at 0x8000, INIT + two SIPIs per AP,
     * wait on cpus[i].online. Today we refuse to fire IPIs into the void.
     * ICR helpers are linked so bring-up can call them without new symbols. */
    if (ncpus > 1) {
        u32 ap = cpus[1].apic_id;
        lapic_send_init_ipi(ap);
        pit_busy_wait_ms(10);
        lapic_send_startup_ipi(ap, 0x08);
        pit_busy_wait_ms(1);
        lapic_send_startup_ipi(ap, 0x08);
    }
    kprintf("[SMP] AP bootstrap deferred (no trampoline/per-CPU stacks yet)\n");
    return 0;
}

void smp_init(void) {
    u32 i;
    bsp_apic = lapic_get_id();
    ncpus = 1;
    for (i = 0; i < MAX_CPUS; i++) {
        cpus[i].apic_id = 0;
        cpus[i].online = 0;
    }
    cpus[0].apic_id = bsp_apic;
    cpus[0].online = 1;
    kprintf("[SMP] BSP APIC ID %u; %u CPU online (stubs: INIT/SIPI ready, no AP start)\n",
            (u64)bsp_apic, (u64)ncpus);
}
