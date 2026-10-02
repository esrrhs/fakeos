package arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void outb(u16 port, u8 val);
extern void io_wait(void);
extern void idt_load(void);

/* Interrupt Descriptor Table, 256 x 16-byte gates, stored as raw qwords
 * (16-byte gate descriptor split into two u64 halves; see Intel SDM):
 *
 *   low : offset15:0 | selector<<16 | IST<<32 | flags<<40 | offset31:16<<48
 *   high: offset63:32 (rest reserved zero)
 *
 * Raw storage avoids any compiler struct-layout ambiguity for the exact
 * hardware binary layout. Referenced by idtr in desc.asm. */
u64 idt[512];

/* Stub entry table generated in desc.asm: vectors 0..47. */
extern u64 isr_stub_table[];
extern void isr_stub_255(void);

static void idt_set_gate(u8 vector, u64 handler, u8 ist, u8 flags) {
    idt[2 * vector] = (handler & 0x000000000000FFFFULL)
                    | (0x08ULL << 16)
                    | (((u64)ist) << 32)
                    | (((u64)flags) << 40)
                    | ((handler & 0x00000000FFFF0000ULL) << 32);
    idt[2 * vector + 1] = handler >> 32;
}

/* Permanently mask the legacy 8259A PIC. fakeos routes all device interrupts
 * through the modern Local APIC / I/O APIC path, so the vintage PIC must be
 * silenced to stop IRQ0/IRQ1 etc. from colliding with CPU exception vectors. */
void pic_disable(void) {
    outb(0x21, 0xFF);
    io_wait();
    outb(0xA1, 0xFF);
    io_wait();
}

void idt_init(void) {
    u32 v;
    for (v = 0; v < 48; v++) {
        /* #DF (vector 8) runs on the dedicated IST1 guard stack. */
        u8 ist = (v == 8) ? 1 : 0;
        idt_set_gate((u8)v, isr_stub_table[v], ist, 0x8E);
    }
    /* Local APIC spurious vector (handler returns without EOI). */
    idt_set_gate(255, (u64)(&isr_stub_255), 0, 0x8E);

    pic_disable();
    idt_load();
}

u64 idt_get_base(void) {
    return (u64)(&idt[0]);
}
