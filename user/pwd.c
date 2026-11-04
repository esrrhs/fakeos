/* fakeos userland - pwd: print working directory (thin; mirrors shell builtin).
 *   pwd
 * Uses getcwd(2). External form for xargs/scripts that exec pwd.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_getcwd(char *buf, unsigned long len);
extern unsigned long strlen(const char *s);

int main(int argc, char **argv) {
    char buf[128];
    (void)argv;
    if (argc != 1) {
        u_write(2, "usage: pwd\n", 11);
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
