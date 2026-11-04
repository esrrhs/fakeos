/* fakeos userland - date: print clock_gettime(REALTIME) (thin).
 *   date
 *   date +FORMAT
 * No RTC — REALTIME is boot-relative (same as MONOTONIC). Bare `date` and
 * `+%s` print whole seconds. +FORMAT supports %s %N %Y %m %d %H %M %S %y
 * %F (%Y-%m-%d) %T (%H:%M:%S) %R (%H:%M) %% (calendar fields treat
 * boot-relative seconds as Unix epoch). External form for xargs/scripts.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_clock_gettime(long clockid, void *tp);
extern unsigned long strlen(const char *s);
extern void printf(const char *fmt, ...);

enum { CLOCK_REALTIME = 0 };

static void emit_ch(char c) {
    u_write(1, &c, 1);
}

static void emit_str(const char *s) {
    unsigned long n = strlen(s);
    if (n > 0) u_write(1, s, n);
}

static void emit_udec(unsigned long v) {
    char tmp[24];
    int i = 0;
    int k;
    if (v == 0) {
        emit_ch('0');
        return;
    }
    while (v > 0 && i < 24) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (k = i - 1; k >= 0; k--) emit_ch(tmp[k]);
}

static void emit_pad(unsigned long v, int width) {
    char tmp[24];
    int i = 0;
    int k;
    unsigned long x = v;
    if (x == 0) {
        tmp[i++] = '0';
    } else {
        while (x > 0 && i < 24) {
            tmp[i++] = (char)('0' + (x % 10));
            x /= 10;
        }
    }
    while (i < width && i < 24) tmp[i++] = '0';
    for (k = i - 1; k >= 0; k--) emit_ch(tmp[k]);
}

/* Civil Y-M-D from days since 1970-01-01 (Howard Hinnant / C++20 chrono). */
static void ymd_from_days(long z, int *year, int *month, int *day) {
    long era;
    unsigned long doe;
    unsigned long yoe;
    long y;
    unsigned long doy;
    unsigned long mp;
    unsigned long d;
    unsigned long m;

    z += 719468L;
    if (z >= 0) {
        era = z / 146097L;
    } else {
        era = (z - 146096L) / 146097L;
    }
    doe = (unsigned long)(z - era * 146097L);
    yoe = (doe - doe / 1460UL + doe / 36524UL - doe / 146096UL) / 365UL;
    y = (long)yoe + era * 400L;
    doy = doe - (365UL * yoe + yoe / 4UL - yoe / 100UL);
    mp = (5UL * doy + 2UL) / 153UL;
    d = doy - (153UL * mp + 2UL) / 5UL + 1UL;
    if (mp < 10UL) {
        m = mp + 3UL;
    } else {
        m = mp - 9UL;
    }
    y += (m <= 2UL) ? 1L : 0L;
    *year = (int)y;
    *month = (int)m;
    *day = (int)d;
}

static void emit_format(const char *fmt, long sec, long nsec) {
    int year = 1970;
    int month = 1;
    int day = 1;
    long days;
    long tod;
    int hour;
    int min;
    int ssec;
    const char *p = fmt;

    if (sec < 0) sec = 0;
    if (nsec < 0) nsec = 0;
    if (nsec > 999999999L) nsec = 999999999L;

    days = sec / 86400L;
    tod = sec % 86400L;
    hour = (int)(tod / 3600L);
    min = (int)((tod % 3600L) / 60L);
    ssec = (int)(tod % 60L);
    ymd_from_days(days, &year, &month, &day);

    while (*p) {
        if (*p != '%') {
            emit_ch(*p);
            p++;
            continue;
        }
        p++;
        if (*p == 0) {
            emit_ch('%');
            break;
        }
        if (*p == '%') {
            emit_ch('%');
        } else if (*p == 's') {
            emit_udec((unsigned long)sec);
        } else if (*p == 'N') {
            emit_pad((unsigned long)nsec, 9);
        } else if (*p == 'Y') {
            emit_pad((unsigned long)year, 4);
        } else if (*p == 'y') {
            emit_pad((unsigned long)(year % 100), 2);
        } else if (*p == 'm') {
            emit_pad((unsigned long)month, 2);
        } else if (*p == 'd') {
            emit_pad((unsigned long)day, 2);
        } else if (*p == 'H') {
            emit_pad((unsigned long)hour, 2);
        } else if (*p == 'M') {
            emit_pad((unsigned long)min, 2);
        } else if (*p == 'S') {
            emit_pad((unsigned long)ssec, 2);
        } else if (*p == 'F') {
            /* %Y-%m-%d */
            emit_pad((unsigned long)year, 4);
            emit_ch('-');
            emit_pad((unsigned long)month, 2);
            emit_ch('-');
            emit_pad((unsigned long)day, 2);
        } else if (*p == 'T') {
            /* %H:%M:%S */
            emit_pad((unsigned long)hour, 2);
            emit_ch(':');
            emit_pad((unsigned long)min, 2);
            emit_ch(':');
            emit_pad((unsigned long)ssec, 2);
        } else if (*p == 'R') {
            /* %H:%M */
            emit_pad((unsigned long)hour, 2);
            emit_ch(':');
            emit_pad((unsigned long)min, 2);
        } else {
            /* Unknown conversion: echo literally like a thin subset. */
            emit_ch('%');
            emit_ch(*p);
        }
        p++;
    }
    emit_ch('\n');
}

int main(int argc, char **argv) {
    long ts[2];
    const char *fmt;

    if (argc > 2) {
        u_write(2, "usage: date [+FORMAT]\n", 23);
        return 1;
    }
    if (argc == 2) {
        if (argv[1][0] != '+') {
            u_write(2, "usage: date [+FORMAT]\n", 23);
            return 1;
        }
        fmt = argv[1] + 1;
    } else {
        fmt = "%s";
    }
    if (u_clock_gettime(CLOCK_REALTIME, ts) != 0) {
        u_write(2, "date: failed\n", 13);
        return 1;
    }
    emit_format(fmt, ts[0], ts[1]);
    return 0;
}
