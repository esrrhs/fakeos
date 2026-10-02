package kernel;
import arch;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);
extern u64 cr2_read(void);
extern void sched_tick(void);
extern void sched_note_user_tick(void);
extern u64 sched_tick_count(void);
extern u32 sched_current_pid(void);
extern void proc_terminate(u64 code);

/* Layout produced by isr_common in arch/x86_64/desc.asm (offsets grow as
 * registers are popped in reverse push order): */
struct trap_frame {
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbp;
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 rcx;
    u64 rbx;
    u64 rax;
    u64 vector;
    u64 error;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

/* CPU architectural exception vectors 0..31 (Intel SDM Vol.3 Ch.6). */
static const char *exception_names[32] = {
    "#DE Divide Error",
    "#DB Debug",
    "NMI Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection",
    "#PF Page Fault",
    "Reserved (15)",
    "#MF x87 Floating-Point",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating-Point",
    "#VE Virtualization",
    "#CP Control Protection",
    "Reserved (22)",
    "Reserved (23)",
    "Reserved (24)",
    "Reserved (25)",
    "Reserved (26)",
    "Reserved (27)",
    "Reserved (28)",
    "Reserved (29)",
    "Reserved (30)",
    "Reserved (31)"
};

/* Test arming state: when set, the expected exception is intercepted and
 * execution is redirected to a pre-registered recovery RIP instead of
 * halting the kernel. */
static int test_armed;
static u32 test_expect;
static u64 test_recover_rip;
extern void exc_de_trigger(void);
extern void exc_de_recover(void);
extern void exc_ud_trigger(void);
extern void exc_ud_recover(void);
extern void exc_pf_trigger(void);
extern void exc_pf_recover(void);

void isr_arm_test(u32 vector, u64 recover_rip) {
    test_armed = 1;
    test_expect = vector;
    test_recover_rip = recover_rip;
}

static void dump_page_fault_error(u64 error) {
    kprintf("  [PF ] CR2 = %p  error = 0x%x (", cr2_read(), (u32)error);
    if (error & 0x01) {
        kprintf("protection violation");
    } else {
        kprintf("non-present page");
    }
    if (error & 0x02) kprintf(", write"); else kprintf(", read");
    if (error & 0x04) kprintf(", user"); else kprintf(", supervisor");
    if (error & 0x08) kprintf(", reserved-bit");
    if (error & 0x10) kprintf(", instruction fetch");
    kprintf(")\n");
}

static void dump_frame(const char *name, struct trap_frame *f) {
    kprintf("\n*** CPU EXCEPTION: vector=%u  %s ***\n", (u32)f->vector, name);
    kprintf("  RIP=%p CS=0x%x RFLAGS=0x%x RSP=%p SS=0x%x ERR=0x%x\n",
            f->rip, (u32)f->cs, (u32)f->rflags, f->rsp, (u32)f->ss, (u32)f->error);
    kprintf("  RAX=%p RBX=%p RCX=%p RDX=%p\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("  RSI=%p RDI=%p RBP=%p\n", f->rsi, f->rdi, f->rbp);
    kprintf("  R8 =%p R9 =%p R10=%p R11=%p\n", f->r8, f->r9, f->r10, f->r11);
    kprintf("  R12=%p R13=%p R14=%p R15=%p\n", f->r12, f->r13, f->r14, f->r15);
    if (f->vector == 14) {
        dump_page_fault_error(f->error);
    }
}

void isr_dispatch(struct trap_frame *f) {
    u32 v = (u32)f->vector;

    /* Vector 32: periodic Local APIC timer tick -> preemptive scheduler. */
    if (v == 32) {
        if ((f->cs & 3) == 3) {
            sched_note_user_tick();     /* tick interrupted a ring-3 thread */
        }
        sched_tick();
        return;
    }

    /* Vector 255: Local APIC spurious interrupt. A genuine spurious delivery
     * leaves ISR clear and requires no EOI; but a stray pulse accepted as a
     * fixed interrupt at 255 sets ISR[255] and would pin PPR at priority 15,
     * blocking the timer - in that case EOI is mandatory. */
    if (v == 255) {
        if (arch.lapic_spurious_in_service()) {
            arch.lapic_send_eoi();
        }
        return;
    }

    /* A process address space is installed: give VMA demand-paging / COW the
     * first chance to resolve the fault and resume the faulting instruction. */
    if (mem.as_current_handle() != 0) {
        if (mem.as_dispatch_fault(1, cr2_read(), f->error)) {
            return;
        }
    }

    /* Unresolvable fault at ring 3 (bad pointer, protection violation, illegal
     * opcode, ...): kill the offending user process, not the kernel. */
    if ((f->cs & 3) == 3) {
        const char *uname = (v < 32) ? exception_names[v] : "Unknown Interrupt";
        dump_frame(uname, f);
        kprintf("  [UPROC] pid %u killed: unhandled ring-3 fault vector=%u\n",
                (u64)sched_current_pid(), v);
        proc_terminate(0xFFFFFFFF);
    }

    /* Controlled self-test: redirect to the recovery label and continue. */
    if (test_armed && v == test_expect) {
        kprintf("  [CAUGHT] vector=%u as expected, recovering via RIP=%p\n",
                v, test_recover_rip);
        if (v == 14) {
            dump_page_fault_error(f->error);
        }
        f->rip = test_recover_rip;
        test_armed = 0;
        return;
    }

    const char *name = (v < 32) ? exception_names[v] : "Unknown Interrupt";
    dump_frame(name, f);

    kprintf("  [FATAL] Unhandled interrupt vector %u - system halted.\n", v);
    arch.cpu_cli();
    for (;;) {
        arch.cpu_halt();
    }
}

void isr_run_tests(void) {
    kprintf("\n[INT] Running CPU Exception Capture Self-Tests...\n");

    isr_arm_test(0, (u64)(&exc_de_recover));
    exc_de_trigger();
    kprintf("      [PASS] Exception #DE (divide by zero) caught, register dump OK, recovered\n");

    isr_arm_test(6, (u64)(&exc_ud_recover));
    exc_ud_trigger();
    kprintf("      [PASS] Exception #UD (undefined opcode) caught, register dump OK, recovered\n");

    isr_arm_test(14, (u64)(&exc_pf_recover));
    exc_pf_trigger();
    kprintf("      [PASS] Exception #PF (write above the 1 GiB boot window) caught, CR2/error decoded, recovered\n");
}
