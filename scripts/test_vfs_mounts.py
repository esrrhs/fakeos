#!/usr/bin/env python3
"""Real guest namespace/persistence regression (not a power-loss guarantee).

Uses a fresh disposable disk, never build/disk.img or test-bootstrap.img.
No network fixture is needed. The raw-syscall assertions live in user/init.c.
"""
import os
import re
import subprocess
import tempfile

from test_qemu import KERNEL, VM, disk_args


class Session:
    def __init__(self, disk):
        self.vm = VM(KERNEL, mem="256M", extra_args=disk_args(disk))
        self.serial = 0
        try:
            boot = self.vm.wait_for("fakeos login:", 60.0)
            print(boot, flush=True)
            if "[U][PASS] cross-mount symlink namespace" not in boot:
                raise AssertionError("raw syscall namespace regressions did not pass")
            if "[U][FAIL]" in boot:
                raise AssertionError("guest init regression failed")
            self.vm.send("root")
            self.vm.wait_for("Password:", 5.0)
            self.vm.send("root")
            welcome = self.vm.wait_for("fakeos:", 10.0)
            if "Welcome root" not in welcome:
                raise AssertionError("guest login failed")
        except BaseException:
            self.vm.close()
            raise

    def command(self, command, lines=(), status=0, patterns=(), timeout=20.0):
        self.serial += 1
        done = "__VFS_DONE_%d__" % self.serial
        self.vm.send(command + "; echo __VFS_RC=$?; echo " + done)
        out = self.vm.wait_for("\r\n" + done + "\r\n", timeout)
        print(out, flush=True)
        normalized = out.replace("\r\n", "\n").replace("\r", "\n")
        actual = normalized.splitlines()
        if done not in actual:
            raise AssertionError("guest command timed out: " + command)
        statuses = re.findall(r"^__VFS_RC=([0-9]+)$", normalized, re.MULTILINE)
        if statuses != [str(status)]:
            raise AssertionError("unexpected command status: " + command)
        for line in lines:
            if line not in actual:
                raise AssertionError("missing exact output %r from %s" % (line, command))
        for pattern in patterns:
            if not any(re.fullmatch(pattern, line) for line in actual):
                raise AssertionError("missing output pattern %r" % pattern)
        return normalized

    def close(self):
        self.vm.close()


def run(disk):
    first = Session(disk)
    try:
        first.command("mkdir /mnt/vfs-work")
        first.command("ln -s /mnt/vfs-work /tmp/vfs-work")
        first.command("echo VFS_PERSIST_PAYLOAD > /tmp/vfs-work/data")
        first.command("cat /mnt/vfs-work/data", ["VFS_PERSIST_PAYLOAD"])
        first.command("ln -s data /tmp/vfs-work/relative-link")
        first.command("ln -s /mnt/vfs-work/data /mnt/vfs-work/absolute-link")
        first.command("ln -s /etc/motd /mnt/vfs-work/to-ram")
        first.command("cat /mnt/vfs-work/to-ram",
                      ["Welcome to fakeos - built entirely with the fakecc toolchain."])
        first.command("echo VFS_VOLATILE_PAYLOAD > /tmp/vfs-volatile")
        first.command("ln -s /tmp/vfs-volatile /mnt/vfs-work/outside-link")
        first.command("cat /mnt/vfs-work/outside-link", ["VFS_VOLATILE_PAYLOAD"])
        first.command("mkdir /tmp/vfs-work/sub")
        first.command("echo VFS_RENAMED_PAYLOAD > /tmp/vfs-work/sub/before")
        first.command("mv /tmp/vfs-work/sub/before /tmp/vfs-work/sub/after")
        first.command("cat /mnt/vfs-work/sub/after", ["VFS_RENAMED_PAYLOAD"])
        first.command("rm /tmp/vfs-work/relative-link")
        first.command("cat /mnt/vfs-work/data", ["VFS_PERSIST_PAYLOAD"])
        first.command("ln -s data /tmp/vfs-work/relative-link")
        first.command("ln -s /bin/hello /mnt/vfs-work/exec-ram")
        first.command("/mnt/vfs-work/exec-ram", status=42,
                      patterns=[r"hello from /bin/hello \(argv\[0\]=/mnt/vfs-work/exec-ram, pid=[0-9]+\)"])
        first.command("cd /tmp/vfs-work; pwd", ["/mnt/vfs-work"])
        first.command("sync")
    finally:
        first.close()
    # QEMU has closed the disk before it is reopened by the second guest.
    second = Session(disk)
    try:
        second.command("cat /mnt/vfs-work/data", ["VFS_PERSIST_PAYLOAD"])
        second.command("cat /mnt/vfs-work/relative-link", ["VFS_PERSIST_PAYLOAD"])
        second.command("cat /mnt/vfs-work/absolute-link", ["VFS_PERSIST_PAYLOAD"])
        second.command("cat /mnt/vfs-work/sub/after", ["VFS_RENAMED_PAYLOAD"])
        second.command("cat /mnt/vfs-work/to-ram",
                       ["Welcome to fakeos - built entirely with the fakecc toolchain."])
        second.command("cat /mnt/vfs-work/outside-link", status=1)
        second.command("echo VFS_RECREATED_PAYLOAD > /tmp/vfs-volatile")
        second.command("cat /mnt/vfs-work/outside-link", ["VFS_RECREATED_PAYLOAD"])
        second.command("ln -s /mnt/vfs-work /tmp/vfs-work")
        second.command("cat /tmp/vfs-work/data", ["VFS_PERSIST_PAYLOAD"])
        second.command("sync")
    finally:
        second.close()
    print("[PASS] VFS namespace and synced two-boot persistence workflows", flush=True)


def main():
    os.makedirs("build", exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vfs-mount-", dir="build") as tmp:
        disk = os.path.join(tmp, "disk.img")
        subprocess.check_call(["python3", "tools/mkext2.py", disk, "32"])
        run(disk)


if __name__ == "__main__":
    main()
