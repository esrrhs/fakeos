#!/usr/bin/env python3
import base64
from contextlib import contextmanager
import os
import re
import selectors
import struct
import subprocess
import sys
import tempfile
import time
from bootstrap_transfer import (capture_dump, decode_dumpbin,
                                dump_transfer_budget, prepare_reference_image)

KERNEL = "build/fakeos.elf"
REBUILT = "build/fakeos-rebuilt.elf"


class VM:
    """One headless QEMU driven over the emulated UART (serial stdio)."""

    def __init__(self, kernel, mem="128M", extra_args=None):
        cmd = [
                "qemu-system-x86_64",
                "-accel", "tcg,thread=multi",
                "-kernel", kernel,
                "-m", mem,
                "-serial", "stdio",
                "-display", "none",
                "-no-reboot",
            ]
        if extra_args:
            cmd.extend(extra_args)
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.proc.stdout, selectors.EVENT_READ)

    def send(self, line):
        try:
            self.proc.stdin.write((line + "\n").encode("utf-8"))
            self.proc.stdin.flush()
            return True
        except BrokenPipeError:
            return False

    def read_available(self, timeout):
        """Drain whatever the VM emits within timeout seconds."""
        chunks = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready = self.sel.select(0.1)
            if not ready:
                continue
            try:
                data = os.read(self.proc.stdout.fileno(), 32768)
            except OSError:
                break
            if not data:
                break
            chunks.append(data.decode("utf-8", errors="surrogateescape"))
        return "".join(chunks)

    def wait_for(self, marker, timeout, failure_markers=()):
        """Read until success, an explicit failure, or timeout; return all text."""
        chunks = []
        needles = (marker,) + tuple(failure_markers)
        keep = max(len(s) for s in needles) - 1
        suffix = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready = self.sel.select(0.2)
            if not ready:
                continue
            try:
                data = os.read(self.proc.stdout.fileno(), 32768)
            except OSError:
                break
            if not data:
                break
            text = data.decode("utf-8", errors="surrogateescape")
            chunks.append(text)
            # Search only new text plus a short overlap for split markers.
            # Repeatedly scanning/concatenating the whole >28 MiB base64
            # transfer is quadratic and stalls QEMU's serial pipe.
            scan = suffix + text
            if any(needle in scan for needle in needles):
                break
            suffix = scan[-keep:] if keep else ""
        return "".join(chunks)

    def close(self):
        try:
            self.proc.terminate()
            try:
                self.proc.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.communicate()
        except Exception:
            pass


def second_boot_checks(disk):
    """Boot the in-OS linked kernel and run a reduced shell session."""
    print("\n==> Phase 5b: second boot of the in-OS rebuilt kernel...")
    vm = VM(REBUILT, mem="256M", extra_args=disk_args(disk) + net_args())
    boot = vm.wait_for("fakeos login:", 30.0)
    vm.send("root")
    boot += vm.wait_for("Password:", 5.0)
    vm.send("root")
    boot += vm.wait_for("Welcome root", 10.0)
    # Drain until a shell prompt ( ~$ or $ ).
    boot += vm.wait_for("fakeos:", 5.0)
    session = ""
    for c in ["hello", "echo $?", "pid", "echo INPUT123", "cat /etc/motd",
              "ls /bin", "ls -l /bin", "nosuchcmd", "exit 7"]:
        if not vm.send(c):
            break
        session += vm.read_available(0.6)
    # Rebuilt/TCG: SUCCESS banner can lag after exit; drain longer.
    tail = vm.read_available(8.0)
    vm.close()

    out = boot + session + tail
    with open("build/test-qemu-rebuilt.serial.log", "w", encoding="utf-8",
              errors="surrogateescape") as log:
        log.write(out)
    norm = out.replace("\r\n", "\n").replace("\r", "\n")
    required = [
        "Welcome to fakeos!",
        "[BLK] using ",
        "[EXT2] mounted:",
        "[EXT2] [PASS] create/write/read/sync /mnt/m6pass.txt",
        # big.bin 270KiB may fail on a disk already filled by the first boot's selftest
        "[VNET] ready:",
        "[NET] up: ip=10.0.2.15/24",
        "[PASS] ICMP echo reply from gateway",
        "[PASS] UDP loopback (bind/sendto/recv path)",
        "[PASS] TCP handshake + stream loopback",
        "[KBD] PS/2 keyboard armed (IRQ1)",
        "fakeos:",
        "Welcome root",
        "hello from /bin/hello",
        "-rw-r--r--",
        "exited (code 42)",
        "INPUT123",
        "Welcome to fakeos - built entirely with the fakecc toolchain.",
        "sh: nosuchcmd: command not found",
        "[UPROC] pid 1 exited (code 7)",
        "pid 1 (init -> /bin/sh) exited cleanly",
        # rebuilt image publishes no m5 payload but keeps the userspace
        "published 0 staged bootstrap files",
        "[PASS] Timer preemption",
        "[U][PASS] kill SIGKILL + wait status",
        "[U][PASS] SIGTERM handler + sigreturn",
        "[U][PASS] SIGSTOP/CONT + WUNTRACED/WCONTINUED",
        "[U][PASS] pipe read EINTR + handler",
        "[U][PASS] pipe SA_RESTART keeps read",
        "[U][PASS] orphan reparented to pid 1",
        "[NATIVE] PASS kernel-only object/channel suite",
    ]
    results = []
    for req in required:
        results.append((f"rebuilt boot: {req!r}", req in out))
    results.append(("rebuilt: pid builtin prints 1", "pid\n1\n" in norm))
    results.append(("rebuilt: multiple prompts",
                     norm.count("fakeos:~$") + norm.count("fakeos:$ ") >= 8))
    return results


@contextmanager
def ensure_disk():
    """One fresh, test-owned 32 MiB disk shared by both boots of this run.

    Reusing test-bootstrap.img made repeated `make test` depend on old volume
    contents (including a previous >21 MiB kernel). Never overwrite/delete that
    legacy fixture or the developer's build/disk.img. A unique directory owns
    only this invocation's disk and is cleaned on success, failure, or exception.
    """
    os.makedirs("build", exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="test-bootstrap-", dir="build") as tmp:
        disk = os.path.join(tmp, "disk.img")
        subprocess.check_call(["python3", "tools/mkext2.py", disk, "32"])
        yield disk


def net_args():
    # Legacy VirtIO-net + QEMU user networking (guest 10.0.2.15, gw 10.0.2.2).
    return ["-netdev", "user,id=net0",
            "-device", "virtio-net-pci,disable-modern=on,netdev=net0"]


def disk_args(disk):
    # Prefer IDE so tests exercise the ATA PIO backend; guest still probes
    # VirtIO first and falls through when no virtio-blk device is present.
    return ["-drive", f"file={disk},format=raw,if=ide"]


def run_checks(disk):
    expected_size, expected_digest = prepare_reference_image()
    print(f"==> Trusted rebuilt image: {expected_size} bytes SHA256={expected_digest}; "
          f"dump budget={dump_transfer_budget(expected_size)}s, frame stall=60s")
    print("==> Phase 1-6: boot stock kernel (+IDE disk) and run the interactive shell...")
    vm = VM(KERNEL, mem="256M", extra_args=disk_args(disk) + net_args())

    # 1. Boot + kernel self-tests + init -> login stub -> shell prompt.
    boot = vm.wait_for("fakeos login:", 30.0)
    vm.send("root")
    boot += vm.wait_for("Password:", 5.0)
    vm.send("root")  # /etc/passwd $fake$ hash
    boot += vm.wait_for("fakeos:~$", 10.0)

    # 2. Drive the interactive shell line by line.
    commands = [
        "hello",            # external exec, exit code 42
        "echo $?",          # prints 42
        "pid",              # builtin getpid -> 1
        "ppid",             # builtin getppid -> 0 (init parent)
        "uname",            # builtin utsname sysname
        "echo INPUT123",    # builtin echo
        "cat /etc/motd",    # file read through inherited fd 1
        "mkdir /home",      # builtin mkdir syscall
        "cd /home",         # chdir
        "pwd",              # getcwd -> /home
        "cd ..",
        "ls /bin",          # getdents64, programs including kbuild/ldfake
        "ls -l /bin",       # fstatat mode/size listing
        "ln -s /etc/motd /tmp/motdlink",  # symlink builtin
        "ln /etc/motd /tmp/motdhard",     # hard link builtin
        "readlink /tmp/motdlink",         # readlink builtin
        "realpath /tmp/motdlink",         # physical resolve builtin
        "ls -l /tmp",                     # show l type + -> target
        "export FOO=bar",                 # shell env -> execve envp
        "echo $FOO",
        "cat /proc/self/environ",         # child sees FOO=bar + FAKECC_PKG
        "which ls",
        "which cd",
        "env",
        "unset FOO",
        "mkdir /home",      # duplicate -> error, shell survives
        "nosuchcmd",        # exec failure -> 127 + not found
        "ls /mnt",          # Ext2 root listing (contains m6pass.txt)
        "cat /mnt/m6pass.txt",  # persistent Ext2 file written by selftest
        "echo PIPEOK | cat",    # pipe() + shell | via wait queues
        "ping -c 1 10.0.2.2",  # sys_ping + VirtIO-net ICMP
        "ping -c 1 gateway",   # /etc/hosts resolve_host
        "ping -c 1 localhost", # 127.0.0.1 loopback ICMP
        "echo \"QUOTE OK\"", # shell quote tokenizer
        "udptest",            # UDP socket loopback
        "falsecmd || echo OR_OK",  # shell || short-circuit
        "echo AND_OK && echo AND2", # shell &&
        "sync",                # ext2 flush builtin
    ]
    session = ""
    for c in commands:
        if not vm.send(c):
            break
        session += vm.read_available(0.5)

    # 3. Phase 5a: in-OS self-bootstrap. kbuild compiles every kernel
    #    module with /bin/fakecc, byte-compares each against /ref, then
    #    links /mnt/fakeos-new with /bin/ldfake. Under TCG this is the
    #    long pole (~1 min); drain until the completion marker.
    print("==> Phase 5a: running kbuild (in-OS compile + byte compare + link)...")
    bootstrap = ""
    if not vm.send("kbuild"):
        bootstrap += "kbuild dispatch failed\n"
    else:
        bootstrap += vm.wait_for(
            "[kbuild] kernel image linked", 5400.0,
            failure_markers=("[kbuild] FAIL", "[kbuild] MISMATCH"),
        )
        if "[kbuild] kernel image linked" not in bootstrap:
            print("    kbuild did not finish; capturing 5 s of tail...")
            bootstrap += vm.read_available(5.0)

    print("==> dumping /mnt/fakeos-new through /bin/dumpbin...")
    dumped = ""
    if "[kbuild] kernel image linked" in bootstrap:
        vm.send("dumpbin /mnt/fakeos-new")
        dumped, transfer_reason = capture_dump(vm.proc.stdout, vm.sel, expected_size)
        print(f"    dump transfer ended: {transfer_reason}")
        if "dumpbin: sent " not in dumped:
            dumped += vm.read_available(5.0)
        else:
            # The completion line is printed after the last frame, but the
            # serial pipe may still hold buffered bytes when wait_for's poll
            # happens to observe the marker mid-drain. Truncating there loses
            # base64 characters mid-frame and surfaces as a padding error,
            # so keep reading until the stream goes quiet.
            for _ in range(20):
                extra = vm.read_available(0.5)
                if not extra:
                    break
                dumped += extra

    # 4. Release pid 1 (code 7) -> monitor SUCCESS banner.
    vm.send("exit 7")
    tail = vm.read_available(3.0)
    vm.close()

    stdout = boot + session + bootstrap + dumped + tail
    # Console writes translate LF to CRLF; normalize for line-based checks.
    norm = stdout.replace("\r\n", "\n").replace("\r", "\n")

    # The dumpbin transfer embeds ~400 KiB of base64; redact it from the
    # human-readable log while preserving every frame marker.
    display = re.sub(
        r"(===DUMP/BEGIN.*?===)[\s\S]*?(===DUMP/END===)",
        r"\1<base64 frame>\2", stdout)
    print("\n--- [Captured Kernel Output Start] ---")
    print(display)
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
        "[SMP] AP trampoline at phys",
        "[SMP] sched: single-CPU, no AP attach",
        "[PASS] I/O APIC IRQ1 (kbd) -> 33, IRQ4 (COM1) -> 36 armed",
        "[EXT2] [PASS] create/write/read/sync /mnt/m6pass.txt",
        "[EXT2] [PASS] double-indirect /mnt/big.bin 270KiB",
        "[EXT2] [PASS] triple-indirect sparse /mnt/trip.bin",
        "[EXT2] [PASS] ftruncate grow/shrink/zero",
        "[EXT2] [PASS] rename same-dir/cross-dir",
        "[EXT2] [PASS] journal ordered durability recover",
        "[EXT2] [PASS] journal commit-block recover",
        "[EXT2] [PASS] journal descriptor/revoke + multi-txn ring",
        "[EXT2] [PASS] journal metadata-copy replay",
        "[EXT2] [PASS] journal ordered blk_async meta-copy",
        "[EXT2] [PASS] journal copy pool grow",
        "[EXT2] [PASS] journal revoke-aware meta-copy replay",
        "[EXT2] [PASS] data-path blk_async full-block",
        "[EXT2] journal-lite ordered + descriptor/revoke + meta-copy/8 + revoke-aware replay + data-path blk_async/8 online (not a full journal)",
        # AHCI probe log (QEMU may or may not expose 01.06)
        re.compile(r"\[BLK\] (?:no AHCI \(PCI 01\.06\)|AHCI PCI 01\.06)"),
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
        "[PASS] MAP_SHARED anon",
        "[U][PASS] POSIX shm_open/mmap/unlink",
        "[U][PASS] /dev/null /dev/zero /dev/tty",
        "[U][PASS] /proc/self status/maps/cmdline",
        "[U][PASS] getppid/uname/nanosleep/clock_gettime",
        "[U][PASS] stat/fstat/newfstatat",
        "[U][PASS] futex WAIT/WAKE",
        "[U][PASS] pthread_mutex futex lock/unlock",
        "[U][PASS] pthread_create clone same-AS",
        "[U][PASS] pthread_join + FS TLS",
        "[U][PASS] pthread_exit retval",
        "[U][PASS] pthread_detach + stack reclaim",
        "[U][PASS] pthread_join CLEARTID",
        "[PASS] Cooperative threads",
        "[PASS] Timer preemption",
        "[PASS] CFS-lite",
        "[PASS] CFS rbtree",
        "[PASS] CFS task_group: shares",
        "[PASS] CFS task_group: equal shares",
        "[PASS] CFS task_group nested: mid shares",
        "[PASS] CFS task_group nested: equal mids",
        "[PASS] CFS task_group SE rbtree",
        "[PASS] kmutex sleep-lock",
        "[PASS] kmutex waiter enters TCB_SLEEPING",
        "[PASS] ksem down/up rendezvous + count",
        "[PASS] wait_queue wake_n(1) then wake-all",
        "[PASS] pipe kmutex + wait_queue_sleep_unlock blocking",
        "[PASS] tty kmutex + wait_queue_sleep_unlock blocking",
        "[PASS] sock kmutex + wait_queue_sleep_unlock blocking",
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
        "[FS] published /bin/touch (",
        "[FS] published /bin/mv (",
        "[FS] published /bin/ping (",
        "[FS] published /bin/udptest (",
        "[FS] published /bin/nc (",
        "[FS] published /bin/ed (",
        "[FS] published /bin/login (",
        "[FS] published /bin/head (",
        "[FS] published /bin/wc (",
        "[FS] published /bin/getent (",
        "[FS] published /bin/tail (",
        "[FS] published /bin/grep (",
        "[FS] published /bin/printf (",
        "[FS] published /bin/tee (",
        "[FS] published /bin/basename (",
        "[FS] published /bin/dirname (",
        "[FS] published /bin/true (",
        "[FS] published /bin/false (",
        "[FS] published /bin/seq (",
        "[FS] published /bin/cut (",
        "[FS] published /bin/tr (",
        "[FS] published /bin/test (",
        "[FS] published /bin/[ (",
        "[FS] published /bin/uniq (",
        "[FS] published /bin/sort (",
        "[FS] published /bin/yes (",
        "[FS] published /bin/od (",
        "[FS] published /bin/xargs (",
        "[FS] published /bin/cmp (",
        "[FS] published /bin/dd (",
        "[FS] published /bin/expr (",
        "[FS] published /bin/hostname (",
        "[FS] published /bin/echo (",
        "[FS] published /bin/sleep (",
        "[FS] published /bin/truncate (",
        "[FS] published /bin/mkdir (",
        "[FS] published /bin/pwd (",
        "[FS] published /bin/ln (",
        "[FS] published /bin/chmod (",
        "[FS] published /bin/uname (",
        "[FS] published /bin/env (",
        "[FS] published /bin/sync (",
        "[FS] published /bin/readlink (",
        "[FS] published /bin/realpath (",
        "[FS] published /bin/date (",
        "[FS] published /bin/stat (",
        "[FS] published /bin/nl (",
        "[FS] published /bin/fold (",
        "[FS] published /bin/mktemp (",
        "[FS] published /bin/rev (",
        "[FS] published /bin/paste (",
        "[FS] published /bin/expand (",
        "[FS] published /bin/unexpand (",
        "[FS] published /bin/comm (",
        "[FS] published /bin/join (",
        "[FS] published /bin/split (",
        "[FS] published /bin/csplit (",
        "[FS] published /bin/fmt (",
        "[FS] published /bin/which (",
        "[FS] published /bin/cksum (",
        "[FS] published /bin/sum (",
        "[FS] published /bin/strings (",
        "[FS] published /bin/pr (",
        "[FS] published /bin/column (",
        "[FS] published /bin/md5sum (",
        "[FS] published /bin/sha1sum (",
        "[FS] published /bin/sha256sum (",
        "[FS] published /bin/base64 (",
        "[FS] published /bin/factor (",
        "[FS] published /bin/clear (",
        "[FS] published /bin/tty (",
        "[FS] published /bin/whoami (",
        "[FS] published /bin/id (",
        "[FS] published /bin/groups (",
        "[FS] published /bin/cal (",
        "[FS] published /bin/install (",
        "[FS] published /bin/chown (",
        "[FS] published /bin/chgrp (",
        "[FS] published /bin/tac (",
        "[FS] published /bin/nproc (",
        "[FS] published /bin/timeout (",
        "[FS] published /bin/rmdir (",
        "[FS] published /bin/unlink (",
        "[FS] published /bin/tsort (",
        "[FS] published /bin/pathchk (",
        "[FS] published /bin/logname (",
        "[FS] published /bin/nohup (",
        "[FS] published /bin/find (",
        "[FS] published /bin/du (",
        "[FS] published /bin/shuf (",
        "[FS] published /bin/kill (",
        "[FS] published /bin/diff (",
        "[FS] published /proc/cpuinfo (",
        "[FS] published /bin/printenv (",
        "[FS] published /etc/motd (",
        "[FS] published /etc/resolv.conf (",
        "[FS] published /etc/hosts (",
        "[FS] published /etc/passwd (",
        "[FS] published /dev/null /dev/zero /dev/tty",
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
        "[U][PASS] VMA_FILE demand mmap",
        "[U][PASS] file MAP_FIXED mmap",
        "[U][PASS] mprotect RO/RW",
        "[U][PASS] PROT_NONE + mmap addr hint",
        "[U][PASS] symlink/readlink",
        "[U][PASS] cross-mount symlink namespace",
        "[U][PASS] hard link(2)",
        "[U][PASS] getcwd physical",
        "[U][PASS] vfs getdents",
        "[U][PASS] execve rejects missing image",
        "[U][PASS] execve rejects non-ELF file",
        "[U][PASS] huge write length rejected",
        "[U][PASS] orphan reparented to pid 1",
        "[U][PASS] kill SIGKILL + wait status",
        "[U][PASS] SIGTERM handler + sigreturn",
        "[U][PASS] SIGSTOP/CONT + WUNTRACED/WCONTINUED",
        "[U][PASS] pipe read EINTR + handler",
        "[U][PASS] pipe SA_RESTART keeps read",
        # interactive shell session
        "fakeos login:",
        "Welcome root",
        "fakeos shell - session/fg, pipes/redirects, &&/||, ;/for/while/until/if/elif/case/esac/break[N]/continue[N]/exec, [[ ]], :/true/false, let/declare/typeset, eval/source/., jobs[-l]/fg/bg/wait/Ctrl-Z/ioctl/tcsetpgrp, printf/history/!!/readline/Tab/Double-Tab/Ctrl-W/K/V/L + /home/.sh_history, env/export[-np]/readonly[-p]/type[-t]/command[-vV]/builtin/hash[-rplt]/read/alias/unalias/return/local/shift/functions/envp, set -e/-u/-o, $((arith))/$0..$9/$#/$@/$*, cd[-LP]/pushd/popd/dirs[-vp], chmod/umask/chown/id, PATH/quotes, kill; persist under /home",
        "hello from /bin/hello",
        "INPUT123",
        "Welcome to fakeos - built entirely with the fakecc toolchain.",
        "Try: hello, ls /bin, cat /etc/motd",
        "sh: mkdir: /home: cannot create directory",
        "/home",
        "sh: nosuchcmd: command not found",
        "[UPROC] pid 1 exited (code 7)",
        # NOTE: the "exited cleanly" monitor line is only emitted when the
        # shell exits inside the 30 s monitor window. The in-OS bootstrap
        # (kbuild ~1 min) deliberately exceeds that window; pid 1's clean
        # code-7 exit above still proves correct termination, and the
        # short second-boot session asserts "exited cleanly" end to end.
        # kernel diagnostics + new success banner
        "[PASS] 64-bit Addition",
        "[PASS] Signed Arithmetic",
        "[PASS] Hex Formatting",
        "[SUCCESS] fakeos: CFS nested task_group SE + CFS rbtree + AHCI blk_async end_io + bio queue + async NCQ + Ext2 data-path blk_async + journal copy-pool/8 revoke-aware replay + sock/tty/pipe kmutex + sleeping locks + CLEARTID + IPI resched + signals SMP + steal + pthread + futex online!",
        # ---- Stage 5: in-OS self-bootstrap -------------------------------
        # kbuild stage a: the fakecc-compiled ping runs inside the OS
        "[kbuild] ping compile+run OK",
        "VMAIO adjacency/content/hole/offset OK",
        "[kbuild] truncated object rejected cleanly",
        "[kbuild] identical kernel/kmain.o",
        # every kernel module recompiled by the in-OS compiler matches the
        # host reference object byte-for-byte
        re.compile(r"\[kbuild\] all 43 kernel objects byte-identical"),
        # ldfake linked the multiboot kernel image
        "[ldfake] wrote /mnt/fakeos-new",
        "[kbuild] kernel image linked: /mnt/fakeos-new",
        # dumpbin frame transfer completed
        "dumpbin: sent ",
    ]

    all_passed = True
    for req in required_substrings:
        # entries are either plain substrings or compiled patterns
        if isinstance(req, str):
            ok_req = req in stdout
            label = req
        else:
            ok_req = bool(req.search(stdout))
            label = req.pattern
        if ok_req:
            print(f" [OK] Matched: {label}")
        else:
            print(f" [FAIL] Missing expected output: {label}")
            all_passed = False

    # Context-sensitive checks beyond plain substrings.
    contextual = [
        # echo $? right after hello must surface hello's 42 exit status
        ("$? status propagation", 'echo $?' in norm and "exited (code 42)" in norm),
        ("pipe echo|cat", "PIPEOK\n" in norm and "usage: cat" not in norm),
        ("shell || short-circuit", "OR_OK\n" in norm),
        ("shell && list", "AND_OK\n" in norm and "AND2\n" in norm),
        # pid builtin reports that the shell kept pid 1 across execve
        ("shell keeps pid 1", "pid\n1\n" in norm),
        ("realpath motdlink", "/etc/motd\n" in norm),
        ("ls -l symlink arrow", "motdlink -> /etc/motd" in norm),
        ("getcwd physical init", "[U][PASS] getcwd physical" in stdout),
        ("shell echo $FOO", "\nbar\n" in norm or "echo $FOO\nbar\n" in norm),
        ("execve envp FOO in child environ", "FOO=bar" in stdout),
        ("execve envp FAKECC_PKG live", "FAKECC_PKG=/src" in stdout),
        ("shell which ls", "/bin/ls" in norm),
        ("shell which cd builtin", "cd: shell builtin" in norm),
        ("shell env PATH", "PATH=/bin:/usr/bin:." in norm),
        # ls /bin lists all five programs
        ("ls /bin lists init",     "\ninit\n" in norm),
        ("ls /bin lists sh",       "\nsh\n" in norm),
        ("ls /bin lists hello",    "\nhello\n" in norm),
        ("ls /bin lists cat",      "\ncat\n" in norm),
        ("ls /bin lists ls",       "\nls\n" in norm),
        # prompt returns after every command
        ("multiple shell prompts",
         norm.count("fakeos:~$") + norm.count("fakeos:/") >= 19),
        ("userland ping replied", "reply from 10.0.2.2" in norm),
        ("userland udptest loopback", "UDP loopback OK" in norm),
    ]
    # kbuild/ldfake also publish the new user programs
    #
    # Every module must be byte-identical. Upstream fakecc had a use-after-free
    # in its semantic analyser (ftab_lookup() handing out a pointer into a
    # realloc'd table) that made register allocation read uninitialised heap
    # bytes, so its two build paths -- CMake host build vs the self-hosted
    # compiler running inside fakeos -- disagreed on kernel/vfstest.c by 5
    # bytes. That is fixed upstream (esrrhs/fakecc#92); a DIFF here now means
    # the self-bootstrap really is not reproducible, which kbuild treats as
    # fatal, so the assertion is strict again rather than tolerating a slack
    # module. See docs/fakecc-bootstrap-nondeterminism.md.
    mismatches = len(re.findall(r"\[kbuild\] DIFF ", stdout))
    identical = norm.count("[kbuild] identical ")
    extra = [
        ("ls /bin lists kbuild", "\nkbuild\n" in norm),
        ("ls /bin lists ldfake", "\nldfake\n" in norm),
        ("ls /bin lists dumpbin", "\ndumpbin\n" in norm),
        # every module accounted for, and all of them identical
        ("all 43 modules accounted for",
         identical + mismatches == 43),
        (f"all 43 modules byte-identical (saw {identical} identical, "
         f"{mismatches} mismatched)", identical == 43 and mismatches == 0),
        ("no kbuild FAIL line", "[kbuild] FAIL" not in stdout),
        ("no ldfake error line", "ldfake: " not in stdout
                                 or "[ldfake] wrote" in stdout),
    ]
    for name, ok in extra:
        if ok:
            print(f" [OK] Contextual: {name}")
        else:
            print(f" [FAIL] Contextual: {name}")
            all_passed = False

    # ---- Stage 5a artifacts: decode the dumpbin transfer --------------
    structural = []
    try:
        blob, nframes, declared = decode_dumpbin(stdout, expected_size, expected_digest)
    except ValueError as error:
        print(f" [FAIL] Full dump validation: {error}")
        blob, nframes, declared = None, 0, 0
    if blob is None:
        structural.append(("dumpbin frames decoded", False))
    else:
        structural.append(("rebuilt: exact trusted reference size and SHA256", True))
        structural.append((f"dumpbin frames decoded ({nframes} frames, "
                           f"{declared} bytes)", len(blob) == declared))
        if len(blob) == declared:
            with open(REBUILT, "wb") as f:
                f.write(blob)
            # ELF32 multiboot structure, parsed from the serial transfer
            ok = (blob[:4] == b"\x7fELF" and blob[4] == 1
                  and blob[0x1000:0x1004] == bytes.fromhex("02b0ad1b"))
            structural.append(("rebuilt: ELF32 + MB1 magic", ok))
            entry, phoff = struct.unpack_from("<II", blob, 24)
            phnum = struct.unpack_from("<H", blob, 44)[0]
            phs = [struct.unpack_from("<IIIIIIII", blob, phoff + i * 32)
                   for i in range(phnum)]
            structural.append(("rebuilt: entry 0x100028", entry == 0x100028))
            structural.append(("rebuilt: large-image storage regression (>16 MiB)",
                               len(blob) > 16 * 1024 * 1024))
            structural.append(("rebuilt: full loadable file ranges transferred",
                               max(p[1] + p[4] for p in phs) == len(blob)))
            structural.append((f"rebuilt: {phnum} program headers (2)",
                               phnum == 2))
            if len(phs) == 2:
                lo, hi = phs
                structural.append((
                    "rebuilt: low segment 0x100000/0x5000",
                    lo[1:6] == (0x1000, 0x100000, 0x100000, 0x5000, 0x5000)))
                structural.append((
                    "rebuilt: high segment va 0x80105000 pa 0x105000",
                    hi[1] == 0x6000 and hi[2] == 0x80105000
                    and hi[3] == 0x105000 and hi[4] > 0 and hi[5] >= hi[4]))
            # the 32-bit bootstrap region must be byte-identical to the
            # GNU-linked kernel image (same objects, same placement)
            try:
                with open(KERNEL, "rb") as f:
                    gnu = f.read()
                same = blob[0x1000:0x6000] == gnu[0x1000:0x6000]
                structural.append((
                    "rebuilt: low 0x5000 bytes identical to GNU image",
                    same))
            except OSError:
                structural.append(("GNU image readable for compare", False))

    for name, ok in structural:
        if ok:
            print(f" [OK] {name}")
        else:
            print(f" [FAIL] {name}")
        all_passed = all_passed and ok

    # ---- Stage 5b: second boot of the rebuilt kernel ------------------
    second = []
    if all(s for _, s in structural):
        second = second_boot_checks(disk)
    else:
        print("\n[SKIP] second boot: phase-5a structural checks failed")
    for name, ok in second:
        if ok:
            print(f" [OK] {name}")
        else:
            print(f" [FAIL] {name}")
        all_passed = all_passed and ok

    if all_passed:
        print("\nAll boot, self-test, shell-session and self-bootstrap "
              "checks passed successfully!")
        sys.exit(0)
    else:
        print("\nTest failed: some assertions did not match.")
        sys.exit(1)

def main():
    # Keep exactly the same disk alive through stock and reconstructed boots.
    with ensure_disk() as disk:
        run_checks(disk)


if __name__ == "__main__":
    main()
