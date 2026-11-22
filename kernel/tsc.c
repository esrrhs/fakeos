package kernel;
import arch;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);

/* TSC-derived monotonic clock (slice: high-resolution time).
 *
 * rdtsc is machine-coded as a naked single-statement function: fakecc's .S
 * assembler SILENTLY DROPS unknown opcodes (rdtsc and lock cmpxchg both
 * vanish — see docs/evidence/vm-shootdown-20261121/RESULT.md), and the
 * host-side wire tests prove the naked + one __asm__ .byte statement path
 * emits exactly the listed bytes. Multiple statements or normal functions
 * gain a fakecc prologue whose stack spills corrupt the caller's frame.
 * Encoding: 0F 31 = RDTSC; 48 C1 E2 20 = SHL $32, %rdx; 48 09 D0 =
 * OR %rdx, %rax; C3 = RET (EDX:EAX -> RAX). */
__attribute__((naked)) u64 tsc_read(void) {
    /* push %rdx; rdtsc; shl $32,%rdx; or %rdx,%rax; pop %rdx; ret —
     * single .byte directive (wire-test format: fakecc keeps only the
     * FIRST .byte directive of a statement). RDX is preserved: fakecc's
     * internal call ABI does not document caller-saved registers, so the
     * rdtsc side effects must not leak into the caller's registers. */
    __asm__ volatile(".byte 82,15,49,72,193,226,32,72,9,208,90,195");
}

/* ns-per-cycle as a fixed-point multiplier: ns = (cycles * mult) >> 20.
 * mult = (1e9 << 20) / hz fits u64 comfortably and (cycles * mult) cannot
 * overflow for any realistic uptime (cycles < 2^64 / mult). */
static u64 tsc_mult;
static u32 tsc_shift = 20;
static u64 tsc_base;          /* cycles at calibration            */
static u64 ns_base;           /* monotonic ns at calibration      */
static u64 tsc_hz_value;
static u64 vdso_page;         /* shared clock page (vDSO-lite)    */

u64 tsc_hz(void) {
    return tsc_hz_value;
}

/* Calibrate against the PIT (real-time hardware pacing, independent of the
 * LAPIC tick rate and of which CPU runs this). A implausible rate falls
 * back to tick-derived time (tsc_hz == 0), keeping every caller correct. */
void tsc_calibrate(void) {
    u64 c0;
    u64 c1;
    u64 delta;
    c0 = tsc_read();
    arch.pit_busy_wait_ms(100);
    c1 = tsc_read();
    delta = c1 - c0;
    if (delta < (u64)50000000) {              /* < 500 MHz: implausible */
        tsc_hz_value = 0;
        kprintf("[TSC] rate %u Hz implausible; tick clock stays\n",
                (u64)(delta * 10));
        return;
    }
    if (delta > (u64)20000000000ULL) {        /* > 20 GHz: implausible */
        tsc_hz_value = 0;
        kprintf("[TSC] rate %u Hz implausible; tick clock stays\n",
                (u64)(delta * 10));
        return;
    }
    tsc_hz_value = delta * 10;
    tsc_base = c0;
    ns_base = 0;
    tsc_mult = ((u64)1000000000 << 20) / tsc_hz_value;
    kprintf("[TSC] calibrated: %u Hz (PIT 100 ms window), mult=%u shift=%u\n",
            (u64)tsc_hz_value, (u64)tsc_mult, (u64)tsc_shift);
}

/* Monotonic nanoseconds since calibration. Safe on any CPU once calibrated
 * (per-CPU TSC monotonic; cross-CPU TCG skew is clamped by the caller's
 * non-decreasing reads where it matters). */
u64 tsc_ns(void) {
    u64 cycles;
    if (tsc_hz_value == 0) {
        return (u64)0;
    }
    cycles = tsc_read() - tsc_base;
    return ns_base + ((cycles * tsc_mult) >> tsc_shift);
}

/* ---------------------------------------------------------------------------
 * vDSO-lite: one shared clock page mapped read-only-ish into every user
 * address space at a fixed VA. Fields: magic, version, mult, shift,
 * base_ns. User code reads its TSC and the page without a syscall.
 * ------------------------------------------------------------------------- */
enum {
    VDSO_MAGIC   = 0x646F7376u,            /* "vdso"                        */
    VDSO_VERSION = 1,
    /* u32 indexes: ns = base + ((tsc - tsc_base) * mult) >> shift       */
    VDSO_OFFSET_MAGIC  = 0,
    VDSO_OFFSET_VER    = 1,
    VDSO_OFFSET_MULT   = 2,                /* u64 pair: indexes 2,3         */
    VDSO_OFFSET_SHIFT  = 4,
    VDSO_OFFSET_FLAGS  = 5,                /* 1 = usable                    */
    VDSO_OFFSET_BASE   = 6,                /* u64 pair: indexes 6,7         */
    VDSO_OFFSET_TSCBASE = 8                /* u64 pair: indexes 8,9         */
};

static volatile u32 *vdso_ctrl;

void tsc_vdso_init(void) {
    vdso_page = mem.pmm_alloc_page();
    if (vdso_page == 0) {
        return;
    }
    vdso_ctrl = (volatile u32 *)(0xFFFF800000000000ULL + vdso_page);
    vdso_ctrl[VDSO_OFFSET_MAGIC] = VDSO_MAGIC;
    vdso_ctrl[VDSO_OFFSET_VER] = VDSO_VERSION;
    vdso_ctrl[VDSO_OFFSET_FLAGS] = 0;      /* invalid until calibrated */
    tsc_vdso_publish();
}

/* Re-publish the clock parameters (calibration values are boot-static, so
 * this runs once from tsc_calibrate and once at page setup). */
void tsc_vdso_publish(void) {
    if (vdso_ctrl == 0) {
        return;
    }
    vdso_ctrl[VDSO_OFFSET_MULT] = (u32)(tsc_mult & 0xFFFFFFFFu);
    vdso_ctrl[VDSO_OFFSET_MULT + 1] = (u32)(tsc_mult >> 32);
    vdso_ctrl[VDSO_OFFSET_SHIFT] = tsc_shift;
    vdso_ctrl[VDSO_OFFSET_BASE] = (u32)(ns_base & 0xFFFFFFFFu);
    vdso_ctrl[VDSO_OFFSET_BASE + 1] = (u32)(ns_base >> 32);
    vdso_ctrl[VDSO_OFFSET_TSCBASE] = (u32)(tsc_base & 0xFFFFFFFFu);
    vdso_ctrl[VDSO_OFFSET_TSCBASE + 1] = (u32)(tsc_base >> 32);
    if (tsc_hz_value != 0) {
        vdso_ctrl[VDSO_OFFSET_FLAGS] = 1;
    }
}

u64 tsc_vdso_phys(void) {
    return vdso_page;
}

u32 tsc_vdso_ready(void) {
    return (vdso_ctrl != 0 && vdso_page != 0) ? 1 : 0;
}
