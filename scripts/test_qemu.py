#!/usr/bin/env python3
import os
import selectors
import subprocess
import sys
import time

def main():
    cmd = [
        "qemu-system-x86_64",
        "-kernel", "build/fakeos.elf",
        "-m", "128M",
        "-serial", "stdio",
        "-display", "none",
        "-no-reboot"
    ]

    print("==> Starting QEMU process with serial stdin/stdout...")
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0
    )

    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)

    def read_available(timeout):
        """Drain whatever the VM emits within timeout seconds."""
        chunks = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready = sel.select(0.1)
            if not ready:
                continue
            try:
                data = os.read(proc.stdout.fileno(), 4096)
            except OSError:
                break
            if not data:
                break
            chunks.append(data.decode("utf-8", errors="replace"))
        return "".join(chunks)

    def wait_for(marker, timeout):
        """Read until marker appears; return everything captured."""
        out = ""
        deadline = time.time() + timeout
        while marker not in out and time.time() < deadline:
            ready = sel.select(0.2)
            if not ready:
                continue
            try:
                data = os.read(proc.stdout.fileno(), 4096)
            except OSError:
                break
            if not data:
                break
            out += data.decode("utf-8", errors="replace")
        return out

    # 1. Boot + kernel self-tests + init userspace tests -> first prompt.
    boot = wait_for("fakeos:~$", 8.0)

    # 2. Drive the interactive shell line by line.
    commands = [
        "hello",            # external exec, exit code 42
        "echo $?",          # prints 42
        "echo INPUT123",    # builtin echo
        "cat /etc/motd",    # file read through inherited fd 1
        "mkdir /home",      # builtin mkdir syscall
        "cd /home",         # chdir
        "pwd",              # getcwd -> /home
        "cd ..",
        "ls /bin",          # getdents64, five programs
        "mkdir /home",      # duplicate -> error, shell survives
        "nosuchcmd",        # exec failure -> 127 + not found
        "exit 7"            # pid 1 clean exit, code 7
    ]
    session = ""
    for c in commands:
        try:
            proc.stdin.write((c + "\n").encode("utf-8"))
            proc.stdin.flush()
        except BrokenPipeError:
            break
        session += read_available(0.5)

    # 3. After `exit 7` the monitor releases kmain -> SUCCESS banner.
    tail = read_available(3.0)

    try:
        proc.terminate()
        try:
            proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
    except Exception:
        pass

    stdout = boot + session + tail
    # Console writes translate LF to CRLF; normalize for line-based checks.
    norm = stdout.replace("\r\n", "\n").replace("\r", "\n")

    print("\n--- [Captured Kernel Output Start] ---")
    print(stdout)
    print("--- [Captured Kernel Output End] ---\n")

    required_substrings = [
        # Stage 1-3 retained assertions
        "Welcome to fakeos!",
        "Bootloader detected: Multiboot 1",
        "Interrupt Infrastructure Online",
        "8259A PIC      : all IRQ lines masked",
        "[CAUGHT] vector=0 as expected",
        "[CAUGHT] vector=6 as expected",
        "[CAUGHT] vector=14 as expected",
        "[PASS] Exception #DE",
        "[PASS] Exception #UD",
        "[PASS] Exception #PF",
        "[PASS] APIC Timer Calibration",
        "Multiboot Physical Memory Map",
        "Buddy Physical Page Allocator Online",
        "4-Level Page Table Toolchain Online",
        "Slab Kernel Heap Online",
        "[PASS] Single-page alloc/free",
        "[PASS] Multi-order alloc",
        "[PASS] Buddy merge",
        "[PASS] Stress test",
        "[PASS] Map/translate/unmap",
        "[PASS] Size-class alloc",
        "[PASS] Big alloc path",
        "[PASS] Free-list reuse",
        "[PASS] Demand paging",
        "[PASS] Permissions: read-only VMA",
        "[PASS] COW clone",
        "[PASS] Cooperative threads",
        "[PASS] Timer preemption",
        # Stage 4: VFS + Ramfs kernel self-test
        "[PASS] VFS path resolution",
        "[PASS] Ramfs file create/read/write/seek",
        "[PASS] Ramfs directory listing",
        # rootfs publication
        "[FS] published /bin/init (",
        "[FS] published /bin/sh (",
        "[FS] published /bin/hello (",
        "[FS] published /bin/cat (",
        "[FS] published /bin/ls (",
        "[FS] published /etc/motd (",
        # Ring-3 retained + stage-4 user self-tests
        "[U] hello from ring 3, pid=",
        "[U][PASS] child COW write took effect",
        "[U][PASS] parent COW value intact after child write",
        "[PASS] Ring-3 entry",
        "[PASS] SYSCALL/SYSRET",
        "[PASS] COW fork across user processes",
        "[PASS] Timer preemption of ring-3 threads",
        "[U][PASS] vfs file write/read/seek",
        "[U][PASS] brk heap malloc",
        "[U][PASS] anonymous mmap",
        "[U][PASS] vfs getdents",
        "[U][PASS] execve rejects missing image",
        # interactive shell session
        "fakeos shell - builtins",
        "hello from /bin/hello",
        "INPUT123",
        "Welcome to fakeos - built entirely with the fakecc toolchain.",
        "Try: hello, ls /bin, cat /etc/motd",
        "sh: mkdir: /home: cannot create directory",
        "/home",
        "sh: nosuchcmd: command not found",
        "[UPROC] pid 1 exited (code 7)",
        "pid 1 (init -> /bin/sh) exited cleanly",
        # kernel diagnostics + new success banner
        "[PASS] 64-bit Addition",
        "[PASS] Signed Arithmetic",
        "[PASS] Hex Formatting",
        "[SUCCESS] fakeos: VFS, Ramfs, POSIX syscalls, TTY input, libc and interactive shell online!",
    ]

    all_passed = True
    for req in required_substrings:
        if req in stdout:
            print(f" [OK] Matched: {req}")
        else:
            print(f" [FAIL] Missing expected output: {req}")
            all_passed = False

    # Context-sensitive checks beyond plain substrings.
    contextual = [
        # echo $? right after hello must surface hello's 42 exit status
        ("$? status propagation", 'echo $?' in norm and "exited (code 42)" in norm),
        # ls /bin lists all five programs
        ("ls /bin lists init",     "\ninit\n" in norm),
        ("ls /bin lists sh",       "\nsh\n" in norm),
        ("ls /bin lists hello",    "\nhello\n" in norm),
        ("ls /bin lists cat",      "\ncat\n" in norm),
        ("ls /bin lists ls",       "\nls\n" in norm),
        # prompt returns after every command
        ("multiple shell prompts", norm.count("fakeos:~$") >= 10),
    ]
    for name, ok in contextual:
        if ok:
            print(f" [OK] Contextual: {name}")
        else:
            print(f" [FAIL] Contextual: {name}")
            all_passed = False

    if all_passed:
        print("\nAll automated boot, self-test and shell-session checks passed successfully!")
        sys.exit(0)
    else:
        print("\nTest failed: some assertions did not match.")
        sys.exit(1)

if __name__ == "__main__":
    main()
