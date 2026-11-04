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
 * source. Empty checks hold tty_lock under CLI then wait_queue_sleep_unlock
 * so unlock+sleep is atomic under the run lock (same pattern as pipes).
 * Echo goes to UART and VGA. Arrow keys (CSI A/B) recall
 * a 32-entry line history shared by serial and PS/2. Left/Right + Ctrl-A/E/U
 * provide basic readline-style editing. Tab completes the word at the cursor
 * against cwd / absolute paths, /bin (command position), and a small builtin
 * name table; unique directory matches get a trailing '/'. ISIG: Ctrl-C/Ctrl-Z
 * are intercepted
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
extern void select_kick(void);
extern void cpu_cli(void);
extern void cpu_sti(void);

struct kmutex {
    u32 locked;
    u32 owner;
};

struct wait_queue_head {
    u32 pad;
};

extern void kmutex_init(struct kmutex *m);
extern void kmutex_lock(struct kmutex *m);
extern void kmutex_unlock(struct kmutex *m);
extern void wait_queue_init(struct wait_queue_head *wq);
extern void wait_queue_wake(struct wait_queue_head *wq);
extern void wait_queue_sleep_unlock(struct wait_queue_head *wq, struct kmutex *m);

static struct kmutex tty_lock;
static struct wait_queue_head tty_wait;
static u32 tty_locks_inited;
static u32 tty_fg_pgid;         /* foreground process group for ISIG (0=none) */
static u32 tty_got_intr;        /* Ctrl-C interrupted the current read */

static void tty_locks_ensure(void) {
    if (!tty_locks_inited) {
        kmutex_init(&tty_lock);
        wait_queue_init(&tty_wait);
        tty_locks_inited = 1;
    }
}

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
        tty_locks_ensure();
        wait_queue_wake(&tty_wait);
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
        tty_locks_ensure();
        wait_queue_wake(&tty_wait);
        select_kick();
        return;
    }
    if ((tty_iflag & TTY_ICRNL) != 0 && c == '\r') {
        c = '\n';
    }
    tty_queue_byte(c);
    tty_locks_ensure();
    wait_queue_wake(&tty_wait);
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
    /* Hold tty_lock around the wait condition. CLI covers the empty-check to
     * sleep-arm window so an IRQ byte+wake cannot land in between; sleep uses
     * wait_queue_sleep_unlock (atomic unlock+sleep under the run lock). */
    tty_locks_ensure();
    kmutex_lock(&tty_lock);
    cpu_cli();
    for (;;) {
        if (tty_got_intr || sched_has_pending_fatal()) {
            cpu_sti();
            kmutex_unlock(&tty_lock);
            return -2;                  /* interrupted */
        }
        c = tty_in_pop();
        if (c < 0) {
            c = drivers.uart_getc_nonblock();
        }
        if (c >= 0) {
            cpu_sti();
            kmutex_unlock(&tty_lock);
            return c;
        }
        wait_queue_sleep_unlock(&tty_wait, &tty_lock);
        /* Lock re-acquired; IF on. Deliver stop/kill before re-polling. */
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

/* ---- Tab completion (path + /bin + shell builtin names) -----------------
 * On Tab, complete the token under the cursor. Relative words use cwd;
 * absolute words use the directory prefix. Command-position tokens (no
 * slash, at column 0 of the line) also search /bin and a builtin table.
 * Unique directory hits append '/'. No match / no progress -> BEL.
 * Skipped when ECHO is off (hidden password-style input).
 * ---------------------------------------------------------------------- */
enum {
    TAB_MATCH_MAX = 24,
    TAB_NAME_MAX = 28,
    TAB_PATH_MAX = 128,
    TAB_DENT_RECLEN = 48,
    TAB_DENT_BUF = 1920,
    TAB_DT_DIR = 4
};

extern const char *sched_cwd(void);
extern u32 fs_file_open(const char *path, u32 flags);
extern u32 fs_close(u32 fh);
extern u64 fs_getdents_h(u32 fh, char *kbuf, u64 len);
extern u32 fs_file_is_dir(u32 fh);
extern u32 ext2_is_mnt_path(const char *path);
extern u32 ext2_mounted(void);
extern u32 ext2_open(const char *path, u32 flags);
extern u32 ext2_close(u32 fh);
extern u64 ext2_getdents(u32 fh, char *kbuf, u64 len);
extern u32 ext2_file_is_dir(u32 fh);
extern u32 ext2_fh_is(u32 fh);

static char tab_matches[TAB_MATCH_MAX][TAB_NAME_MAX];
static u8 tab_isdir[TAB_MATCH_MAX];
static u32 tab_nmatch;

static void tty_beep(void) {
    drivers.uart_putc((char)0x07);
}

static u32 tab_strlen(const char *s) {
    u32 n = 0;
    if (s == 0) return 0;
    while (s[n] != 0) n++;
    return n;
}

static u32 tab_startswith(const char *name, const char *pfx, u32 pfx_n) {
    u32 i;
    for (i = 0; i < pfx_n; i++) {
        if (name[i] == 0 || name[i] != pfx[i]) return 0;
    }
    return 1;
}

static u32 tab_name_eq(const char *a, const char *b) {
    u32 i = 0;
    for (;;) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
        i++;
        if (i >= TAB_NAME_MAX) return 1;
    }
}

static void tab_add_match(const char *name, u32 is_dir) {
    u32 n;
    u32 i;
    if (tab_nmatch >= TAB_MATCH_MAX) return;
    n = tab_strlen(name);
    if (n == 0 || n >= TAB_NAME_MAX) return;
    for (i = 0; i < tab_nmatch; i++) {
        if (tab_name_eq(tab_matches[i], name)) return;
    }
    for (i = 0; i < n; i++) tab_matches[tab_nmatch][i] = name[i];
    tab_matches[tab_nmatch][n] = 0;
    tab_isdir[tab_nmatch] = (u8)(is_dir ? 1 : 0);
    tab_nmatch++;
}

static u32 tab_open_dir(const char *path) {
    u32 fh;
    if (path == 0 || path[0] == 0) return 0;
    if (ext2_is_mnt_path(path)) {
        if (!ext2_mounted()) return 0;
        fh = ext2_open(path, 0);
        if (fh == 0) return 0;
        if (!ext2_file_is_dir(fh)) {
            ext2_close(fh);
            return 0;
        }
        return fh;
    }
    fh = fs_file_open(path, 0);
    if (fh == 0) return 0;
    if (!fs_file_is_dir(fh)) {
        fs_close(fh);
        return 0;
    }
    return fh;
}

static void tab_close_fh(u32 fh) {
    if (fh == 0) return;
    if (ext2_fh_is(fh)) ext2_close(fh);
    else fs_close(fh);
}

static u64 tab_getdents(u32 fh, char *buf, u64 len) {
    if (ext2_fh_is(fh)) return ext2_getdents(fh, buf, len);
    return fs_getdents_h(fh, buf, len);
}

static char tab_dent_buf[TAB_DENT_BUF];

static void tab_scan_dir(const char *dirpath, const char *pfx, u32 pfx_n) {
    u32 fh;
    u64 n;
    u64 off;
    fh = tab_open_dir(dirpath);
    if (fh == 0) return;
    for (;;) {
        n = tab_getdents(fh, tab_dent_buf, TAB_DENT_BUF);
        if (n == 0 || n == (u64)(-1)) break;
        off = 0;
        while (off + TAB_DENT_RECLEN <= n) {
            char *name = tab_dent_buf + off + 19;
            u8 dtype = (u8)tab_dent_buf[off + 18];
            if (name[0] == '.' && (name[1] == 0
                || (name[1] == '.' && name[2] == 0))) {
                if (!(pfx_n > 0 && pfx[0] == '.')) {
                    off += TAB_DENT_RECLEN;
                    continue;
                }
            }
            if (tab_startswith(name, pfx, pfx_n)) {
                tab_add_match(name, dtype == TAB_DT_DIR);
            }
            off += TAB_DENT_RECLEN;
        }
    }
    tab_close_fh(fh);
}

static void tab_scan_builtins(const char *pfx, u32 pfx_n) {
    static const char *builtins[32] = {
        "echo", "cd", "mkdir", "rmdir", "pwd", "pid", "ppid", "uname",
        "sleep", "ln", "readlink", "realpath", "env", "export", "unset",
        "which", "chmod", "umask", "chown", "id", "sync", "clear",
        "history", "jobs", "fg", "bg", "kill", "exit", "help", 0
    };
    u32 i;
    for (i = 0; builtins[i] != 0; i++) {
        if (tab_startswith(builtins[i], pfx, pfx_n)) {
            tab_add_match(builtins[i], 0);
        }
    }
}

static u32 tab_common_len(void) {
    u32 len = 0;
    u32 i;
    char c;
    if (tab_nmatch == 0) return 0;
    for (;;) {
        c = tab_matches[0][len];
        if (c == 0) return len;
        for (i = 1; i < tab_nmatch; i++) {
            if (tab_matches[i][len] != c) return len;
        }
        len++;
        if (len >= TAB_NAME_MAX) return len;
    }
}

static void tty_insert_char(char ch) {
    u32 i;
    if (tty_editing >= LINE_MAX - 1) return;
    for (i = tty_editing; i > tty_cursor; i--) {
        tty_buf[i] = tty_buf[i - 1];
    }
    tty_buf[tty_cursor] = ch;
    tty_editing++;
    tty_echo_ch(ch);
    tty_cursor++;
    if (tty_cursor < tty_editing) tty_redraw_tail();
}

static void tty_insert_range(const char *s, u32 from, u32 to) {
    u32 k;
    for (k = from; k < to; k++) {
        if (s[k] == 0) break;
        tty_insert_char(s[k]);
    }
}

static u32 tab_copy_cwd(char *out, u32 cap) {
    const char *cwd = sched_cwd();
    u32 i = 0;
    if (cwd == 0) cwd = "/";
    while (cwd[i] != 0 && i + 1 < cap) {
        out[i] = cwd[i];
        i++;
    }
    out[i] = 0;
    return 1;
}

/* dir_end = index after directory prefix in word (0 => use cwd only).
 * Examples: word "/tm" dir_end=1 => "/"; word "/tmp/x" dir_end=5 => "/tmp";
 * word "a/b" dir_end=2 => cwd+"/a". */
static u32 tab_build_dir(char *dirpath, u32 cap, const char *word, u32 dir_end) {
    u32 i;
    u32 j;
    char cwd[TAB_PATH_MAX];
    if (dir_end == 0) {
        return tab_copy_cwd(dirpath, cap);
    }
    if (word[0] == '/') {
        if (dir_end == 1) {
            if (cap < 2) return 0;
            dirpath[0] = '/';
            dirpath[1] = 0;
            return 1;
        }
        if (dir_end >= cap) return 0;
        for (i = 0; i < dir_end; i++) dirpath[i] = word[i];
        dirpath[dir_end] = 0;
        if (dir_end > 1 && dirpath[dir_end - 1] == '/') {
            dirpath[dir_end - 1] = 0;
        }
        return 1;
    }
    if (!tab_copy_cwd(cwd, TAB_PATH_MAX)) return 0;
    i = 0;
    while (cwd[i] != 0 && i + 1 < cap) {
        dirpath[i] = cwd[i];
        i++;
    }
    if (i == 0 || dirpath[i - 1] != '/') {
        if (i + 1 >= cap) return 0;
        dirpath[i++] = '/';
    }
    j = 0;
    while (j < dir_end && word[j] != 0 && i + 1 < cap) {
        dirpath[i++] = word[j++];
    }
    dirpath[i] = 0;
    if (i > 1 && dirpath[i - 1] == '/') dirpath[i - 1] = 0;
    return 1;
}

static void tty_tab_complete(void) {
    u32 ws;
    u32 we;
    u32 i;
    u32 dir_end;
    u32 pfx_n;
    u32 common;
    u32 is_cmd;
    u32 have_slash;
    char word[TAB_PATH_MAX];
    char dirpath[TAB_PATH_MAX];
    char prefix[TAB_NAME_MAX];

    if ((tty_lflag & TTY_ECHO) == 0) return;

    ws = tty_cursor;
    while (ws > 0 && tty_buf[ws - 1] != ' ' && tty_buf[ws - 1] != '\t') {
        ws--;
    }
    we = tty_cursor;
    if (we < ws || (we - ws) >= TAB_PATH_MAX) {
        tty_beep();
        return;
    }
    for (i = 0; i < we - ws; i++) word[i] = tty_buf[ws + i];
    word[we - ws] = 0;

    have_slash = 0;
    dir_end = 0;
    for (i = 0; word[i] != 0; i++) {
        if (word[i] == '/') {
            dir_end = i + 1;
            have_slash = 1;
        }
    }
    if (!have_slash) dir_end = 0;

    i = 0;
    while (word[dir_end + i] != 0 && i + 1 < TAB_NAME_MAX) {
        prefix[i] = word[dir_end + i];
        i++;
    }
    prefix[i] = 0;
    pfx_n = i;

    if (!tab_build_dir(dirpath, TAB_PATH_MAX, word, dir_end)) {
        tty_beep();
        return;
    }

    tab_nmatch = 0;
    is_cmd = (ws == 0 && !have_slash);
    if (is_cmd) {
        tab_scan_builtins(prefix, pfx_n);
        tab_scan_dir("/bin", prefix, pfx_n);
    }
    tab_scan_dir(dirpath, prefix, pfx_n);

    if (tab_nmatch == 0) {
        tty_beep();
        return;
    }
    common = tab_common_len();
    if (common <= pfx_n && tab_nmatch > 1) {
        tty_beep();
        return;
    }
    if (common > pfx_n) {
        tty_insert_range(tab_matches[0], pfx_n, common);
    }
    if (tab_nmatch == 1 && tab_isdir[0] != 0) {
        if (tty_cursor == 0 || tty_buf[tty_cursor - 1] != '/') {
            tty_insert_char('/');
        }
    }
    tty_hist_view = -1;
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
        if (c == '\t') {
            tty_tab_complete();
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            if (tty_editing < LINE_MAX - 1) {
                tty_insert_char((char)c);
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
                tty_locks_ensure();
                kmutex_lock(&tty_lock);
                cpu_cli();
                c = tty_in_pop();
                if (c < 0) {
                    c = drivers.uart_getc_nonblock();
                }
                if (c < 0) {
                    wait_queue_sleep_unlock(&tty_wait, &tty_lock);
                    sched_check_signals();
                    continue;
                }
                cpu_sti();
                kmutex_unlock(&tty_lock);
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

/* Self-test: block on tty wait_queue until one input byte arrives, then drop it.
 * Uses the same kmutex + wait_queue_sleep_unlock path as console read. */
void tty_selftest_wait_byte(void) {
    int c;
    tty_locks_ensure();
    kmutex_lock(&tty_lock);
    cpu_cli();
    for (;;) {
        c = tty_in_pop();
        if (c >= 0) {
            cpu_sti();
            kmutex_unlock(&tty_lock);
            return;
        }
        wait_queue_sleep_unlock(&tty_wait, &tty_lock);
        cpu_cli();
    }
}
