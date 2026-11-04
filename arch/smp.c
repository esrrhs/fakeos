package arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);
extern u32 lapic_get_id(void);
extern void lapic_send_init_ipi(u32 apic_id);
extern void lapic_send_startup_ipi(u32 apic_id, u8 vector);
extern void pit_busy_wait_ms(u32 ms);
extern void tlb_flush(void);
extern void cpu_cli(void);
extern void cpu_halt(void);
extern void cpu_pause(void);
extern u32 sched_ap_waiting(void);
extern void sched_ap_enter(u32 cpu);
extern u8 boot_pml4[];
extern u8 boot_pdpt[];
extern u8 ap_tramp_blob[];
extern u8 ap_tramp_blob_end[];

/* ---------------------------------------------------------------------------
 * SMP: MADT discovery + AP trampoline bring-up (AP bring-up + sched release hook).
 *
 * smp_init runs before PMM, so AP stacks are static BSS. BSP restores
 * PML4[0] identity (boot_pdpt) so the SIPI trampoline at phys 0x8000 can
 * enter long mode, then APs jump to high-half ap_main and spin until sched release.
 * Default QEMU (-smp 1) stays single-CPU; -smp 2+ brings APs online.
 * ------------------------------------------------------------------------- */

enum {
    MAX_CPUS       = 8,
    TRAMP_PHYS     = 0x8000,
    TRAMP_SIPI_VEC = 0x08,
    OFF_CR3        = 0xC8,
    OFF_STACK      = 0xD0,
    OFF_ENTRY      = 0xD8,
    OFF_CPU        = 0xE0,
    OFF_DONE       = 0xE4,
    AP_STACK_WORDS = 2048
};

struct cpu_info {
    u32 apic_id;
    u32 online;
    u64 stack_top;
};

static struct cpu_info cpus[MAX_CPUS];
static u32 ncpus;
static u32 bsp_apic;
static u32 tramp_ready;
static u32 identity_on;
static u64 ap_stack_storage[MAX_CPUS][AP_STACK_WORDS];

/* APIC id -> logical CPU index; SYSCALL asm indexes per-CPU kstack by this. */
u8 smp_apic_logical[256];

static const u64 HHDM_BASE = 0xFFFF800000000000ULL;

static volatile u8 *tramp_u8(void) {
    return (volatile u8 *)(HHDM_BASE + (u64)TRAMP_PHYS);
}

static volatile u32 *tramp_u32(u32 off) {
    return (volatile u32 *)(HHDM_BASE + (u64)TRAMP_PHYS + (u64)off);
}

static volatile u64 *tramp_u64(u32 off) {
    return (volatile u64 *)(HHDM_BASE + (u64)TRAMP_PHYS + (u64)off);
}

static volatile u64 *pml4_slot0(void) {
    return (volatile u64 *)(HHDM_BASE + (u64)(&boot_pml4[0]));
}

static void smp_restore_identity(void) {
    u64 pdpt = (u64)(&boot_pdpt[0]);
    if (identity_on) {
        return;
    }
    *pml4_slot0() = pdpt | 0x3ULL;
    tlb_flush();
    identity_on = 1;
}

static u8 checksum_ok(const volatile u8 *p, u32 len) {
    u32 i;
    u8 s = 0;
    for (i = 0; i < len; i++) {
        s = (u8)(s + p[i]);
    }
    return s == 0;
}

static void madt_consume(u64 madt_pa) {
    volatile u8 *tp = (volatile u8 *)(HHDM_BASE + madt_pa);
    u32 mlen = *(volatile u32 *)(HHDM_BASE + madt_pa + 4);
    u32 off = 44;
    u32 found = 0;
    while (off + 2 <= mlen && found < MAX_CPUS) {
        u8 typ = tp[off];
        u8 tlen = tp[off + 1];
        if (tlen < 2) {
            break;
        }
        if (typ == 0 && tlen >= 8) {
            u8 apic = tp[off + 3];
            u32 fl = *(volatile u32 *)(HHDM_BASE + madt_pa + off + 4);
            if (fl & 1u) {
                cpus[found].apic_id = (u32)apic;
                cpus[found].online = 0;
                cpus[found].stack_top = 0;
                found++;
            }
        }
        off += tlen;
    }
    if (found > 0) {
        ncpus = found;
        kprintf("[SMP] MADT: %u usable Local APICs\n", (u64)ncpus);
    }
}

static void smp_parse_madt(void) {
    u64 rsdp = 0;
    u32 i;
    u16 ebda_seg;
    u64 ebda;

    ebda_seg = *(volatile u16 *)(HHDM_BASE + 0x40EULL);
    ebda = (u64)ebda_seg << 4;
    if (ebda != 0 && ebda < 0x100000ULL) {
        for (i = 0; i < 0x400; i += 16) {
            volatile u8 *p = (volatile u8 *)(HHDM_BASE + ebda + (u64)i);
            if (p[0]=='R' && p[1]=='S' && p[2]=='D' && p[3]==' ' &&
                p[4]=='P' && p[5]=='T' && p[6]=='R' && p[7]==' ') {
                if (checksum_ok(p, 20)) {
                    rsdp = ebda + i;
                    break;
                }
            }
        }
    }
    if (rsdp == 0) {
        for (i = 0xE0000; i < 0x100000; i += 16) {
            volatile u8 *p = (volatile u8 *)(HHDM_BASE + (u64)i);
            if (p[0]=='R' && p[1]=='S' && p[2]=='D' && p[3]==' ' &&
                p[4]=='P' && p[5]=='T' && p[6]=='R' && p[7]==' ') {
                if (checksum_ok(p, 20)) {
                    rsdp = (u64)i;
                    break;
                }
            }
        }
    }
    if (rsdp == 0) {
        kprintf("[SMP] RSDP not found; single-CPU\n");
        return;
    }

    {
        volatile u8 *rp = (volatile u8 *)(HHDM_BASE + rsdp);
        u8 rev = rp[15];
        u64 xsdt = 0;
        u64 rsdt = 0;
        u32 root_len;
        u32 ent;

        rsdt = (u64)(*(volatile u32 *)(HHDM_BASE + rsdp + 16));
        if (rev >= 2) {
            xsdt = *(volatile u64 *)(HHDM_BASE + rsdp + 24);
        }

        if (xsdt != 0) {
            root_len = *(volatile u32 *)(HHDM_BASE + xsdt + 4);
            if (root_len >= 36) {
                u32 n = (root_len - 36) / 8;
                for (ent = 0; ent < n; ent++) {
                    u64 tbl = *(volatile u64 *)(HHDM_BASE + xsdt + 36 + (u64)ent * 8);
                    volatile u8 *tp = (volatile u8 *)(HHDM_BASE + tbl);
                    if (tp[0]=='A' && tp[1]=='P' && tp[2]=='I' && tp[3]=='C') {
                        madt_consume(tbl);
                        return;
                    }
                }
            }
        }
        if (rsdt != 0) {
            root_len = *(volatile u32 *)(HHDM_BASE + rsdt + 4);
            if (root_len >= 36) {
                u32 n = (root_len - 36) / 4;
                for (ent = 0; ent < n; ent++) {
                    u32 tbl = *(volatile u32 *)(HHDM_BASE + rsdt + 36 + (u64)ent * 4);
                    volatile u8 *tp = (volatile u8 *)(HHDM_BASE + (u64)tbl);
                    if (tp[0]=='A' && tp[1]=='P' && tp[2]=='I' && tp[3]=='C') {
                        madt_consume((u64)tbl);
                        return;
                    }
                }
            }
        }
        kprintf("[SMP] MADT not found in ACPI; single-CPU\n");
    }
}

static void smp_install_trampoline(void) {
    u64 blob = (u64)(&ap_tramp_blob[0]);
    u64 end = (u64)(&ap_tramp_blob_end[0]);
    u64 len = end - blob;
    u64 i;
    volatile u8 *dst;
    const u8 *src;

    if (len == 0 || len > 0x1000) {
        kprintf("[SMP] trampoline blob bad len %u\n", len);
        return;
    }
    smp_restore_identity();
    dst = tramp_u8();
    src = (const u8 *)blob;
    for (i = 0; i < len; i++) {
        dst[i] = src[i];
    }
    for (; i < 0x1000; i++) {
        dst[i] = 0;
    }
    tramp_ready = 1;
    kprintf("[SMP] AP trampoline at phys 0x%x (%u bytes)\n",
            (u64)TRAMP_PHYS, len);
}

void ap_main(u32 cpu) {
    if (cpu < MAX_CPUS) {
        cpus[cpu].online = 1;
    }
    *tramp_u32(OFF_DONE) = 1;
    /* Park until the BSP finishes sched_init/selftests and releases APs
     * into per-CPU idle via sched_smp_start(). */
    while (sched_ap_waiting()) {
        cpu_pause();
    }
    sched_ap_enter(cpu);
    cpu_cli();
    for (;;) {
        cpu_halt();
    }
}

u32 smp_cpu_count(void) {
    return ncpus != 0 ? ncpus : 1;
}

u32 smp_cpuid(void) {
    u32 apic = lapic_get_id();
    u32 i;
    for (i = 0; i < ncpus; i++) {
        if (cpus[i].apic_id == apic) {
            return i;
        }
    }
    return 0;
}

u32 smp_bsp_apic_id(void) {
    return bsp_apic;
}

u32 smp_start_aps(void) {
    u32 started = 0;
    u32 i;

    if (!tramp_ready) {
        kprintf("[SMP] AP start skipped (no trampoline)\n");
        return 0;
    }
    if (ncpus <= 1) {
        kprintf("[SMP] no APs to start (ncpus=%u)\n", (u64)ncpus);
        return 0;
    }

    smp_restore_identity();

    for (i = 0; i < ncpus; i++) {
        u64 stack_top;
        u32 wait;
        if (cpus[i].apic_id == bsp_apic) {
            cpus[i].online = 1;
            continue;
        }
        if (cpus[i].online) {
            continue;
        }

        stack_top = (u64)(&ap_stack_storage[i][AP_STACK_WORDS]);
        stack_top &= ~0xFULL;
        cpus[i].stack_top = stack_top;

        *tramp_u64(OFF_CR3) = (u64)(&boot_pml4[0]);
        *tramp_u64(OFF_STACK) = stack_top;
        *tramp_u64(OFF_ENTRY) = (u64)(&ap_main);
        *tramp_u32(OFF_CPU) = i;
        *tramp_u32(OFF_DONE) = 0;

        lapic_send_init_ipi(cpus[i].apic_id);
        pit_busy_wait_ms(10);
        lapic_send_startup_ipi(cpus[i].apic_id, (u8)TRAMP_SIPI_VEC);
        pit_busy_wait_ms(1);
        lapic_send_startup_ipi(cpus[i].apic_id, (u8)TRAMP_SIPI_VEC);

        for (wait = 0; wait < 500; wait++) {
            if (*tramp_u32(OFF_DONE) != 0) {
                break;
            }
            pit_busy_wait_ms(1);
        }
        if (cpus[i].online || *tramp_u32(OFF_DONE) != 0) {
            cpus[i].online = 1;
            started++;
            kprintf("[SMP] AP%u online (APIC %u)\n",
                    (u64)i, (u64)cpus[i].apic_id);
        } else {
            kprintf("[SMP] AP%u (APIC %u) bring-up timeout\n",
                    (u64)i, (u64)cpus[i].apic_id);
        }
    }
    kprintf("[SMP] started %u AP(s); parked until sched release\n",
            (u64)started);
    return started;
}


u32 smp_cpu_online(u32 cpu) {
    if (cpu >= MAX_CPUS) {
        return 0;
    }
    return cpus[cpu].online;
}

u64 smp_cpu_stack_top(u32 cpu) {
    if (cpu >= MAX_CPUS) {
        return 0;
    }
    return cpus[cpu].stack_top;
}

void smp_init(void) {
    u32 i;
    bsp_apic = lapic_get_id();
    ncpus = 1;
    tramp_ready = 0;
    identity_on = 0;
    for (i = 0; i < 256; i++) {
        smp_apic_logical[i] = 0;
    }
    for (i = 0; i < MAX_CPUS; i++) {
        cpus[i].apic_id = 0;
        cpus[i].online = 0;
        cpus[i].stack_top = 0;
    }
    cpus[0].apic_id = bsp_apic;
    cpus[0].online = 1;
    smp_apic_logical[bsp_apic & 0xFF] = 0;

    smp_parse_madt();
    if (ncpus == 0) {
        ncpus = 1;
        cpus[0].apic_id = bsp_apic;
        cpus[0].online = 1;
    } else {
        for (i = 0; i < ncpus; i++) {
            if (cpus[i].apic_id == bsp_apic) {
                cpus[i].online = 1;
            }
        }
    }
    for (i = 0; i < ncpus && i < MAX_CPUS; i++) {
        smp_apic_logical[cpus[i].apic_id & 0xFF] = (u8)i;
    }

    smp_install_trampoline();
    smp_start_aps();
    kprintf("[SMP] BSP APIC ID %u; %u CPU(s) tracked\n",
            (u64)bsp_apic, (u64)ncpus);
}
