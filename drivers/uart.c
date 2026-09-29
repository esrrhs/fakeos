package drivers;
import arch;
import types;

// Standard COM1 Base I/O Port
static types.uint16_t COM1 = 0x03F8;

void uart_init(void) {
    // Disable all UART interrupts
    arch.outb(COM1 + 1, 0x00);

    // Enable DLAB (set baud rate divisor)
    arch.outb(COM1 + 3, 0x80);

    // Set divisor to 1 (lo byte = 1, hi byte = 0) -> 115200 baud rate
    arch.outb(COM1 + 0, 0x01);
    arch.outb(COM1 + 1, 0x00);

    // 8 bits, no parity, one stop bit (8N1)
    arch.outb(COM1 + 3, 0x03);

    // Enable FIFO, clear transmit & receive queues, with 14-byte threshold
    arch.outb(COM1 + 2, 0xC7);

    // Turn on RTS/DSR and auxiliary output 2 (enables interrupt line)
    arch.outb(COM1 + 4, 0x0B);
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
    arch.outb(COM1, (types.uint8_t)c);
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s);
        s++;
    }
}
