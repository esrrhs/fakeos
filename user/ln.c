/* fakeos userland - ln: create hard or symbolic links (thin; mirrors shell).
 *   ln [-s] TARGET LINKPATH
 * -s  create a symlink via symlink(2); otherwise hard link via link(2).
 * External form for xargs/scripts that exec ln without the shell builtin.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_link(const char *oldpath, const char *newpath);
extern long u_symlink(const char *target, const char *linkpath);
extern int strcmp(const char *a, const char *b);

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "-s") == 0) {
        if (argc != 4) {
            u_write(2, "usage: ln [-s] TARGET LINKPATH\n", 31);
            return 1;
        }
        if (u_symlink(argv[2], argv[3]) != 0) {
            u_write(2, "ln: symlink failed\n", 19);
            return 1;
        }
        return 0;
    }
    if (argc != 3) {
        u_write(2, "usage: ln [-s] TARGET LINKPATH\n", 31);
        return 1;
    }
    if (u_link(argv[1], argv[2]) != 0) {
        u_write(2, "ln: link failed\n", 16);
        return 1;
    }
    return 0;
}
