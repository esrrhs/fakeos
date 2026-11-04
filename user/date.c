/* fakeos userland - date: print clock_gettime(REALTIME) seconds (thin).
 *   date
 *   date +%s
 * No RTC yet — REALTIME is boot-relative like MONOTONIC (same as kernel
 * clock_gettime). External form for xargs/scripts.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_clock_gettime(long clockid, void *tp);
extern int strcmp(const char *a, const char *b);
extern void printf(const char *fmt, ...);

enum { CLOCK_REALTIME = 0 };

int main(int argc, char **argv) {
    long ts[2];

    if (argc > 2) {
        u_write(2, "usage: date [+%s]\n", 18);
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "+%s") != 0) {
        u_write(2, "usage: date [+%s]\n", 18);
        return 1;
    }
    if (u_clock_gettime(CLOCK_REALTIME, ts) != 0) {
        u_write(2, "date: failed\n", 13);
        return 1;
    }
    printf("%u\n", (unsigned int)ts[0]);
    return 0;
}
