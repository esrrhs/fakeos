/* In-OS compile probe: this file is compiled BY fakecc INSIDE fakeos (with
 * fakecc's own freestanding runtime resolved from /src/runtime via the
 * FAKECC_PKG=/src environment, not the fakeos libc). Its fixed marker proves
 * the compiler -> built-in linker -> execve chain works end to end. */
package main;

import runtime;

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    runtime.printf("pong-from-fakeos-compiler\n");
    return 0;
}
