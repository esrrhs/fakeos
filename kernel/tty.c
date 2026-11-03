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
 * arrives. UART (IRQ4) and PS/2 keyboard (IRQ1) both feed this path: UART
 * via its own ring, keyboard via tty_push_byte. Sleepers wake on either
 * source; empty checks use sched_sleep_cli so a byte cannot be lost between
 * poll and sleep. Echo goes to UART and VGA.
 * ------------------------------------------------------------------------- */

enum {
    LINE_MAX = 256,
    IN_CAP = 512
};

static char tty_buf[LINE_MAX];
static u32 tty_editing;
static u32 tty_line_len;
static u32 tty_pos;

/* Shared input ring for PS/2 (and any non-UART source). UART still has its
 * own ring in drivers/uart.c; both feed tty_getc_block. */
static char tty_in[IN_CAP];
static u32 tty_in_head;
static u32 tty_in_tail;
static u32 tty_in_count;

extern u64 sched_current_as_h(void);
extern void sched_sleep_cli(u64 chan);
extern void sched_wakeup(u64 chan);
extern void cpu_cli(void);
extern void cpu_sti(void);

static u32 tty_wait_token;
static u32 tty_fg_pid;          /* foreground process for SIGINT (0=reader) */
static u32 tty_got_intr;        /* Ctrl-C interrupted the current read */

extern u32 sched_current_pid(void);
extern u32 sched_signal_send(u32 pid, u32 sig);
extern u32 sched_has_pending_fatal(void);

enum {
    SIGINT_N = 2,
    EINTR_NEG = (u64)-4
};

void tty_set_fg(u32 pid) {
    tty_fg_pid = pid;
}

u32 tty_get_fg(void) {
    return tty_fg_pid;
}

void tty_on_uart_byte(void) {
    sched_wakeup((u64)&tty_wait_token);
}

/* Called from kbd_irq (interrupts masked). */
void tty_push_byte(char c) {
    if (tty_in_count >= IN_CAP) {
        return;
    }
    tty_in[tty_in_tail] = c;
    tty_in_tail++;
    if (tty_in_tail >= IN_CAP) {
        tty_in_tail = 0;
    }
    tty_in_count++;
    sched_wakeup((u64)&tty_wait_token);
}

static int tty_in_pop(void) {
    char c;
    if (tty_in_count == 0) {
        return -1;
    }
    c = tty_in[tty_in_head];
    tty_in_head++;
    if (tty_in_head >= IN_CAP) {
        tty_in_head = 0;
    }
    tty_in_count--;
    return (int)(unsigned char)c;
}

static int tty_getc_block(void) {
    int c;
    /* Syscall entry clears IF; allow device IRQs, then close the race under
     * CLI so wakeup cannot land between an empty poll and sleep. */
    cpu_sti();
    cpu_cli();
    for (;;) {
        if (sched_has_pending_fatal()) {
            cpu_sti();
            return -2;                  /* interrupted */
        }
        c = tty_in_pop();
        if (c < 0) {
            c = drivers.uart_getc_nonblock();
        }
        if (c >= 0) {
            cpu_sti();
            return c;
        }
        sched_sleep_cli((u64)&tty_wait_token);
        /* Still CLI after sleep_cli; re-check the ring / signals. */
    }
}

static void tty_recv_line(void) {
    for (;;) {
        int c = tty_getc_block();
        if (c == -2 || sched_has_pending_fatal()) {
            /* Woken by kill while blocked on input. */
            tty_got_intr = 1;
            tty_editing = 0;
            tty_line_len = 0;
            tty_pos = 0;
            return;
        }
        if (c == 0x03) {
            /* Ctrl-C: echo, reset line, SIGINT to fg (or current reader). */
            drivers.uart_putc('^');
            drivers.uart_putc('C');
            drivers.uart_putc('\n');
            drivers.vga_putc('^');
            drivers.vga_putc('C');
            drivers.vga_putc('\n');
            tty_editing = 0;
            tty_line_len = 0;
            tty_pos = 0;
            tty_got_intr = 1;
            /* Only signal a declared foreground job. fg==0 means the
             * reader (usually the shell at a prompt) just wants EINTR. */
            if (tty_fg_pid != 0) {
                sched_signal_send(tty_fg_pid, SIGINT_N);
            }
            return;
        }
        if (c == '\n' || c == '\r') {
            tty_buf[tty_editing] = '\n';
            tty_line_len = tty_editing + 1;
            tty_pos = 0;
            tty_editing = 0;
            drivers.uart_putc('\n');
            drivers.vga_putc('\n');
            return;
        }
        if (c == 0x7F || c == 0x08) {
            if (tty_editing > 0) {
                tty_editing--;
                drivers.uart_putc('\b');
                drivers.uart_putc(' ');
                drivers.uart_putc('\b');
                drivers.vga_putc('\b');
                drivers.vga_putc(' ');
                drivers.vga_putc('\b');
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            if (tty_editing < LINE_MAX - 1) {
                tty_buf[tty_editing] = (char)c;
                tty_editing++;
                drivers.uart_putc((char)c);
                drivers.vga_putc((char)c);
            } else {
                tty_buf[tty_editing] = '\n';
                tty_line_len = tty_editing + 1;
                tty_pos = 0;
                tty_editing = 0;
                drivers.uart_putc('\n');
                drivers.vga_putc('\n');
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
    tty_got_intr = 0;
    while (done < len) {
        u64 n;
        u64 i;
        if (tty_line_len == 0 || tty_pos >= tty_line_len) {
            tty_line_len = 0;
            tty_pos = 0;
            tty_recv_line();
            if (tty_got_intr) {
                return EINTR_NEG;
            }
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
