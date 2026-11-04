/* fakeos userland - cal: print a calendar (thin POSIX/GNU subset).
 * Forms:
 *   cal MONTH YEAR
 *   cal YEAR
 * No bare `cal`: CLOCK_REALTIME is boot-relative (no RTC), so the current
 * month is not meaningful unless MONTH/YEAR are given. Week starts Sunday.
 * Year form prints twelve months stacked (compact argv; no wall clock).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);

static const char *mon_names[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

/* Sakamoto offsets; indexed by month-1. */
static const int sak_t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

static int parse_u(const char *s, int *out) {
    int v = 0;
    int any = 0;
    if (s == 0 || s[0] == 0) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (*s - '0');
        s++;
        any = 1;
    }
    if (!any) return 0;
    *out = v;
    return 1;
}

/* 0 = Sunday .. 6 = Saturday. */
static int dow(int y, int m, int d) {
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + sak_t[m - 1] + d) % 7;
}

static int leap(int y) {
    if ((y % 4) != 0) return 0;
    if ((y % 100) != 0) return 1;
    return (y % 400) == 0;
}

static int dim(int y, int m) {
    if (m == 2) return leap(y) ? 29 : 28;
    if (m == 4 || m == 6 || m == 9 || m == 11) return 30;
    return 31;
}

static void usage(void) {
    u_write(2, "usage: cal MONTH YEAR | cal YEAR\n", 34);
}


/* Center "Name YYYY" in a 20-column header (GNU/BSD cal width). */
static void print_header(int month, int year) {
    const char *name = mon_names[month - 1];
    unsigned long nlen = strlen(name);
    char title[32];
    unsigned long n = 0;
    unsigned long i;
    int pad;
    int y;
    char ybuf[12];
    int yi = 0;
    int k;

    for (i = 0; i < nlen && n < 15; i++) {
        title[n++] = name[i];
    }
    title[n++] = ' ';
    y = year;
    if (y == 0) {
        ybuf[yi++] = '0';
    } else {
        while (y > 0 && yi < 12) {
            ybuf[yi++] = (char)('0' + (y % 10));
            y /= 10;
        }
    }
    for (k = yi - 1; k >= 0; k--) {
        title[n++] = ybuf[k];
    }
    pad = (int)((20 - (int)n) / 2);
    if (pad < 0) pad = 0;
    for (k = 0; k < pad; k++) u_write(1, " ", 1);
    u_write(1, title, n);
    u_write(1, "\n", 1);
    u_write(1, "Su Mo Tu We Th Fr Sa\n", 21);
}

static void print_month(int month, int year) {
    int start = dow(year, month, 1);
    int days = dim(year, month);
    int d;
    int col = 0;
    char line[24];
    int li = 0;

    print_header(month, year);
    while (col < start) {
        line[li++] = ' ';
        line[li++] = ' ';
        if (col < 6) line[li++] = ' ';
        col++;
    }
    for (d = 1; d <= days; d++) {
        if (d < 10) {
            line[li++] = ' ';
            line[li++] = (char)('0' + d);
        } else {
            line[li++] = (char)('0' + (d / 10));
            line[li++] = (char)('0' + (d % 10));
        }
        if (col == 6) {
            line[li++] = '\n';
            u_write(1, line, (unsigned long)li);
            li = 0;
            col = 0;
        } else {
            line[li++] = ' ';
            col++;
        }
    }
    if (li > 0) {
        if (line[li - 1] == ' ') li--;
        line[li++] = '\n';
        u_write(1, line, (unsigned long)li);
    }
}

int main(int argc, char **argv) {
    int month;
    int year;
    int m;

    if (argc == 2) {
        if (!parse_u(argv[1], &year) || year < 1 || year > 9999) {
            usage();
            return 1;
        }
        for (m = 1; m <= 12; m++) {
            if (m > 1) u_write(1, "\n", 1);
            print_month(m, year);
        }
        return 0;
    }
    if (argc == 3) {
        if (!parse_u(argv[1], &month) || month < 1 || month > 12) {
            usage();
            return 1;
        }
        if (!parse_u(argv[2], &year) || year < 1 || year > 9999) {
            usage();
            return 1;
        }
        print_month(month, year);
        return 0;
    }
    usage();
    return 1;
}
