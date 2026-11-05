/* fakeos userland - sleep: pause for DURATION (thin; mirrors shell builtin).
 *   sleep NUMBER[ NUMBER...]
 * Each NUMBER is a non-negative decimal, optional fractional part (GNU-ish:
 * 1, 1.5, .5, 0.001). Multiple operands are summed. Uses nanosleep(2).
 * External form for xargs/scripts that exec sleep without the shell builtin.
 * Compact argv for EXEC_MAX_ARGC=8.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_nanosleep(const void *req, void *rem);

/* Parse one duration into *sec / *nsec (0..999999999). Returns 0 on error. */
static int parse_duration(const char *s, long *sec_out, long *nsec_out) {
    long sec = 0;
    unsigned long frac = 0;
    int nd = 0;
    int i = 0;
    int saw_digit = 0;

    if (s == 0 || s[0] == 0) return 0;
    if (s[0] == '+') {
        i = 1;
        if (s[i] == 0) return 0;
    }
    while (s[i] >= '0' && s[i] <= '9') {
        sec = sec * 10 + (long)(s[i] - '0');
        saw_digit = 1;
        i++;
    }
    if (s[i] == '.') {
        i++;
        while (s[i] >= '0' && s[i] <= '9') {
            saw_digit = 1;
            if (nd < 9) {
                frac = frac * 10UL + (unsigned long)(s[i] - '0');
                nd++;
            }
            i++;
        }
    }
    if (!saw_digit || s[i] != 0) return 0;
    while (nd < 9) {
        frac *= 10UL;
        nd++;
    }
    *sec_out = sec;
    *nsec_out = (long)frac;
    return 1;
}

static void usage(void) {
    u_write(2, "usage: sleep NUMBER...\n", 23);
}

int main(int argc, char **argv) {
    long sec = 0;
    long nsec = 0;
    long ts[2];
    int i;

    if (argc < 2) {
        usage();
        return 1;
    }
    for (i = 1; i < argc; i++) {
        long s;
        long ns;
        if (!parse_duration(argv[i], &s, &ns)) {
            u_write(2, "sleep: invalid time interval\n", 29);
            return 1;
        }
        sec += s;
        nsec += ns;
        if (nsec >= 1000000000L) {
            sec += nsec / 1000000000L;
            nsec = nsec % 1000000000L;
        }
    }
    ts[0] = sec;
    ts[1] = nsec;
    if (u_nanosleep(ts, 0) < 0) {
        u_write(2, "sleep: interrupted\n", 19);
        return 1;
    }
    return 0;
}
