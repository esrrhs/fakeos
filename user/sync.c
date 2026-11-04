/* fakeos userland - sync: flush filesystem caches (thin; mirrors shell).
 *   sync
 * Calls sync(2). External form for xargs/scripts.
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);
extern long u_sync(void);

int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) {
        u_write(2, "usage: sync\n", 12);
        return 1;
    }
    u_sync();
    return 0;
}
