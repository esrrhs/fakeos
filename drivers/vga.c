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
