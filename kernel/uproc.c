package kernel;
import fs;
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
extern u64 sched_current_as_h(void);
extern void sched_replace_as(u64 new_as_h);

/* sysfile.c */
extern u32 copy_user_path(char *dst, u64 upath);
extern u32 copy_user_cstring(char *dst, u64 uva, u32 maxlen);

/* Embedded userland images live in Ramfs after rootfs_publish(); pid 1 is
 * loaded from /bin/init rather than from a private blob symbol. */

enum {
    /* 8 MiB demand-paged stack near (but below) the signed-32-bit ceiling:
     * fakecc -O0 materializes enum constants through sign-extending 32-bit
     * moves, so a top at exactly 0x80000000 would arrive as a negative-high
     * address. 0x7f800000 stays positive, clears the 1 GiB mmap ceiling and
     * leaves the compiler the deep -O0 frames it needs (Linux soft limit). */
    UPROC_STACK_TOP  = 0x7f800000,       /* user stack top VA          */
    UPROC_STACK_SIZE = 0x00800000,       /* 8 MiB demand-paged RW      */
    UPROC_TEST_CHILDREN = 2,            /* init forks two COW probes  */
    MONITOR_PHASE1_TICKS = 1500,        /* 15 s: COW children deadline  */
    MONITOR_PHASE2_TICKS = 3000,        /* 30 s: shell exit deadline    */

    ELF_MAX_PHDR     = 16,              /* sanity cap on program headers */
    ELF_MAX_SEGSZ    = 0x01000000,      /* 16 MiB per segment           */

    EXEC_MAX_ARGC    = 8,
    EXEC_ARG_SLOT    = 128,             /* fixed per-arg kernel slot  */
    EXEC_PATH_MAX    = 128,
    EXEC_INO_FILE    = 1                /* fs INO_FILE (enum not exported) */
};

static volatile u32 uproc_test_exits;   /* children that exited before sh   */
static volatile u32 uproc_pid1_done;    /* pid 1 (init -> sh) has exited    */
static volatile u32 uproc_done;

void uproc_note_exit(u32 pid, u64 code) {
    (void)code;
    if (pid == 1) {
        uproc_pid1_done = 1;
    } else {
        uproc_test_exits++;
    }
}

/* ---------------------------------------------------------------------------
 * Minimal ELF64 loader: maps every PT_LOAD segment of an image already
 * resident in kernel memory into the supplied fresh address space (RX text,
 * RW data honored from p_flags; page content copied through the HHDM).
 * Returns the entry VA, or 0 on any malformed/unmappable image.
 * ------------------------------------------------------------------------- */

static u64 elf_u64(u64 base, u64 off) {
    u64 *p = (u64 *)(base + off);
    return *p;
}

static u32 elf_u32(u64 base, u64 off) {
    u32 *p = (u32 *)(base + off);
    return *p;
}

static u64 load_elf_into(u64 as_h, u64 blob, u64 size) {
    u8 *id = (u8 *)blob;
    u64 phoff;
    u32 phnum;
    u32 i;

    /* Header bounds and magic. */
    if (size < 64 || id[0] != 0x7F || id[1] != 'E' || id[2] != 'L'
        || id[3] != 'F' || id[4] != 2) {
        return 0;
    }

    phoff = elf_u64(blob, 32);
    phnum = elf_u32(blob, 56) & 0xFFFF;
    if (phnum == 0 || phnum > ELF_MAX_PHDR) {
        return 0;
    }
    /* The whole program-header table must lie inside the image. */
    if (phoff + (u64)phnum * 56 < phoff
        || phoff + (u64)phnum * 56 > size) {
        return 0;
    }
    for (i = 0; i < phnum; i++) {
        u64 ph = blob + phoff + i * 56;
        u32 flags;
        u64 off;
        u64 vaddr;
        u64 filesz;
        u64 memsz;
        u32 prot;
        if (elf_u32(ph, 0) != 1) {      /* PT_LOAD only */
            continue;
        }
        flags = elf_u32(ph, 4);         /* PF_X=1 PF_W=2 PF_R=4 */
        off = elf_u64(ph, 8);
        vaddr = elf_u64(ph, 16);
        filesz = elf_u64(ph, 32);
        memsz = elf_u64(ph, 40);
        /* Reject malformed segments before any mapping or copy: file bytes
         * must live inside the image, memsz covers filesz, and sizes stay
         * bounded. (VA/off page offsets need not match: the preload path
         * copies bytes by linear offset, not via file-offset mmap.) */
        if (filesz > memsz || memsz == 0 || memsz > ELF_MAX_SEGSZ) {
            return 0;
        }
        if (off + filesz < off || off + filesz > size) {
            return 0;
        }
        if (vaddr > 0x80000000ULL - memsz) {
            return 0;                  /* keep images in the lower half */
        }
        prot = 1;                       /* PROT_READ */
        if (flags & 2) {
            prot = prot | 2;
        }
        if (flags & 1) {
            prot = prot | 4;
        }
        if (!mem.as_map_preload(as_h, vaddr, memsz, prot, blob + off, filesz)) {
            return 0;
        }
    }
    return elf_u64(blob, 24);           /* e_entry */
}

/* ---------------------------------------------------------------------------
 * SysV initial user stack, built top-down in the (not necessarily active) new
 * address space via HHDM writes:
 *
 *   RSP -> argc
 *          argv[0] .. argv[argc-1]
 *          NULL            (argv terminator)
 *          NULL            (envp terminator; environment is always empty)
 *
 * Strings live above the pointer block. RSP is 16-byte aligned at _start.
 * argv strings arrive in a flat kernel buffer of EXEC_MAX_ARGC fixed slots.
 * ------------------------------------------------------------------------- */

static u64 build_user_stack(u64 as_h, u32 argc, const char *argbuf) {
    u64 sp = UPROC_STACK_TOP;
    u64 strptr[EXEC_MAX_ARGC];
    u64 block[EXEC_MAX_ARGC + 3];
    u64 total;
    u32 i;

    for (i = 0; i < argc; i++) {
        const char *s = argbuf + (u64)i * EXEC_ARG_SLOT;
        u32 n = 0;
        while (s[n] != 0 && n < EXEC_ARG_SLOT - 1) {
            n++;
        }
        sp -= (n + 1);
        if (!mem.as_copy_in_h(as_h, sp, s, n + 1)) {
            return 0;
        }
        strptr[i] = sp;
    }

    total = 8 * ((u64)argc + 3);
    sp &= ~15ULL;
    if ((sp - total) & 15) {
        sp -= 8;
    }
    sp -= total;

    block[0] = argc;
    for (i = 0; i < argc; i++) {
        block[1 + i] = strptr[i];
    }
    block[1 + argc] = 0;
    block[2 + argc] = 0;
    if (!mem.as_copy_in_h(as_h, sp, (const char *)block, total)) {
        return 0;
    }
    return sp;
}

/* Build a complete runnable user image in a fresh address space: ELF segments
 * + demand-paged stack VMA + argv frame. Returns 1 on success and fills the
 * entry VA, initial RSP and address-space handle. */
u32 proc_build_image(u64 kbuf, u64 size, u32 argc, const char *argbuf,
                     u64 *out_entry, u64 *out_sp, u64 *out_as_h) {
    u64 as_h = mem.as_create();
    u64 entry;
    u64 sp;
    if (as_h == 0) {
        return 0;
    }
    entry = load_elf_into(as_h, kbuf, size);
    if (entry == 0) {
        mem.as_destroy_h(as_h);
        return 0;
    }
    if (!mem.as_map_anon_h(as_h, UPROC_STACK_TOP - UPROC_STACK_SIZE,
                           UPROC_STACK_SIZE, 1 | 2)) {
        mem.as_destroy_h(as_h);
        return 0;
    }
    sp = build_user_stack(as_h, argc, argbuf);
    if (sp == 0) {
        mem.as_destroy_h(as_h);
        return 0;
    }
    *out_entry = entry;
    *out_sp = sp;
    *out_as_h = as_h;
    return 1;
}

/* ---------------------------------------------------------------------------
 * execve(): replace the current process image. pid, ppid, descriptors and
 * cwd stay (they live in the TCB); only the address space and user registers
 * change. On success this returns 0 into the NEW image (the syscall frame's
 * user RIP/RSP are rewritten). Failures return (u64)-1 with the old image
 * intact.
 * ------------------------------------------------------------------------- */

/* Binary-compatible mirror of the 128-byte frame in arch/x86_64/sysentry.asm
 * (offset 104 = user RIP/RCX slot, 120 = user RSP pad slot). */
struct exec_frame {
    u64 rax;
    u64 r9;
    u64 r8;
    u64 r10;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 rbp;
    u64 rbx;
    u64 rip;
    u64 rflags;
    u64 usersp;
};

/* Read one user u64 with exact 8-byte range validation (covers wrap-around
 * and a crossing of the VMA end from a deliberately misaligned slot). */
static u32 read_user_u64(u64 as, u64 uva, u64 *out) {
    if (!mem.as_user_range_ok(as, uva, 8)) {
        return 0;
    }
    *out = *(volatile u64 *)uva;
    return 1;
}

u64 sys_execve(u64 path_u, u64 argv_u, u64 envp_u, u64 frame_p) {
    char path[EXEC_PATH_MAX];
    char argbuf[EXEC_MAX_ARGC * EXEC_ARG_SLOT];
    u32 argc = 0;
    u64 as;
    u32 ino;
    u64 kva;
    u64 size;
    u64 entry;
    u64 sp;
    u64 new_as;
    u64 old_as;
    struct exec_frame *f;
    u32 i;

    as = sched_current_as_h();
    if (as == 0 || frame_p == 0 || !copy_user_path(path, path_u)) {
        return (u64)(-1);
    }

    /* Parse argv: up to EXEC_MAX_ARGC pointers, each string <= 127 chars.
     * Vector slots are read one at a time with per-page validation. */
    if (argv_u == 0) {
        return (u64)(-1);
    }
    {
        for (i = 0; i <= EXEC_MAX_ARGC; i++) {
            u64 slot_uva = argv_u + (u64)i * 8;
            u64 p = 0;
            char *dst;
            if (slot_uva < argv_u) {
                return (u64)(-1);           /* wrap */
            }
            if (!read_user_u64(as, slot_uva, &p)) {
                return (u64)(-1);
            }
            if (p == 0) {
                break;                      /* argv terminator */
            }
            if (i == EXEC_MAX_ARGC) {
                return (u64)(-1);           /* too many arguments */
            }
            dst = argbuf + (u64)argc * EXEC_ARG_SLOT;
            if (!copy_user_cstring(dst, p, EXEC_ARG_SLOT - 1)) {
                return (u64)(-1);
            }
            argc++;
        }
    }

    /* Environment is not supported: envp must be NULL or an empty vector. */
    if (envp_u != 0) {
        u64 e0 = 0;
        if (!read_user_u64(as, envp_u, &e0) || e0 != 0) {
            return (u64)(-1);
        }
    }

    ino = fs.fs_lookup(path);
    if (fs.fs_ino_kind(ino) != EXEC_INO_FILE) {
        return (u64)(-1);
    }
    kva = fs.fs_ino_data(ino);
    size = fs.fs_ino_size(ino);
    if (kva == 0 || size == 0) {
        return (u64)(-1);
    }

    if (!proc_build_image(kva, size, argc, argbuf, &entry, &sp, &new_as)) {
        return (u64)(-1);
    }

    /* Commit: switch TCB + CR3 to the new space, then drop the old one. Kernel
     * stacks and this frame live in the shared higher half and survive. */
    old_as = as;
    sched_replace_as(new_as);
    mem.as_activate_h(new_as);
    if (old_as != 0) {
        mem.as_destroy_h(old_as);
    }

    f = (struct exec_frame *)frame_p;
    f->rip = entry;
    f->usersp = sp;
    return 0;                          /* epilogue returns RAX into new image */
}

/* ---------------------------------------------------------------------------
 * Kernel-side monitor, two phases:
 *   1. wait for init's two COW test children -> print stage-3 results
 *   2. wait for pid 1 (init execve'd into sh) to exit, or a 30 s interactive
 *      grace period -> release kmain for the final banner
 * ------------------------------------------------------------------------- */

static void uproc_monitor(u64 arg) {
    u64 start_ticks = (u64)arg;

    while (uproc_test_exits < UPROC_TEST_CHILDREN
           && sched_tick_count() < start_ticks + MONITOR_PHASE1_TICKS) {
        sched_yield();
    }

    kprintf("\n[UPROC] Monitor: %u COW test children exited\n",
            (u64)uproc_test_exits);
    kprintf("      SYSCALLs served: %u, ring-3 timer ticks: %u\n",
            syscall_count_total(), sched_user_tick_count());

    if (uproc_test_exits >= UPROC_TEST_CHILDREN) {
        kprintf("      [PASS] Ring-3 entry: pid 1 ran via IRET trampoline and execved /bin/sh\n");
        kprintf("      [PASS] SYSCALL/SYSRET: file/process/memory/dir syscalls served (%u calls)\n",
                syscall_count_total());
        kprintf("      [PASS] COW fork across user processes: %u test children forked and reaped\n",
                (u64)UPROC_TEST_CHILDREN);
    } else {
        kprintf("      [FAIL] COW test children did not all exit before the deadline\n");
    }
    if (sched_user_tick_count() > 0) {
        kprintf("      [PASS] Timer preemption of ring-3 threads (%u user-mode ticks)\n",
                sched_user_tick_count());
    } else {
        kprintf("      [FAIL] no APIC timer tick ever interrupted ring 3\n");
    }

    /* Phase 2: under the automated test the shell exits on `exit N`; in a
     * manual session the timeout simply releases kmain while the shell keeps
     * running on its own thread. */
    while (!uproc_pid1_done
           && sched_tick_count() < start_ticks + MONITOR_PHASE2_TICKS) {
        sched_yield();
    }
    if (uproc_pid1_done) {
        kprintf("[UPROC] pid 1 (init -> /bin/sh) exited cleanly\n");
    } else {
        kprintf("[UPROC] interactive shell still running after 30 s monitor window\n");
    }

    uproc_done = 1;
    kthread_exit();
}

void uproc_selftest(void) {
    /* argv[0] presented to pid 1 ("/bin/init"), in a fixed 128-byte slot. */
    static char init_argbuf[EXEC_ARG_SLOT] = {
        '/', 'b', 'i', 'n', '/', 'i', 'n', 'i', 't', 0
    };
    u64 entry = 0;
    u64 user_rsp = 0;
    u64 as_h = 0;
    u32 ino;
    u64 kva;
    u64 size;
    u32 pid;

    kprintf("\n[UPROC] Ring-3 User Processes + SYSCALL/SYSRET Online:\n");
    kprintf("      MSR STAR/LSTAR/SFMASK + EFER.SCE programmed, GDT SYSRET-compatible\n");
    kprintf("      Per-process PML4 (kernel slots shared), demand-paged user stack\n");

    syscall_init();
    uproc_test_exits = 0;
    uproc_pid1_done = 0;
    uproc_done = 0;

    /* pid 1 is loaded out of Ramfs (/bin/init, published from the incbin
     * rootfs) exactly like any later execve image. */
    ino = fs.fs_lookup("/bin/init");
    kva = fs.fs_ino_data(ino);
    size = fs.fs_ino_size(ino);
    if (fs.fs_ino_kind(ino) != EXEC_INO_FILE
        || !proc_build_image(kva, size, 1, init_argbuf, &entry, &user_rsp, &as_h)) {
        kprintf("      [FAIL] could not load /bin/init from Ramfs\n");
        uproc_done = 1;
        return;
    }

    pid = sched_alloc_pid();
    if (kthread_create_user(entry, user_rsp, as_h, pid) == 0xFFFFFFFF) {
        kprintf("      [FAIL] could not create pid 1\n");
        mem.as_destroy_h(as_h);
        uproc_done = 1;
        return;
    }
    kprintf("      Started pid %u from /bin/init (entry %p, stack top %p)\n",
            (u64)pid, entry, (u64)UPROC_STACK_TOP);

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
