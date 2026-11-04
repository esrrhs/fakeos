/* fakeos userland - sleep: pause for SECONDS (thin; mirrors shell builtin).
 *   sleep SECONDS
 * SECONDS is a non-negative decimal integer. Uses nanosleep(2).
 * External form for xargs/scripts that exec sleep without the shell builtin.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_nanosleep(const void *req, void *rem);

static int parse_secs(const char *s, long *out) {
    long v = 0;
    int i = 0;
    if (s == 0 || s[0] == 0) return 0;
    if (s[0] == '+') {
        i = 1;
        if (s[i] == 0) return 0;
    }
    if (s[i] < '0' || s[i] > '9') return 0;
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    *out = v;
    return 1;
}

int main(int argc, char **argv) {
    long sec;
    long ts[2];

    if (argc != 2) {
        u_write(2, "usage: sleep SECONDS\n", 21);
        return 1;
    }
    if (!parse_secs(argv[1], &sec)) {
        u_write(2, "sleep: invalid time interval\n", 29);
        return 1;
    }
    ts[0] = sec;
    ts[1] = 0;
    if (u_nanosleep(ts, 0) < 0) {
        u_write(2, "sleep: interrupted\n", 19);
        return 1;
    }
    return 0;
}
