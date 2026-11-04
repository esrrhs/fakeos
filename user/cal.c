/* fakeos userland - cal: print a calendar (thin POSIX/GNU subset).
 * Forms:
 *   cal MONTH YEAR
 *   cal YEAR
 *   cal -y YEAR
 * No bare `cal` / bare `cal -y`: CLOCK_REALTIME is boot-relative (no RTC),
 * so the current month/year is not meaningful unless YEAR is given.
 * Week starts Sunday. Year form prints twelve months stacked.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);

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
    u_write(2, "usage: cal [-y] MONTH YEAR | cal [-y] YEAR\n", 45);
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

static void print_year(int year) {
    int m;
    for (m = 1; m <= 12; m++) {
        if (m > 1) u_write(1, "\n", 1);
        print_month(m, year);
    }
}

int main(int argc, char **argv) {
    int month;
    int year;
    int ai = 1;
    int year_mode = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *a = argv[ai];
        if (strcmp(a, "--") == 0) {
            ai++;
            break;
        }
        if (strcmp(a, "-y") == 0) {
            year_mode = 1;
            ai++;
            continue;
        }
        usage();
        return 1;
    }

    if (year_mode) {
        /* cal -y YEAR  (bare cal -y needs RTC — refuse) */
        if (ai + 1 != argc) {
            usage();
            return 1;
        }
        if (!parse_u(argv[ai], &year) || year < 1 || year > 9999) {
            usage();
            return 1;
        }
        print_year(year);
        return 0;
    }

    if (argc - ai == 1) {
        if (!parse_u(argv[ai], &year) || year < 1 || year > 9999) {
            usage();
            return 1;
        }
        print_year(year);
        return 0;
    }
    if (argc - ai == 2) {
        if (!parse_u(argv[ai], &month) || month < 1 || month > 12) {
            usage();
            return 1;
        }
        if (!parse_u(argv[ai + 1], &year) || year < 1 || year > 9999) {
            usage();
            return 1;
        }
        print_month(month, year);
        return 0;
    }
    usage();
    return 1;
}
