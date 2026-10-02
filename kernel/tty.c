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
 * arrives. Editing buffer state:
 *   buf[0..editing) - line currently being typed (raw chars, no newline)
 *   line_len        - once Enter arrives, total ready length = editing+1,
 *                     buf[editing] holds '\n'; pos walks the ready line
 * A read() consumes up to len bytes; leftovers stay for the next read.
 *
 * Polling yields the CPU while no byte is present so other threads (and the
 * APIC tick) keep running; there is no UART IRQ line in use.
 * ------------------------------------------------------------------------- */

enum {
    LINE_MAX = 256
};

static char tty_buf[LINE_MAX];
static u32 tty_editing;              /* chars typed but not yet terminated */
static u32 tty_line_len;             /* 0 = no ready line, else ready count */
static u32 tty_pos;                  /* consumed bytes of the ready line   */

extern u64 sched_current_as_h(void);
extern void sched_yield(void);

/* Collect bytes until the line is complete. */
static void tty_recv_line(void) {
    for (;;) {
        int c = drivers.uart_getc_nonblock();
        if (c < 0) {
            sched_yield();
            continue;
        }
        /* Terminals send CR (0x0D); QEMU serial stdio may send LF (0x0A).
         * Either one terminates the line. */
        if (c == '\n' || c == '\r') {
            tty_buf[tty_editing] = '\n';
            tty_line_len = tty_editing + 1;
            tty_pos = 0;
            tty_editing = 0;
            drivers.uart_putc('\n');      /* driver emits CR+LF */
            return;
        }
        /* Rubout: DEL (0x7F) or BS (0x08) erases one char; the "\b \b"
         * sequence moves the cursor back, wipes the glyph and backs again. */
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
                /* Buffer saturated: force line termination rather than
                 * silently dropping input. */
                tty_buf[tty_editing] = '\n';
                tty_line_len = tty_editing + 1;
                tty_pos = 0;
                tty_editing = 0;
                drivers.uart_putc('\n');
                return;
            }
        }
        /* Control chars other than the handled ones are ignored. */
    }
}

u64 tty_console_read(u64 ubuf, u64 len) {
    u64 as = sched_current_as_h();
    u64 n;
    u64 i;
    u64 pg;
    volatile char *u;
    if (as == 0 || len == 0) {
        return (u64)(-1);
    }
    /* Validate the full destination BEFORE blocking on a line, so a bad
     * buffer cannot consume a line of input with nowhere to deliver it. */
    if (ubuf + len < ubuf) {
        return (u64)(-1);
    }
    pg = ubuf & ~0xFFFULL;
    while (pg < ubuf + len) {
        if (!mem.as_user_range_ok(as, pg, 4096)) {
            return (u64)(-1);
        }
        pg += 4096;
    }
    if (tty_line_len == 0) {
        tty_recv_line();
    }
    n = tty_line_len - tty_pos;
    if (n > len) {
        n = len;
    }
    if (!mem.as_user_range_ok(as, ubuf, n)) {
        return (u64)(-1);
    }
    u = (volatile char *)ubuf;
    for (i = 0; i < n; i++) {
        u[i] = tty_buf[tty_pos + i];
    }
    tty_pos += (u32)n;
    if (tty_pos >= tty_line_len) {
        tty_line_len = 0;
        tty_pos = 0;
    }
    return n;
}
