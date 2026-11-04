/* fakeos userland - pwd: print working directory (thin; mirrors shell builtin).
 *   pwd [-L|-P]
 * -L logical (default), -P physical. Kernel chdir stores a physical cwd, so
 * both modes use getcwd(2) today. Flags accepted for POSIX/script compat.
 * External form for xargs/scripts that exec pwd.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_getcwd(char *buf, unsigned long len);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *a, const char *b);

static void usage(void) {
    u_write(2, "usage: pwd [-L|-P]\n", 19);
}

int main(int argc, char **argv) {
    char buf[128];
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "-P") == 0) {
            continue;
        }
        if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) {
                usage();
                return 1;
            }
            break;
        }
        usage();
        return 1;
    }

    if (u_getcwd(buf, sizeof(buf)) == -1) {
        u_write(2, "pwd: failed\n", 12);
        return 1;
    }
    u_write(1, buf, strlen(buf));
    u_write(1, "\n", 1);
    return 0;
}
