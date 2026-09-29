package kernel;
import drivers;
import types;

static void print_char(char c) {
    drivers.uart_putc(c);
    drivers.vga_putc(c);
}

static void print_string(const char *s) {
    if (!s) {
        s = "(null)";
    }
    drivers.uart_puts(s);
    drivers.vga_puts(s);
}

static void print_udec(types.uint64_t val) {
    char buf[32];
    int i = 0;

    if (val == 0) {
        print_char('0');
        return;
    }

    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }

    while (i > 0) {
        print_char(buf[--i]);
    }
}

static void print_dec(types.int64_t val) {
    if (val < 0) {
        print_char('-');
        print_udec((types.uint64_t)(-val));
    } else {
        print_udec((types.uint64_t)val);
    }
}

static void print_hex(types.uint64_t val, int uppercase) {
    char buf[32];
    int i = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        print_char('0');
        return;
    }

    while (val > 0) {
        buf[i++] = digits[val & 0x0F];
        val >>= 4;
    }

    while (i > 0) {
        print_char(buf[--i]);
    }
}

static void print_ptr(types.uint64_t val) {
    const char *digits = "0123456789abcdef";
    print_string("0x");

    // Output all 16 nibbles for 64-bit canonical addresses
    for (int i = 60; i >= 0; i -= 4) {
        print_char(digits[(val >> i) & 0x0F]);
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == '\0') break;

            if (*fmt == 'c') {
                char c = (char)va_arg(ap, int);
                print_char(c);
            } else if (*fmt == 's') {
                const char *s = va_arg(ap, const char *);
                print_string(s);
            } else if (*fmt == 'd' || *fmt == 'i') {
                types.int64_t val = (types.int64_t)va_arg(ap, long long);
                print_dec(val);
            } else if (*fmt == 'u') {
                types.uint64_t val = (types.uint64_t)va_arg(ap, unsigned long long);
                print_udec(val);
            } else if (*fmt == 'x') {
                types.uint64_t val = (types.uint64_t)va_arg(ap, unsigned long long);
                print_hex(val, 0);
            } else if (*fmt == 'X') {
                types.uint64_t val = (types.uint64_t)va_arg(ap, unsigned long long);
                print_hex(val, 1);
            } else if (*fmt == 'p') {
                types.uint64_t val = (types.uint64_t)va_arg(ap, void *);
                print_ptr(val);
            } else if (*fmt == '%') {
                print_char('%');
            } else {
                print_char('%');
                print_char(*fmt);
            }
        } else {
            print_char(*fmt);
        }
        fmt++;
    }

    va_end(ap);
}
