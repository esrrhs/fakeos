/* fakeos userland - id: print uid/euid/gid/egid (+ supplementary groups).
 * Thin shell-mirror of the `id` builtin (numeric; no name lookup).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_getuid(void);
extern long u_geteuid(void);
extern long u_getgid(void);
extern long u_getegid(void);
extern long u_getgroups(long size, unsigned int *list);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    unsigned int gids[8];
    long ng;
    long gi;

    if (argc > 1) {
        u_write(2, "usage: id\n", 10);
        return 1;
    }
    printf("uid=%d euid=%d gid=%d egid=%d",
           (int)u_getuid(), (int)u_geteuid(),
           (int)u_getgid(), (int)u_getegid());
    ng = u_getgroups(8, gids);
    if (ng < 0) {
        ng = 0;
    }
    if (ng > 0) {
        printf(" groups=");
        for (gi = 0; gi < ng; gi++) {
            if (gi) printf(",");
            printf("%d", (int)gids[gi]);
        }
    }
    printf("\n");
    return 0;
}
