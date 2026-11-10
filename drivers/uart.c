package drivers;
import arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern u64 cpu_irq_save(void);
extern void cpu_irq_restore(u64 flags);
extern void spin_lock(volatile u32 *lock);
extern void spin_unlock(volatile u32 *lock);
static volatile u32 uart_rx_lock;

/* Standard COM1. RX path can be polled or IRQ-driven (I/O APIC IRQ4);
 * received bytes land in a small ring that tty drains. */

static u16 COM1 = 0x03F8;

enum {
    UART_RX_CAP = 512,
    UART_IER_RDA = 0x01,             /* received-data available */
    UART_MCR_OUT2 = 0x08             /* AUX2 gates the interrupt line */
};

static char uart_rx[UART_RX_CAP];
static u32 uart_rx_head;
static u32 uart_rx_tail;
static u32 uart_rx_count;
static u32 uart_irq_enabled;
static volatile u64 uart_irq_count;

void uart_init(void) {
    arch.outb(COM1 + 1, 0x00);       /* IER: interrupts off for now */
    arch.outb(COM1 + 3, 0x80);       /* DLAB on */
    arch.outb(COM1 + 0, 0x01);       /* divisor lo: 115200 */
    arch.outb(COM1 + 1, 0x00);       /* divisor hi */
    arch.outb(COM1 + 3, 0x03);       /* 8N1 */
    arch.outb(COM1 + 2, 0xC7);       /* FIFO on, clear, 14-byte trigger */
    arch.outb(COM1 + 4, 0x0B);       /* DTR|RTS|OUT2 */
    uart_rx_head = 0;
    uart_rx_tail = 0;
    uart_rx_count = 0;
    uart_irq_enabled = 0;
    uart_irq_count = 0;
}

/* Arm received-data interrupts. Safe to call after I/O APIC routes IRQ4. */
void uart_enable_rx_irq(void) {
    u8 mcr = arch.inb(COM1 + 4);
    arch.outb(COM1 + 4, (u8)(mcr | UART_MCR_OUT2));
    arch.outb(COM1 + 1, UART_IER_RDA);
    uart_irq_enabled = 1;
}

static void uart_rx_push(char c) {
    if (uart_rx_count >= UART_RX_CAP) {
        return;                      /* drop on overrun */
    }
    uart_rx[uart_rx_tail] = c;
    uart_rx_tail++;
    if (uart_rx_tail >= UART_RX_CAP) {
        uart_rx_tail = 0;
    }
    uart_rx_count++;
}

/* Called from isr_dispatch on the COM1 vector with interrupts masked. */
static void uart_irq_locked(void) {
    u32 drained = 0;
    /* Drain the FIFO while LSR.DR is set. */
    while ((arch.inb(COM1 + 5) & 0x01) != 0) {
        char c = (char)(arch.inb(COM1 + 0) & 0xFF);
        uart_rx_push(c);
        drained++;
    }
    if (drained) {
        uart_irq_count++;
    }
}

void uart_irq(void) {
    u64 flags = cpu_irq_save();
    spin_lock(&uart_rx_lock);
    uart_irq_locked();
    spin_unlock(&uart_rx_lock);
    cpu_irq_restore(flags);
}

u64 uart_irq_count_total(void) {
    return uart_irq_count;
}

static int uart_is_transmit_empty(void) {
    return (arch.inb(COM1 + 5) & 0x20) != 0;
}

void uart_putc(char c) {
    if (c == '\n') {
        while (!uart_is_transmit_empty()) {
        }
        arch.outb(COM1, '\r');
    }
    while (!uart_is_transmit_empty()) {
    }
    arch.outb(COM1, (u8)c);
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s);
        s++;
    }
}

/* Non-blocking receive: prefer the IRQ ring, then fall back to LSR polling
 * so early-boot and no-APIC paths keep working. */
static int uart_getc_locked(void) {
    char c;
    if (uart_rx_count > 0) {
        c = uart_rx[uart_rx_head];
        uart_rx_head++;
        if (uart_rx_head >= UART_RX_CAP) {
            uart_rx_head = 0;
        }
        uart_rx_count--;
        return (int)(u8)c;
    }
    if ((arch.inb(COM1 + 5) & 0x01) == 0) {
        return -1;
    }
    return (int)(arch.inb(COM1 + 0) & 0xFF);
}

int uart_getc_nonblock(void) {
    int c;
    u64 flags = cpu_irq_save();
    spin_lock(&uart_rx_lock);
    c = uart_getc_locked();
    spin_unlock(&uart_rx_lock);
    cpu_irq_restore(flags);
    return c;
}
