/* fakeos userland - hello: execve/argv/exit-code probe. Exits 42 so the
 * shell's $? / wait4 status path is observable. */
package user;

extern long u_getpid(void);
extern void printf(const char *fmt, ...);

int main(int argc, char **argv) {
    printf("hello from /bin/hello (argv[0]=%s, pid=%d)\n",
           argc > 0 ? argv[0] : "?", (int)u_getpid());
    return 42;
}
