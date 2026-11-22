#!/usr/bin/env python3
"""Host-side fakecc unit tests: the tools/test_*.py checks that compile real
kernel/user bodies and run the result without QEMU.

These have been documented in docs/daily-cli.md as accepted checks for a long
time while nothing executed them, which is how tools/test_clock_io.py was able
to sit broken against four kernel refactors without anyone noticing.

Excluded on purpose: test_net_tcp_io.py, test_net_irq_work.py and
test_net_wire_io.py. Those three ignore FCC and pass a hardcoded
/tmp/fakecc-pr116-merge as the compiler location, so they cannot run on any
machine that is not that exact scratch checkout; they need their toolchain
lookup fixed before they can be gated at all.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FCC = os.environ.get('FCC', 'fakecc')
TIMEOUT = 300

SUITES = [
    'tools/test_clock_io.py',
    'tools/test_cp_io.py',
    'tools/test_dirent_io.py',
    'tools/test_ext2_dir_add.py',
    'tools/test_mv_io.py',
    'tools/test_pmm_buddy.py',
    'tools/test_proc_job_notice.py',
    'tools/test_user_cstring.py',
]


def main():
    print(f'host unit tests: FCC={FCC}', flush=True)
    failed = []
    for suite in SUITES:
        try:
            proc = subprocess.run([sys.executable, suite], cwd=str(ROOT),
                                  capture_output=True, text=True, timeout=TIMEOUT,
                                  env=dict(os.environ, FCC=FCC))
            output = (proc.stdout + proc.stderr).strip()
            rc = proc.returncode
        except subprocess.TimeoutExpired:
            output = f'no result within {TIMEOUT}s'
            rc = 'TIMEOUT'
        if rc != 0:
            failed.append(suite)
            # Name the suite and quote what the compiler or the binary said, so
            # a red run explains itself instead of just counting failures.
            print(f'FAIL {suite} rc={rc}', flush=True)
            print('\n'.join(output.splitlines()[-15:]), flush=True)
        else:
            tail = output.splitlines()
            print(f'PASS {suite}: {tail[-1] if tail else ""}', flush=True)
    print(f'host unit tests: {len(SUITES) - len(failed)}/{len(SUITES)} passed',
          flush=True)
    if failed:
        print(f'failing suites: {", ".join(failed)}', flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
