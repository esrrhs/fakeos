package drivers;
import arch;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);
extern void tty_push_byte(char c);

/* ---------------------------------------------------------------------------
 * PS/2 keyboard (i8042) - IRQ1, scancode set 1, US layout.
 *
 * Routes through the I/O APIC (ISA IRQ1 -> IDT vector 33). Printable keys
 * and Enter/Backspace are translated to ASCII and pushed into the TTY
 * input ring so the same canonical line discipline serves serial and kbd.
 * ------------------------------------------------------------------------- */

enum {
    KBD_DATA = 0x60,
    KBD_STAT = 0x64,
    KBD_STAT_OBF = 0x01,             /* output buffer full */
    KBD_STAT_IBF = 0x02              /* input buffer full */
};

static u32 kbd_shift;
static u32 kbd_caps;
static u32 kbd_ok;
static u32 kbd_e0;                   /* next scancode is E0-prefixed */
static volatile u64 kbd_irq_count;

/* Set-1 make codes -> ASCII (unshifted). 0 = ignore / special. */
static const char keymap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char keymap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void kbd_wait_ibf_clear(void) {
    u32 spins = 0;
    while ((arch.inb(KBD_STAT) & KBD_STAT_IBF) != 0) {
        spins++;
        if (spins > 100000) {
            return;
        }
    }
}

static void kbd_write_cmd(u8 cmd) {
    kbd_wait_ibf_clear();
    arch.outb(KBD_STAT, cmd);
}

static void kbd_write_data(u8 data) {
    kbd_wait_ibf_clear();
    arch.outb(KBD_DATA, data);
}

static int kbd_read_data(void) {
    u32 spins = 0;
    while ((arch.inb(KBD_STAT) & KBD_STAT_OBF) == 0) {
        spins++;
        if (spins > 100000) {
            return -1;
        }
    }
    return (int)(arch.inb(KBD_DATA) & 0xFF);
}

static char apply_case(char c) {
    u32 upper = kbd_shift;
    if (c >= 'a' && c <= 'z') {
        if (kbd_caps) {
            upper = !upper;
        }
        if (upper) {
            return (char)(c - 'a' + 'A');
        }
        return c;
    }
    if (kbd_shift && c != 0) {
        /* Non-letters already taken from keymap_shift. */
        return c;
    }
    return c;
}

void kbd_irq(void) {
    u8 sc;
    char ch;
    if ((arch.inb(KBD_STAT) & KBD_STAT_OBF) == 0) {
        return;
    }
    sc = arch.inb(KBD_DATA);
    kbd_irq_count++;

    if (sc == 0xE1) {
        return; /* pause/break sequence ignored */
    }
    if (sc == 0xE0) {
        kbd_e0 = 1;
        return;
    }

    if (sc & 0x80) {
        /* Break code */
        sc = (u8)(sc & 0x7F);
        if (kbd_e0) {
            kbd_e0 = 0;
            return;
        }
        if (sc == 0x2A || sc == 0x36) {
            kbd_shift = 0;
        }
        return;
    }

    if (kbd_e0) {
        kbd_e0 = 0;
        /* Arrow keys: emit CSI A/B/C/D for TTY history / cursor. */
        if (sc == 0x48 || sc == 0x50 || sc == 0x4B || sc == 0x4D) {
            tty_push_byte(0x1B);
            tty_push_byte('[');
            if (sc == 0x48) tty_push_byte('A');      /* Up */
            else if (sc == 0x50) tty_push_byte('B'); /* Down */
            else if (sc == 0x4D) tty_push_byte('C'); /* Right */
            else tty_push_byte('D');                 /* Left */
        }
        return;
    }

    if (sc == 0x2A || sc == 0x36) {
        kbd_shift = 1;
        return;
    }
    if (sc == 0x3A) {
        kbd_caps = !kbd_caps;
        return;
    }
    if (sc >= 128) {
        return;
    }

    if (kbd_shift) {
        ch = keymap_shift[sc];
    } else {
        ch = keymap[sc];
        ch = apply_case(ch);
    }
    if (ch == 0) {
        return;
    }
    if (ch == '\b') {
        ch = 0x7F;                   /* match serial DEL so TTY erases */
    }
    tty_push_byte(ch);
}

u64 kbd_irq_count_total(void) {
    return kbd_irq_count;
}

u32 kbd_present(void) {
    return kbd_ok;
}

void kbd_init(void) {
    int r;
    kbd_shift = 0;
    kbd_caps = 0;
    kbd_e0 = 0;
    kbd_irq_count = 0;
    kbd_ok = 0;

    /* Disable ports while we reconfigure. */
    kbd_write_cmd(0xAD);             /* disable keyboard */
    kbd_write_cmd(0xA7);             /* disable mouse */
    /* Drain output buffer (bounded - a stuck OBF must not hang boot). */
    {
        u32 drain = 0;
        while ((arch.inb(KBD_STAT) & KBD_STAT_OBF) != 0) {
            (void)arch.inb(KBD_DATA);
            drain++;
            if (drain > 256) {
                break;
            }
        }
    }

    /* Read controller config, enable IRQ1 (bit0), disable mouse IRQ (bit1),
     * translation on (bit6) for set-1 scancodes. */
    kbd_write_cmd(0x20);
    r = kbd_read_data();
    if (r < 0) {
        kprintf("[KBD] controller config read failed\n");
        return;
    }
    {
        u8 cfg = (u8)r;
        cfg = (u8)((cfg | 0x01) & (u8)~0x22); /* IRQ1 on; mouse IRQ/clock off */
        cfg = (u8)(cfg | 0x40);               /* translation */
        kbd_write_cmd(0x60);
        kbd_write_data(cfg);
    }

    /* Enable keyboard port. */
    kbd_write_cmd(0xAE);

    /* Reset device; expect ACK (0xFA) then BAT 0xAA. Soft-fail if absent. */
    kbd_write_data(0xFF);
    r = kbd_read_data();
    if (r != 0xFA) {
        kprintf("[KBD] no ACK to reset (got 0x%x) - continuing anyway\n", (u64)(u32)r);
    } else {
        r = kbd_read_data();
        if (r != 0xAA) {
            kprintf("[KBD] BAT not 0xAA (got 0x%x)\n", (u64)(u32)r);
        }
    }

    kbd_ok = 1;
    kprintf("[KBD] PS/2 keyboard armed (IRQ1)\n");
}
