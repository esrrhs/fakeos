/* fakeos userland - echo: print ARGS with -n/-e/-E (mirror shell builtin).
 *   echo [-neE] [ARGS...]
 * -n  no trailing newline
 * -e  enable backslash escapes (\n \t \r \a \b \f \v \\ \c octal)
 * -E  literal backslashes (default)
 * Sticky clusters (-ne) accepted; first non-flag operand ends option parse.
 * \c stops further output and implies -n.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern unsigned long strlen(const char *s);

int main(int argc, char **argv) {
    int nonew = 0;
    int do_esc = 0;
    int ai = 1;
    int first = 1;
    int stop = 0;

    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        const char *p = argv[ai] + 1;
        int ok = 1;
        int j;
        for (j = 0; p[j] != 0; j++) {
            if (p[j] == 'n') nonew = 1;
            else if (p[j] == 'e') do_esc = 1;
            else if (p[j] == 'E') do_esc = 0;
            else {
                ok = 0;
                break;
            }
        }
        if (!ok) break;
        ai++;
    }

    for (; ai < argc && !stop; ai++) {
        if (!first) u_write(1, " ", 1);
        first = 0;
        if (!do_esc) {
            u_write(1, argv[ai], strlen(argv[ai]));
        } else {
            const char *s = argv[ai];
            char outb[256];
            int on = 0;
            while (*s != 0 && on + 1 < 256) {
                if (*s != '\\') {
                    outb[on++] = *s++;
                    continue;
                }
                s++;
                if (*s == 0) {
                    outb[on++] = '\\';
                    break;
                }
                if (*s == 'c') {
                    stop = 1;
                    nonew = 1;
                    s++;
                    break;
                } else if (*s == 'n') { outb[on++] = '\n'; s++; }
                else if (*s == 't') { outb[on++] = '\t'; s++; }
                else if (*s == 'r') { outb[on++] = '\r'; s++; }
                else if (*s == 'a') { outb[on++] = '\a'; s++; }
                else if (*s == 'b') { outb[on++] = '\b'; s++; }
                else if (*s == 'f') { outb[on++] = '\f'; s++; }
                else if (*s == 'v') { outb[on++] = '\v'; s++; }
                else if (*s == '\\') { outb[on++] = '\\'; s++; }
                else if (*s >= '0' && *s <= '7') {
                    int v = 0;
                    int k = 0;
                    while (k < 3 && s[k] >= '0' && s[k] <= '7') {
                        v = (v << 3) + (s[k] - '0');
                        k++;
                    }
                    outb[on++] = (char)v;
                    s += k;
                } else {
                    outb[on++] = *s++;
                }
            }
            if (on > 0) u_write(1, outb, (unsigned long)on);
        }
    }
    if (!nonew) u_write(1, "\n", 1);
    return 0;
}
