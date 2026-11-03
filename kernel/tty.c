package kernel;
import drivers;
import mem;
import types;

typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* ---------------------------------------------------------------------------
 * Serial TTY: canonical line discipline over COM1.
 *
 * read(0) blocks until a full line has been received, echoing input as it
 * arrives. With I/O APIC IRQ4 + UART RDA interrupts the RX path sleeps on a
 * wait channel; uart_irq drains into the UART ring and tty_on_uart_byte wakes
 * sleepers. Empty-ring checks run under CLI with sched_sleep_cli so a byte
 * that arrives between poll and sleep cannot be lost.
 * ------------------------------------------------------------------------- */

enum {
    LINE_MAX = 256
};

static char tty_buf[LINE_MAX];
static u32 tty_editing;
static u32 tty_line_len;
static u32 tty_pos;

extern u64 sched_current_as_h(void);
extern void sched_sleep_cli(u64 chan);
extern void sched_wakeup(u64 chan);
extern void cpu_cli(void);
extern void cpu_sti(void);

static u32 tty_wait_token;

void tty_on_uart_byte(void) {
    sched_wakeup((u64)&tty_wait_token);
}

static int tty_getc_block(void) {
    int c;
    /* Syscall entry clears IF; allow device IRQs, then close the race under
     * CLI so wakeup cannot land between an empty poll and sleep. */
    cpu_sti();
    cpu_cli();
    for (;;) {
        c = drivers.uart_getc_nonblock();
        if (c >= 0) {
            cpu_sti();
            return c;
        }
        sched_sleep_cli((u64)&tty_wait_token);
        /* Still CLI after sleep_cli; re-check the ring. */
    }
}

static void tty_recv_line(void) {
    for (;;) {
        int c = tty_getc_block();
        if (c == '\n' || c == '\r') {
            tty_buf[tty_editing] = '\n';
            tty_line_len = tty_editing + 1;
            tty_pos = 0;
            tty_editing = 0;
            drivers.uart_putc('\n');
            return;
        }
        if (c == 0x7F || c == 0x08) {
            if (tty_editing > 0) {
                tty_editing--;
                drivers.uart_putc('\b');
                drivers.uart_putc(' ');
                drivers.uart_putc('\b');
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            if (tty_editing < LINE_MAX - 1) {
                tty_buf[tty_editing] = (char)c;
                tty_editing++;
                drivers.uart_putc((char)c);
            } else {
                tty_buf[tty_editing] = '\n';
                tty_line_len = tty_editing + 1;
                tty_pos = 0;
                tty_editing = 0;
                drivers.uart_putc('\n');
                return;
            }
        }
    }
}

u64 tty_console_read(u64 ubuf, u64 len) {
    u64 as = sched_current_as_h();
    u64 done = 0;
    if (as == 0 || len == 0) {
        return (u64)(-1);
    }
    if (!mem.as_user_range_ok(as, ubuf, len)) {
        return (u64)(-1);
    }
    while (done < len) {
        u64 n;
        u64 i;
        if (tty_line_len == 0 || tty_pos >= tty_line_len) {
            tty_line_len = 0;
            tty_pos = 0;
            tty_recv_line();
        }
        n = tty_line_len - tty_pos;
        if (n > len - done) {
            n = len - done;
        }
        {
            volatile char *u = (volatile char *)(ubuf + done);
            for (i = 0; i < n; i++) {
                u[i] = tty_buf[tty_pos + i];
            }
        }
        tty_pos += (u32)n;
        done += n;
        if (tty_pos >= tty_line_len) {
            tty_line_len = 0;
            tty_pos = 0;
            break;
        }
    }
    return done;
}
