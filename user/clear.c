/* fakeos userland - clear: ANSI clear screen (shell-mirror thin).
 * Writes ESC[2J ESC[H to stdout (serial-friendly; same as shell builtin).
 */
package user;

extern long u_write(long fd, const void *buf, unsigned long n);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* ANSI clear for serial; VGA users still see history (no userspace VGA). */
    u_write(1, "\033[2J\033[H", 7);
    return 0;
}
