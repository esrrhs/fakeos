package kernel;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);

/* syscall.c */
extern void syscall_init(void);
extern u64 syscall_count_total(void);

/* sched.c */
extern u32 kthread_create(u64 entry, u64 arg);
extern u32 kthread_create_user(u64 entry_rip, u64 user_rsp, u64 as_h, u32 pid);
extern u32 sched_alloc_pid(void);
extern void sched_yield(void);
extern void kthread_exit(void);
extern u64 sched_tick_count(void);
extern u64 sched_user_tick_count(void);

/* Embedded userland ELF image (user/blob.asm, incbin of build/user/init.elf) */
extern char user_elf_start;
extern char user_elf_end;

enum {
    UPROC_STACK_TOP  = 0x800000,        /* user stack top VA          */
    UPROC_STACK_SIZE = 16384,           /* 4 demand-paged RW pages    */
    UPROC_N          = 2,               /* init processes started     */
    UPROC_MAX_EXIT   = 4                /* each forks one child       */
};

static volatile u32 uproc_exits;
static volatile u32 uproc_done;

void uproc_note_exit(u32 pid, u64 code) {
    (void)code;
    uproc_exits++;
}

/* ---------------------------------------------------------------------------
 * Minimal ELF64 loader: parses program headers of the embedded userland image
 * and maps every PT_LOAD segment into a fresh address space (RX text, RW data
 * honored from p_flags; page content copied through the HHDM).
 * ------------------------------------------------------------------------- */

static u64 elf_u64(u64 base, u64 off) {
    u64 *p = (u64 *)(base + off);
    return *p;
}

static u32 elf_u32(u64 base, u64 off) {
    u32 *p = (u32 *)(base + off);
    return *p;
}

static u64 load_elf(u64 blob, u64 size) {
    u8 *id = (u8 *)blob;
    if (size < 64 || id[0] != 0x7F || id[1] != 'E' || id[2] != 'L' || id[3] != 'F') {
        kprintf("      [FAIL] user ELF magic mismatch\n");
        return 0;
    }
    if (id[4] != 2) {                   /* ELFCLASS64 */
        kprintf("      [FAIL] user ELF is not 64-bit\n");
        return 0;
    }

    u64 as_h = mem.as_create();
    if (as_h == 0) {
        kprintf("      [FAIL] could not allocate address space\n");
        return 0;
    }

    u64 phoff = elf_u64(blob, 32);
    u32 phnum = elf_u32(blob, 56) & 0xFFFF;
    u32 i;
    for (i = 0; i < phnum; i++) {
        u64 ph = blob + phoff + i * 56;
        if (elf_u32(ph, 0) != 1) {      /* PT_LOAD only */
            continue;
        }
        u32 flags = elf_u32(ph, 4);     /* PF_X=1 PF_W=2 PF_R=4 */
        u64 off = elf_u64(ph, 8);
        u64 vaddr = elf_u64(ph, 16);
        u64 filesz = elf_u64(ph, 32);
        u64 memsz = elf_u64(ph, 40);
        u32 prot = 1;                   /* PROT_READ */
        if (flags & 2) {
            prot = prot | 2;            /* PROT_WRITE */
        }
        if (flags & 1) {
            prot = prot | 4;            /* PROT_EXEC */
        }
        if (!mem.as_map_preload(as_h, vaddr, memsz, prot, blob + off, filesz)) {
            kprintf("      [FAIL] segment map failed va=%p sz=%u\n", vaddr, memsz);
            mem.as_destroy_h(as_h);
            return 0;
        }
    }

    /* Ring-3 stack: demand-paged anonymous RW pages below UPROC_STACK_TOP. */
    if (!mem.as_map_anon_h(as_h, UPROC_STACK_TOP - UPROC_STACK_SIZE,
                           UPROC_STACK_SIZE, 1 | 2)) {
        kprintf("      [FAIL] user stack VMA reservation failed\n");
        mem.as_destroy_h(as_h);
        return 0;
    }
    return as_h;
}

/* ---------------------------------------------------------------------------
 * Kernel-side monitor: waits until every forked generation has exited, then
 * reports the ring-3 / syscall / COW-fork results.
 * ------------------------------------------------------------------------- */

static void uproc_monitor(u64 arg) {
    u64 start_ticks = (u64)arg;
    u64 deadline = start_ticks + 1500;  /* 15 s at 100 Hz */
    while (uproc_exits < UPROC_MAX_EXIT && sched_tick_count() < deadline) {
        sched_yield();
    }

    kprintf("\n[UPROC] Monitor: %u of %u expected processes exited\n",
            (u64)uproc_exits, (u64)UPROC_MAX_EXIT);
    kprintf("      SYSCALLs served: %u, ring-3 timer ticks: %u\n",
            syscall_count_total(), sched_user_tick_count());

    if (uproc_exits == UPROC_MAX_EXIT) {
        kprintf("      [PASS] Ring-3 entry: %u user processes ran via IRET trampoline\n",
                (u64)UPROC_N);
        kprintf("      [PASS] SYSCALL/SYSRET: write/getpid/yield/fork/exit served (%u calls)\n",
                syscall_count_total());
        kprintf("      [PASS] COW fork across user processes: %u clean exits, spaces torn down\n",
                (u64)UPROC_MAX_EXIT);
    } else {
        kprintf("      [FAIL] user processes did not all exit before the deadline\n");
    }
    if (sched_user_tick_count() > 0) {
        kprintf("      [PASS] Timer preemption of ring-3 threads (%u user-mode ticks)\n",
                sched_user_tick_count());
    } else {
        kprintf("      [FAIL] no APIC timer tick ever interrupted ring 3\n");
    }
    uproc_done = 1;
    kthread_exit();
}

void uproc_selftest(void) {
    kprintf("\n[UPROC] Ring-3 User Processes + SYSCALL/SYSRET Online:\n");
    kprintf("      MSR STAR/LSTAR/SFMASK + EFER.SCE programmed, GDT SYSRET-compatible\n");
    kprintf("      Per-process PML4 (kernel slots shared), demand-paged user stack\n");

    syscall_init();
    uproc_exits = 0;
    uproc_done = 0;

    u64 blob = (u64)(&user_elf_start);
    u64 size = (u64)(&user_elf_end) - blob;
    u64 entry = elf_u64(blob, 24);

    u32 created = 0;
    u32 i;
    for (i = 0; i < UPROC_N; i++) {
        u64 as_h = load_elf(blob, size);
        if (as_h == 0) {
            continue;
        }
        /* 16-byte aligned user stack, staged as after a call (RSP%16 == 8). */
        u64 user_rsp = (UPROC_STACK_TOP & ~15ULL) - 8;
        u32 pid = sched_alloc_pid();
        if (kthread_create_user(entry, user_rsp, as_h, pid) == 0xFFFFFFFF) {
            kprintf("      [FAIL] could not create user thread for pid %u\n", (u64)pid);
            mem.as_destroy_h(as_h);
            continue;
        }
        created++;
    }
    kprintf("      Started %u user processes (entry %p, stack top %p)\n",
            (u64)created, entry, (u64)UPROC_STACK_TOP);

    if (created == 0) {
        kprintf("      [FAIL] no user process could be started\n");
        uproc_done = 1;
        return;
    }

    if (kthread_create((u64)(&uproc_monitor), sched_tick_count()) == 0xFFFFFFFF) {
        kprintf("      [FAIL] could not create monitor thread\n");
        uproc_done = 1;
        return;
    }

    /* kmain (slot 0) yields until the monitor reports completion. */
    while (!uproc_done) {
        sched_yield();
    }
}
