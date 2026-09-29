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

    # Let the kernel run and print output
    time.sleep(1.5)
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
        "[PASS] 64-bit Addition",
        "[PASS] Signed Arithmetic",
        "[PASS] Hex Formatting",
        "[SUCCESS] fakeos minimal demo has fully run through the end-to-end pipeline!"
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
