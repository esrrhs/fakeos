package kernel;
import drivers;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

/* ---------------------------------------------------------------------------
 * Serial TTY: canonical line discipline over COM1.
 *
 * read(0) blocks until a full line has been received, echoing input as it
 * arrives. UART (IRQ4) and PS/2 keyboard (IRQ1) both feed this path: UART
 * via its own ring, keyboard via tty_push_byte. Sleepers wake on either
 * source; empty checks use sched_sleep_cli so a byte cannot be lost between
 * poll and sleep. Echo goes to UART and VGA. Arrow keys (CSI A/B) recall
 * a 32-entry line history shared by serial and PS/2. Left/Right + Ctrl-A/E/U
 * provide basic readline-style editing. ISIG: Ctrl-C/Ctrl-Z are intercepted
 * on input (UART IRQ + PS/2) even while the shell is blocked in wait4 when
 * termios c_lflag has ISIG. TCGETS/TCSETS expose ICANON/ECHO/ISIG/ICRNL +
 * c_cc; clearing ICANON switches console read to raw byte mode.
 * ------------------------------------------------------------------------- */

enum {
    LINE_MAX = 256,
    IN_CAP = 512
};

static char tty_buf[LINE_MAX];
static u32 tty_editing;
static u32 tty_cursor;          /* insertion point within tty_editing */
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
extern void select_kick(void);
extern void cpu_cli(void);
extern void cpu_sti(void);

static u32 tty_wait_token;
static u32 tty_fg_pgid;         /* foreground process group for ISIG (0=none) */
static u32 tty_got_intr;        /* Ctrl-C interrupted the current read */

/* termios (Linux-ish): stored separately from the 64-byte user TCGETS blob. */
enum {
    TTY_ISIG   = 0x00000001,
    TTY_ICANON = 0x00000002,
    TTY_ECHO   = 0x00000008,
    TTY_ICRNL  = 0x00000100,
    TTY_OPOST  = 0x00000001,
    TTY_ONLCR  = 0x00000004,
    TTY_CS8    = 0x00000030,
    TTY_CREAD  = 0x00000080,
    TTY_VINTR  = 0,
    TTY_VQUIT  = 1,
    TTY_VERASE = 2,
    TTY_VKILL  = 3,
    TTY_VEOF   = 4,
    TTY_VMIN   = 6,
    TTY_VSUSP  = 10,
    TTY_NCCS   = 20
};
static u32 tty_iflag = TTY_ICRNL;
static u32 tty_oflag = TTY_OPOST | TTY_ONLCR;
static u32 tty_cflag = TTY_CS8 | TTY_CREAD;
static u32 tty_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO;
static u8 tty_cc[TTY_NCCS];
static u32 tty_termios_inited;

static void tty_termios_ensure(void) {
    u32 i;
    if (tty_termios_inited) {
        return;
    }
    for (i = 0; i < TTY_NCCS; i++) {
        tty_cc[i] = 0;
    }
    tty_cc[TTY_VINTR] = 0x03;
    tty_cc[TTY_VQUIT] = 0x1C;
    tty_cc[TTY_VERASE] = 0x7F;
    tty_cc[TTY_VKILL] = 0x15;
    tty_cc[TTY_VEOF] = 0x04;
    tty_cc[TTY_VMIN] = 1;
    tty_cc[TTY_VSUSP] = 0x1A;
    tty_termios_inited = 1;
}

void tty_termios_get(u8 *dst, u32 n) {
    u32 i;
    tty_termios_ensure();
    for (i = 0; i < n; i++) {
        dst[i] = 0;
    }
    if (n < 16) {
        return;
    }
    dst[0] = (u8)(tty_iflag & 0xFF);
    dst[1] = (u8)((tty_iflag >> 8) & 0xFF);
    dst[2] = (u8)((tty_iflag >> 16) & 0xFF);
    dst[3] = (u8)((tty_iflag >> 24) & 0xFF);
    dst[4] = (u8)(tty_oflag & 0xFF);
    dst[5] = (u8)((tty_oflag >> 8) & 0xFF);
    dst[6] = (u8)((tty_oflag >> 16) & 0xFF);
    dst[7] = (u8)((tty_oflag >> 24) & 0xFF);
    dst[8] = (u8)(tty_cflag & 0xFF);
    dst[9] = (u8)((tty_cflag >> 8) & 0xFF);
    dst[10] = (u8)((tty_cflag >> 16) & 0xFF);
    dst[11] = (u8)((tty_cflag >> 24) & 0xFF);
    dst[12] = (u8)(tty_lflag & 0xFF);
    dst[13] = (u8)((tty_lflag >> 8) & 0xFF);
    dst[14] = (u8)((tty_lflag >> 16) & 0xFF);
    dst[15] = (u8)((tty_lflag >> 24) & 0xFF);
    if (n > 16) {
        dst[16] = 0;
    }
    for (i = 0; i < TTY_NCCS && (17 + i) < n; i++) {
        dst[17 + i] = tty_cc[i];
    }
}

void tty_termios_set(const u8 *src, u32 n) {
    u32 i;
    tty_termios_ensure();
    if (n < 16) {
        return;
    }
    tty_iflag = (u32)src[0] | ((u32)src[1] << 8)
              | ((u32)src[2] << 16) | ((u32)src[3] << 24);
    tty_oflag = (u32)src[4] | ((u32)src[5] << 8)
              | ((u32)src[6] << 16) | ((u32)src[7] << 24);
    tty_cflag = (u32)src[8] | ((u32)src[9] << 8)
              | ((u32)src[10] << 16) | ((u32)src[11] << 24);
    tty_lflag = (u32)src[12] | ((u32)src[13] << 8)
              | ((u32)src[14] << 16) | ((u32)src[15] << 24);
    for (i = 0; i < TTY_NCCS && (17 + i) < n; i++) {
        tty_cc[i] = src[17 + i];
    }
}

/* Arrow-key history: completed lines recalled with CSI A/B (Up/Down). */
enum { TTY_HIST_MAX = 32 };
static char tty_hist[TTY_HIST_MAX][LINE_MAX];
static u32 tty_hist_len;
static u32 tty_hist_next;
static int tty_hist_view;       /* -1 = editing new; else index into ring */

extern u32 sched_current_pid(void);
extern u32 sched_current_pgid(void);
extern u32 sched_signal_send(u32 pid, u32 sig);
extern u32 sched_signal_pgrp(u32 pgid, u32 sig);
extern u32 sched_has_pending_fatal(void);
extern u32 sched_should_eintr(void);
extern void sched_check_signals(void);

enum {
    SIGINT_N  = 2,
    SIGTSTP_N = 20,
    SIGTTIN_N = 21,
    SIGTTOU_N = 22,
    EINTR_NEG = (u64)-4,
    EIO_NEG   = (u64)-5
};

extern u32 sched_sig_ignored(u32 sig);

void tty_set_fg(u32 pgid) {
    tty_fg_pgid = pgid;
}

u32 tty_get_fg(void) {
    return tty_fg_pgid;
}

/* True when a controlling fg pgid is set and the caller is not in it. */
u32 tty_is_background(void) {
    u32 pg;
    if (tty_fg_pgid == 0) {
        return 0;
    }
    pg = sched_current_pgid();
    if (pg == 0 || pg == tty_fg_pgid) {
        return 0;
    }
    return 1;
}

/* Job-control gate for console read (SIGTTIN) / write+tcsetpgrp (SIGTTOU).
 * Returns 0 = proceed, 1 = stopped+resumed (retry), 2 = ignored -> EIO. */
static u32 tty_jobctl_gate(u32 sig) {
    u32 pg;
    if (!tty_is_background()) {
        return 0;
    }
    if (sched_sig_ignored(sig)) {
        return 2;
    }
    pg = sched_current_pgid();
    sched_signal_pgrp(pg, sig);
    sched_check_signals();
    return 1;
}

/* Block until foreground (or EIO if SIGTTIN ignored). Returns 0 or EIO_NEG. */
u64 tty_jobctl_read_wait(void) {
    for (;;) {
        u32 g = tty_jobctl_gate(SIGTTIN_N);
        if (g == 0) {
            return 0;
        }
        if (g == 2) {
            return EIO_NEG;
        }
        /* Stopped on SIGTTIN; after SIGCONT re-check membership. */
    }
}

u64 tty_jobctl_write_wait(void) {
    for (;;) {
        u32 g = tty_jobctl_gate(SIGTTOU_N);
        if (g == 0) {
            return 0;
        }
        if (g == 2) {
            return EIO_NEG;
        }
    }
}

/* tcsetpgrp from a background pgrp: SIGTTOU then fail with EINTR.
 * If SIGTTOU is ignored/blocked, POSIX allows the call to proceed (job-control
 * shells ignore these signals so they can reclaim the TTY). */
u64 tty_jobctl_tcsetpgrp_check(void) {
    u32 pg;
    if (!tty_is_background()) {
        return 0;
    }
    if (sched_sig_ignored(SIGTTOU_N)) {
        return 0;
    }
    pg = sched_current_pgid();
    sched_signal_pgrp(pg, SIGTTOU_N);
    sched_check_signals();
    return EINTR_NEG;
}

static void tty_signal_fg(u32 sig) {
    if (tty_fg_pgid == 0) {
        return;
    }
    /* Prefer pgrp broadcast so pipelines stop/interrupt as a unit. */
    if (!sched_signal_pgrp(tty_fg_pgid, sig)) {
        /* Fallback: treat the value as a lone pid (pre-pgrp callers). */
        sched_signal_send(tty_fg_pgid, sig);
    }
}

static void tty_queue_byte(char c) {
    if (tty_in_count >= IN_CAP) {
        return;
    }
    tty_in[tty_in_tail] = c;
    tty_in_tail++;
    if (tty_in_tail >= IN_CAP) {
        tty_in_tail = 0;
    }
    tty_in_count++;
}

/* ISIG processing for UART + keyboard. Control chars never enter the line buf. */
static void tty_ingest_byte(char c) {
    u8 vintr;
    u8 vsusp;
    tty_termios_ensure();
    vintr = tty_cc[TTY_VINTR] != 0 ? tty_cc[TTY_VINTR] : 0x03;
    vsusp = tty_cc[TTY_VSUSP] != 0 ? tty_cc[TTY_VSUSP] : 0x1A;
    if ((tty_lflag & TTY_ISIG) != 0 && (u8)c == vintr) {
        if (tty_lflag & TTY_ECHO) {
            drivers.uart_putc('^');
            drivers.uart_putc('C');
            drivers.uart_putc('\n');
            drivers.vga_putc('^');
            drivers.vga_putc('C');
            drivers.vga_putc('\n');
        }
        tty_editing = 0;
        tty_cursor = 0;
        tty_line_len = 0;
        tty_pos = 0;
        tty_got_intr = 1;
        tty_signal_fg(SIGINT_N);
        sched_wakeup((u64)&tty_wait_token);
        select_kick();
        return;
    }
    if ((tty_lflag & TTY_ISIG) != 0 && (u8)c == vsusp) {
        if (tty_lflag & TTY_ECHO) {
            drivers.uart_putc('^');
            drivers.uart_putc('Z');
            drivers.uart_putc('\n');
            drivers.vga_putc('^');
            drivers.vga_putc('Z');
            drivers.vga_putc('\n');
        }
        tty_signal_fg(SIGTSTP_N);
        sched_wakeup((u64)&tty_wait_token);
        select_kick();
        return;
    }
    if ((tty_iflag & TTY_ICRNL) != 0 && c == '\r') {
        c = '\n';
    }
    tty_queue_byte(c);
    sched_wakeup((u64)&tty_wait_token);
    select_kick();
}


void tty_on_uart_byte(void) {
    /* Drain the UART IRQ ring through ISIG so Ctrl-C/Z work during wait4. */
    for (;;) {
        int c = drivers.uart_getc_nonblock();
        if (c < 0) break;
        tty_ingest_byte((char)c);
    }
}

/* Called from kbd_irq (interrupts masked). */
void tty_push_byte(char c) {
    tty_ingest_byte(c);
}

/* Non-blocking: bytes waiting in the shared ring or a completed line. */
u32 tty_input_pending(void) {
    if (tty_in_count > 0) {
        return 1;
    }
    if (tty_line_len > 0 && tty_pos < tty_line_len) {
        return 1;
    }
    return 0;
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
        if (tty_got_intr || sched_has_pending_fatal()) {
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
        /* Still CLI after sleep_cli; deliver stop/kill before re-polling. */
        cpu_sti();
        sched_check_signals();
        cpu_cli();
    }
}

static void tty_echo_ch(char c) {
    tty_termios_ensure();
    if ((tty_lflag & TTY_ECHO) == 0) {
        return;
    }
    drivers.uart_putc(c);
    drivers.vga_putc(c);
}

static void tty_rubout(void) {
    drivers.uart_putc('\b');
    drivers.uart_putc(' ');
    drivers.uart_putc('\b');
    drivers.vga_putc('\b');
    drivers.vga_putc(' ');
    drivers.vga_putc('\b');
}

static void tty_hist_remember(const char *line, u32 n) {
    u32 i;
    if (n == 0) return;
    if (n >= LINE_MAX) n = LINE_MAX - 1;
    /* Skip duplicate of most recent. */
    if (tty_hist_len > 0) {
        u32 prev = tty_hist_next == 0 ? TTY_HIST_MAX - 1 : tty_hist_next - 1;
        u32 same = 1;
        for (i = 0; i < n; i++) {
            if (tty_hist[prev][i] != line[i]) { same = 0; break; }
        }
        if (same && tty_hist[prev][n] == 0) return;
    }
    for (i = 0; i < n; i++) tty_hist[tty_hist_next][i] = line[i];
    tty_hist[tty_hist_next][n] = 0;
    tty_hist_next = (tty_hist_next + 1) % TTY_HIST_MAX;
    if (tty_hist_len < TTY_HIST_MAX) tty_hist_len++;
}

static void tty_clear_edit(void) {
    while (tty_editing > 0) {
        tty_rubout();
        tty_editing--;
    }
    tty_cursor = 0;
}

static void tty_move_cursor_to(u32 pos) {
    if (pos > tty_editing) pos = tty_editing;
    while (tty_cursor > pos) {
        drivers.uart_putc('\b');
        drivers.vga_putc('\b');
        tty_cursor--;
    }
    while (tty_cursor < pos) {
        tty_echo_ch(tty_buf[tty_cursor]);
        tty_cursor++;
    }
}

static void tty_redraw_tail(void) {
    u32 i;
    u32 save = tty_cursor;
    for (i = tty_cursor; i < tty_editing; i++) {
        tty_echo_ch(tty_buf[i]);
    }
    /* Erase leftover glyph if we deleted. */
    drivers.uart_putc(' ');
    drivers.vga_putc(' ');
    drivers.uart_putc('\b');
    drivers.vga_putc('\b');
    /* Walk back to insertion point. */
    i = tty_editing;
    while (i > save) {
        drivers.uart_putc('\b');
        drivers.vga_putc('\b');
        i--;
    }
    tty_cursor = save;
}

static void tty_hist_apply(int view) {
    u32 idx;
    u32 i;
    const char *s;
    if (tty_hist_len == 0) return;
    if (view < 0 || (u32)view >= tty_hist_len) return;
    idx = (tty_hist_len < TTY_HIST_MAX)
        ? (u32)view
        : (tty_hist_next + (u32)view) % TTY_HIST_MAX;
    s = tty_hist[idx];
    tty_clear_edit();
    for (i = 0; s[i] != 0 && tty_editing < LINE_MAX - 1; i++) {
        tty_buf[tty_editing] = s[i];
        tty_echo_ch(s[i]);
        tty_editing++;
    }
    tty_cursor = tty_editing;
    tty_hist_view = view;
}

static void tty_recv_line(void) {
    u32 esc_state = 0; /* 0=normal 1=saw ESC 2=saw ESC [ 3=CSI digits */
    u32 csi_num = 0;
    tty_hist_view = -1;
    tty_cursor = 0;
    for (;;) {
        int c = tty_getc_block();
        if (c == -2 || sched_should_eintr() || sched_has_pending_fatal()) {
            tty_got_intr = 1;
            tty_editing = 0;
            tty_cursor = 0;
            tty_line_len = 0;
            tty_pos = 0;
            return;
        }
        if (c == 0x03) {
            /* Backup ISIG path (polled RX before IRQ arming). */
            drivers.uart_putc('^');
            drivers.uart_putc('C');
            drivers.uart_putc('\n');
            drivers.vga_putc('^');
            drivers.vga_putc('C');
            drivers.vga_putc('\n');
            tty_editing = 0;
            tty_cursor = 0;
            tty_line_len = 0;
            tty_pos = 0;
            tty_got_intr = 1;
            tty_signal_fg(SIGINT_N);
            return;
        }
        if (c == 0x1A) {
            drivers.uart_putc('^');
            drivers.uart_putc('Z');
            drivers.uart_putc('\n');
            drivers.vga_putc('^');
            drivers.vga_putc('Z');
            drivers.vga_putc('\n');
            tty_signal_fg(SIGTSTP_N);
            /* Do not abort the reader's line; parent wait4(WUNTRACED) observes stop. */
            continue;
        }
        if (c == 0x01) { /* Ctrl-A home */
            tty_move_cursor_to(0);
            continue;
        }
        if (c == 0x05) { /* Ctrl-E end */
            tty_move_cursor_to(tty_editing);
            continue;
        }
        if (c == 0x15) { /* Ctrl-U kill line */
            tty_clear_edit();
            tty_hist_view = -1;
            continue;
        }
        if (c == '\n' || c == '\r') {
            tty_move_cursor_to(tty_editing);
            tty_hist_remember(tty_buf, tty_editing);
            tty_buf[tty_editing] = '\n';
            tty_line_len = tty_editing + 1;
            tty_pos = 0;
            tty_editing = 0;
            tty_cursor = 0;
            tty_echo_ch('\n');
            return;
        }
        if (esc_state == 1) {
            if (c == '[') {
                esc_state = 2;
                csi_num = 0;
                continue;
            }
            esc_state = 0;
            continue;
        }
        if (esc_state == 2 || esc_state == 3) {
            if (c >= '0' && c <= '9') {
                csi_num = csi_num * 10 + (u32)(c - '0');
                esc_state = 3;
                continue;
            }
            esc_state = 0;
            if (c == 'A') {
                int view = tty_hist_view;
                if (view < 0) view = (int)tty_hist_len - 1;
                else if (view > 0) view--;
                tty_hist_apply(view);
                continue;
            }
            if (c == 'B') {
                int view = tty_hist_view;
                if (view < 0) continue;
                if (view + 1 >= (int)tty_hist_len) {
                    tty_clear_edit();
                    tty_hist_view = -1;
                } else {
                    tty_hist_apply(view + 1);
                }
                continue;
            }
            if (c == 'C') { /* Right */
                if (tty_cursor < tty_editing) tty_move_cursor_to(tty_cursor + 1);
                continue;
            }
            if (c == 'D') { /* Left */
                if (tty_cursor > 0) tty_move_cursor_to(tty_cursor - 1);
                continue;
            }
            if (c == 'H' || (c == '~' && csi_num == 1)) { /* Home */
                tty_move_cursor_to(0);
                continue;
            }
            if (c == 'F' || (c == '~' && csi_num == 4)) { /* End */
                tty_move_cursor_to(tty_editing);
                continue;
            }
            continue;
        }
        if (c == 0x1B) {
            esc_state = 1;
            continue;
        }
        if (c == 0x7F || c == 0x08) {
            if (tty_cursor > 0) {
                u32 i;
                for (i = tty_cursor - 1; i + 1 < tty_editing; i++) {
                    tty_buf[i] = tty_buf[i + 1];
                }
                tty_editing--;
                tty_cursor--;
                drivers.uart_putc('\b');
                drivers.vga_putc('\b');
                tty_redraw_tail();
                tty_hist_view = -1;
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            if (tty_editing < LINE_MAX - 1) {
                u32 i;
                for (i = tty_editing; i > tty_cursor; i--) {
                    tty_buf[i] = tty_buf[i - 1];
                }
                tty_buf[tty_cursor] = (char)c;
                tty_editing++;
                tty_echo_ch((char)c);
                tty_cursor++;
                if (tty_cursor < tty_editing) tty_redraw_tail();
                tty_hist_view = -1;
            } else {
                tty_move_cursor_to(tty_editing);
                tty_buf[tty_editing] = '\n';
                tty_line_len = tty_editing + 1;
                tty_pos = 0;
                tty_editing = 0;
                tty_cursor = 0;
                tty_echo_ch('\n');
                return;
            }
        }
    }
}

u64 tty_console_read(u64 ubuf, u64 len) {
    u64 as = sched_current_as_h();
    u64 done = 0;
    u64 jc;
    tty_termios_ensure();
    if (as == 0 || len == 0) {
        return (u64)(-1);
    }
    if (!mem.as_user_range_ok(as, ubuf, len)) {
        return (u64)(-1);
    }
    jc = tty_jobctl_read_wait();
    if (jc != 0) {
        return jc;
    }
    tty_got_intr = 0;

    if ((tty_lflag & TTY_ICANON) == 0) {
        while (done < len) {
            int c;
            if (tty_got_intr || sched_should_eintr()) {
                return done != 0 ? done : EINTR_NEG;
            }
            c = tty_in_pop();
            if (c < 0) {
                c = drivers.uart_getc_nonblock();
            }
            if (c < 0) {
                if (done > 0 && tty_cc[TTY_VMIN] == 0) {
                    break;
                }
                cpu_sti();
                cpu_cli();
                sched_sleep_cli((u64)&tty_wait_token);
                cpu_sti();
                sched_check_signals();
                continue;
            }
            ((volatile char *)ubuf)[done] = (char)c;
            done++;
            if (tty_cc[TTY_VMIN] <= 1) {
                break;
            }
        }
        return done;
    }

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
