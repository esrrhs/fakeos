package drivers;
import arch;
import types;

// Screen dimensions
static int VGA_WIDTH = 80;
static int VGA_HEIGHT = 25;

// Higher-Half virtual address for VGA text mode buffer (physical 0xB8000)
static volatile types.uint16_t *VGA_BUFFER = (volatile types.uint16_t *)0xFFFFFFFF800B8000;

// Current cursor coordinates and active color attribute
static int cursor_col = 0;
static int cursor_row = 0;
static types.uint8_t current_color = 0x0F; // White on black

static void update_hardware_cursor(void) {
    types.uint16_t pos = (types.uint16_t)(cursor_row * VGA_WIDTH + cursor_col);

    // Tell CRT controller we want to set high cursor byte (0x0E)
    arch.outb(0x03D4, 0x0E);
    arch.outb(0x03D5, (types.uint8_t)((pos >> 8) & 0xFF));

    // Tell CRT controller we want to set low cursor byte (0x0F)
    arch.outb(0x03D4, 0x0F);
    arch.outb(0x03D5, (types.uint8_t)(pos & 0xFF));
}

void vga_set_color(types.uint8_t fg, types.uint8_t bg) {
    current_color = (types.uint8_t)((bg << 4) | (fg & 0x0F));
}

void vga_clear(void) {
    types.uint16_t blank = (types.uint16_t)(' ' | (current_color << 8));
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = blank;
    }
    cursor_col = 0;
    cursor_row = 0;
    update_hardware_cursor();
}

void vga_init(void) {
    current_color = 0x0F; // Light gray / white on black
    vga_clear();
}

static void vga_scroll(void) {
    types.uint16_t blank = (types.uint16_t)(' ' | (current_color << 8));

    // Move rows 1..24 up by one line
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            VGA_BUFFER[y * VGA_WIDTH + x] = VGA_BUFFER[(y + 1) * VGA_WIDTH + x];
        }
    }

    // Clear the bottom row
    for (int x = 0; x < VGA_WIDTH; x++) {
        VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = blank;
    }

    cursor_row = VGA_HEIGHT - 1;
}

void vga_putc(char c) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        }
        VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] =
            (types.uint16_t)(' ' | (current_color << 8));
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\t') {
        cursor_col = (cursor_col + 4) & ~3;
        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }
    } else if (c >= ' ') {
        types.uint16_t entry = (types.uint16_t)(((types.uint8_t)c) | (current_color << 8));
        VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] = entry;
        cursor_col++;
        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }
    }

    if (cursor_row >= VGA_HEIGHT) {
        vga_scroll();
    }

    update_hardware_cursor();
}

void vga_puts(const char *s) {
    while (*s) {
        vga_putc(*s);
        s++;
    }
}

/* Minimal CSI parser so userspace clear/colors reach the text framebuffer.
 * Recognizes ESC [ ... <cmd> with up to 2 numeric params. */
enum {
    ANSI_NORMAL = 0,
    ANSI_ESC = 1,
    ANSI_CSI = 2
};

static int ansi_state = ANSI_NORMAL;
static int ansi_n[2];
static int ansi_ni;
static int ansi_val;
static int ansi_has;

static void ansi_reset_params(void) {
    ansi_n[0] = 0;
    ansi_n[1] = 0;
    ansi_ni = 0;
    ansi_val = 0;
    ansi_has = 0;
}

static void ansi_apply_sgr(int p) {
    if (p == 0) {
        current_color = 0x0F;
    } else if (p >= 30 && p <= 37) {
        current_color = (types.uint8_t)((current_color & 0xF0) | (p - 30));
    } else if (p >= 40 && p <= 47) {
        current_color = (types.uint8_t)((current_color & 0x0F) | ((p - 40) << 4));
    } else if (p >= 90 && p <= 97) {
        current_color = (types.uint8_t)((current_color & 0xF0) | (8 + (p - 90)));
    }
}

static void ansi_exec(char cmd) {
    int a = ansi_has || ansi_ni > 0 ? ansi_n[0] : 0;
    int b = ansi_ni > 0 ? ansi_n[1] : 0;
    if (cmd == 'J') {
        if (a == 2 || a == 0) {
            vga_clear();
        }
    } else if (cmd == 'H' || cmd == 'f') {
        int row = a > 0 ? a - 1 : 0;
        int col = b > 0 ? b - 1 : 0;
        if (row < 0) row = 0;
        if (col < 0) col = 0;
        if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
        if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
        cursor_row = row;
        cursor_col = col;
        update_hardware_cursor();
    } else if (cmd == 'K') {
        types.uint16_t blank = (types.uint16_t)(' ' | (current_color << 8));
        int x;
        int start = (a == 1) ? 0 : cursor_col;
        int end = (a == 1) ? cursor_col + 1 : VGA_WIDTH;
        if (a == 2) {
            start = 0;
            end = VGA_WIDTH;
        }
        for (x = start; x < end; x++) {
            VGA_BUFFER[cursor_row * VGA_WIDTH + x] = blank;
        }
    } else if (cmd == 'm') {
        if (!ansi_has && ansi_ni == 0) {
            ansi_apply_sgr(0);
        } else {
            ansi_apply_sgr(ansi_n[0]);
            if (ansi_ni > 0) {
                ansi_apply_sgr(ansi_n[1]);
            }
        }
    } else if (cmd == 'A') {
        int n = a > 0 ? a : 1;
        cursor_row -= n;
        if (cursor_row < 0) cursor_row = 0;
        update_hardware_cursor();
    } else if (cmd == 'B') {
        int n = a > 0 ? a : 1;
        cursor_row += n;
        if (cursor_row >= VGA_HEIGHT) cursor_row = VGA_HEIGHT - 1;
        update_hardware_cursor();
    } else if (cmd == 'C') {
        int n = a > 0 ? a : 1;
        cursor_col += n;
        if (cursor_col >= VGA_WIDTH) cursor_col = VGA_WIDTH - 1;
        update_hardware_cursor();
    } else if (cmd == 'D') {
        int n = a > 0 ? a : 1;
        cursor_col -= n;
        if (cursor_col < 0) cursor_col = 0;
        update_hardware_cursor();
    }
}

void vga_ansi_putc(char c) {
    if (ansi_state == ANSI_NORMAL) {
        if (c == 0x1B) {
            ansi_state = ANSI_ESC;
            return;
        }
        vga_putc(c);
        return;
    }
    if (ansi_state == ANSI_ESC) {
        if (c == '[') {
            ansi_state = ANSI_CSI;
            ansi_reset_params();
            return;
        }
        ansi_state = ANSI_NORMAL;
        vga_putc(c);
        return;
    }
    /* CSI */
    if (c >= '0' && c <= '9') {
        ansi_val = ansi_val * 10 + (c - '0');
        ansi_has = 1;
        return;
    }
    if (c == ';') {
        if (ansi_ni < 2) {
            ansi_n[ansi_ni] = ansi_has ? ansi_val : 0;
            ansi_ni++;
        }
        ansi_val = 0;
        ansi_has = 0;
        return;
    }
    if (ansi_ni < 2) {
        ansi_n[ansi_ni] = ansi_has ? ansi_val : 0;
    }
    ansi_exec(c);
    ansi_state = ANSI_NORMAL;
}
