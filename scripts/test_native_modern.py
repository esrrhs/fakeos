#!/usr/bin/env python3
"""Modern native high-concurrency kernel acceptance suite (SMP1/2/4):
deadlines, rich byte+cap messages (DUP/MOVE), multi-object wait_any,
multithreaded zero-legacy nat_process_spawn, nat_task_spawn/join/cancel,
shared process FD/CWD table with deferred-close UAF safety, and sharded SMP storm.
"""
import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path
from test_qemu import VM, disk_args


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--evidence', default='build/native-modern')
    p.add_argument('--cpus', default='1,2,4')
    a = p.parse_args()
    e = Path(a.evidence)
    e.mkdir(parents=True, exist_ok=True)
    rows = []
    for cpu in map(int, a.cpus.split(',')):
        with tempfile.TemporaryDirectory(prefix='native-modern-', dir='build') as d:
            disk = str(Path(d) / 'disk.img')
            subprocess.run(['python3', 'tools/mkext2.py', disk, '32'], check=True)
            vm = VM('build/fakeos.elf', mem='256M',
                    extra_args=disk_args(disk) + ['-smp', str(cpu), '-nic', 'none'])
            out = ''
            try:
                idle = 'System idle. Yielding for IRQ-driven TTY/net I/O.'
                out += vm.wait_for(idle, 300, failure_markers=('[PANIC]', '[EXCEPTION]'))
                assert idle in out, f'SMP{cpu}: boot-complete barrier missing'
                got_login = False
                for _ in range(4):
                    vm.send('root')
                    out += vm.wait_for('Password:', 30, failure_markers=('[PANIC]',))
                    if 'Password:' in out:
                        got_login = True
                        break
                assert got_login, f'SMP{cpu}: login prompt never appeared'
                vm.send('root')
                out += vm.wait_for('Welcome root', 60)
                out += vm.wait_for('fakeos:', 60)
                vm.send('/bin/init --native-modern; echo __NMOD_RC=$?; echo __NMOD_DONE__')
                out += vm.wait_for('\r\n__NMOD_DONE__\r\n', 180,
                                   failure_markers=('[PANIC]', '[EXCEPTION]'))
            finally:
                out += vm.read_available(2.0)
                vm.close()
                (e / f'smp{cpu}.serial.log').write_text(out)

            steps = re.findall(r'^\[NATIVE-MODERN\] (step\d+) PASS (.+)$', out, re.M)
            assert '__NMOD_RC=0' in out, (
                f'SMP{cpu}: native-modern failed (steps={len(steps)}/9); see {e}')
            assert len(steps) == 9, f'SMP{cpu}: steps incomplete ({len(steps)}/9)'
            assert '[NATIVE-MODERN] PASS modern native high-concurrency kernel suite' in out
            assert '[WAIT] PASS interruptible admission cases=18 failures=0' in out
            assert '[FAIL]' not in out, f'SMP{cpu}: kernel/user [FAIL] marker present'
            rows.append({
                'cpus': cpu,
                'steps': [s[0] for s in steps],
                'pass': True,
            })
            (e / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
            print(f'SMP{cpu} PASS: 9/9 modern native kernel steps', flush=True)


if __name__ == '__main__':
    main()
