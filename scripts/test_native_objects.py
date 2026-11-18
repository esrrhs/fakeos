#!/usr/bin/env python3
"""Real guest kernel-native object tests on SMP1/2/4, sequential VMs, no NIC.

Kernel-only API: this does not assert native cross-process capabilities exist.
Existing initial userspace fork/thread PASS markers are recorded as compatibility
coverage, and run before AP release; they are NOT SMP IPC acceptance evidence.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile
from test_qemu import VM, disk_args


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence", default="build/native-object-tests")
    parser.add_argument("--repeat", type=int, default=1)
    args = parser.parse_args()
    if args.repeat < 1: parser.error("--repeat must be positive")
    evidence = Path(args.evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    for run, cpus in ((run, cpu) for run in range(1, args.repeat + 1) for cpu in (1, 2, 4)):
        with tempfile.TemporaryDirectory(prefix="native-objects-", dir="build") as tmp:
            disk = str(Path(tmp) / "disk.img")
            subprocess.run(["python3", "tools/mkext2.py", disk, "32"], check=True)
            vm = VM("build/fakeos.elf", mem="256M", extra_args=
                    disk_args(disk) + ["-smp", str(cpus), "-nic", "none"])
            output = ""
            try:
                output = vm.wait_for("[NATIVE] PASS kernel-only object/channel suite", 180,
                                     failure_markers=("[NATIVE] FAIL", "[PANIC]", "[EXCEPTION]"))
            finally:
                output += vm.read_available(0.2)
                vm.close()
                (evidence / f"run{run}-smp{cpus}.serial.log").write_text(output)
            expected = "[NATIVE] PASS kernel-only object/channel suite"
            if expected not in output:
                raise AssertionError(f"SMP{cpus}: native suite failed/timed out; see {evidence}")
            match = re.search(r"native channel MPMC: 512 exact-once messages, CPUs=(\d+) mask=0x([0-9a-fA-F]+)", output)
            if not match or int(match[1]) != cpus:
                raise AssertionError(f"SMP{cpus}: CPU count evidence missing")
            mask = int(match[2], 16)
            if not mask or mask >= (1 << cpus) or (cpus > 1 and mask.bit_count() < 2):
                raise AssertionError(f"SMP{cpus}: actual multicore execution missing: {mask:#x}")
            rows = re.findall(r"native CPU (\d+): send=(\d+) recv=(\d+)", output)
            if len(rows) != cpus or {int(row[0]) for row in rows} != set(range(cpus)):
                raise AssertionError(f"SMP{cpus}: per-core transfer evidence missing")
            if sum(int(row[1]) for row in rows) != 512 or sum(int(row[2]) for row in rows) != 512:
                raise AssertionError(f"SMP{cpus}: per-core transfer accounting incorrect")
            actual_mask = sum(1 << int(cpu) for cpu, sent, received in rows if int(sent) or int(received))
            if mask != actual_mask:
                raise AssertionError(f"SMP{cpus}: CPU mask does not match actual transfers")
            if "native barrier: 4/4 workers runnable before payload release" not in output:
                raise AssertionError(f"SMP{cpus}: overlapping runnable worker barrier missing")
            if cpus > 1 and "[PASS] IPI resched balance: remote=" not in output:
                raise AssertionError(f"SMP{cpus}: previous remote-balance regression")
            if "[FAIL]" in output:
                raise AssertionError(f"SMP{cpus}: earlier guest regression failed")
            for marker in ("[U][PASS] parent COW value intact after child write",
                           "[U][PASS] pthread_create clone same-AS",
                           "[U][PASS] pthread_join + FS TLS"):
                if marker not in output:
                    raise AssertionError(f"SMP{cpus}: initial user compatibility missing {marker!r}")
            print(f"PASS run{run} SMP{cpus}: native objects/channel teardown + MPMC, observed CPU mask={mask:#x}; "
                  "initial user fork/thread compatibility preserved (not native process IPC)", flush=True)


if __name__ == "__main__":
    main()
