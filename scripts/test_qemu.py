#!/usr/bin/env python3
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

    print("==> Starting QEMU process...")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    # Let the kernel run and print output (includes a 500 ms APIC timer test)
    time.sleep(3.0)
    proc.terminate()
    try:
        stdout, stderr = proc.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()

    print("\n--- [Captured Kernel Output Start] ---")
    print(stdout.strip())
    print("--- [Captured Kernel Output End] ---\n")

    required_substrings = [
        "Welcome to fakeos!",
        "Bootloader detected: Multiboot 1",
        "Kernel Memory Layout (Higher-Half)",
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
        "[PASS] Single-page alloc/free",
        "[PASS] Multi-order alloc",
        "[PASS] Buddy merge",
        "[PASS] Stress test",
        "4-Level Page Table Toolchain Online",
        "[PASS] Map/translate/unmap",
        "Slab Kernel Heap Online",
        "[PASS] Size-class alloc",
        "[PASS] Big alloc path",
        "[PASS] Free-list reuse",
        "[PASS] Cooperative threads",
        "[PASS] Timer preemption",
        "[PASS] Demand paging",
        "[PASS] Permissions: read-only VMA",
        "[PASS] COW clone",
        "[U] hello from ring 3, pid=",
        "[U][PASS] child COW write took effect",
        "[U][PASS] parent COW value intact after child write",
        "[PASS] Ring-3 entry",
        "[PASS] SYSCALL/SYSRET",
        "[PASS] COW fork across user processes",
        "[PASS] Timer preemption of ring-3 threads",
        "[PASS] 64-bit Addition",
        "[PASS] Signed Arithmetic",
        "[PASS] Hex Formatting",
        "[SUCCESS] fakeos: Ring-3 user processes + SYSCALL/SYSRET fast syscalls online!"
    ]

    all_passed = True
    for req in required_substrings:
        if req in stdout:
            print(f" [OK] Matched: {req}")
        else:
            print(f" [FAIL] Missing expected output: {req}")
            all_passed = False

    if all_passed:
        print("\nAll automated boot and execution tests passed successfully!")
        sys.exit(0)
    else:
        print("\nTest failed: some assertions did not match.", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
