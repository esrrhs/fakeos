#!/usr/bin/env python3
"""Real Ext2 reclaim/reuse and survivor persistence; one disk for both boots."""
import os
import subprocess
import tempfile
from test_vfs_mounts import Session


def run(disk):
    for boot in range(2):
        session = Session(disk)
        try:
            # Four dense 768KiB files per probe x six repeats x two boots
            # allocates >36MiB on the SAME 32MiB disk: actual reuse is required.
            for repeat in range(6):
                required = ["[U][PASS] Ext2 indirect reclaim/accounting/survivor"]
                if boot or repeat:
                    required.append("[U] Ext2 reclaim persisted survivor verified")
                session.command("/bin/init --ext2-reclaim", required, timeout=180.0)
            session.command("sync")
        finally:
            session.close()
    print("[PASS] Ext2 repeated reclaim/reuse, exact counts, survivor and reboot persistence", flush=True)


def main():
    os.makedirs("build", exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ext2-reclaim-", dir="build") as tmp:
        disk = os.path.join(tmp, "disk.img")
        subprocess.check_call(["python3", "tools/mkext2.py", disk, "32"])
        run(disk)  # NEVER reset disk between operations or between boots.


if __name__ == "__main__":
    main()
