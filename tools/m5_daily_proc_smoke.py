#!/usr/bin/env python3
"""Login-shell smoke for live /proc/<pid>, /proc/meminfo, /bin/ps and /bin/free.

Does not claim GUI/desktop readiness. Requires a built build/fakeos.elf.
"""
import argparse
import os
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from test_qemu import VM


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--smp', type=int, default=1)
    parser.add_argument('--log', default='/tmp/fakeos-daily-proc.log')
    args = parser.parse_args()
    os.chdir(ROOT)
    vm = VM('build/fakeos.elf', mem='256M', extra_args=['-smp', str(args.smp)])
    transcript = ''

    def wait(marker, timeout=90):
        nonlocal transcript
        out = vm.wait_for(marker, timeout)
        transcript += out
        assert marker in out, 'timeout waiting for ' + repr(marker)
        return out

    def command(text, marker, timeout=60):
        vm.send(text)
        out = wait('\r\n' + marker + '\r\n', timeout)
        return out

    try:
        wait('fakeos login:', 120)
        vm.send('root')
        wait('Password:', 30)
        vm.send('root')
        wait('fakeos:', 30)
        if 'interactive shell still running after 30 s monitor window' not in transcript:
            wait('interactive shell still running after 30 s monitor window')
        if 'System idle. Yielding for IRQ-driven TTY/net I/O.' not in transcript:
            wait('System idle. Yielding for IRQ-driven TTY/net I/O.')
        assert '[U][PASS] /proc pids meminfo' in transcript, \
            'missing init /proc pids meminfo selftest'
        assert '[U][PASS] /proc/self status/maps/cmdline' in transcript

        out = command('ps; echo PS_BASE_END', 'PS_BASE_END')
        assert re.search(r'(?m)^PID\tPPID\tSTAT\tCOMMAND\r?$', out), out
        # After login, pid 1 is the shell (init execve'd login then sh).
        assert re.search(r'(?m)^1\t0\t[RSTSZ?]\t.*sh', out), out

        out = command('cat /proc/self/status; echo STATUS_END', 'STATUS_END')
        assert 'Name:' in out and 'Pid:' in out and 'PPid:' in out, out

        out = command('cat /proc/meminfo; echo MEMINFO_END', 'MEMINFO_END')
        assert 'MemTotal:' in out and 'MemFree:' in out, out
        m = re.search(r'MemTotal:\t(\d+) kB', out)
        assert m and int(m.group(1)) > 0, out

        out = command('free; echo FREE_END', 'FREE_END')
        assert 'Mem:' in out and 'kB' in out, out
        assert 'free: MemTotal missing' not in out
        assert 'free: MemFree exceeds' not in out

        vm.send('/bin/sleep 20 &')
        job = wait('[1] ', 20)
        m = re.search(r'\[1\] (\d+)', job)
        assert m, 'background sleep did not print [1] pid: ' + job
        job_pid = m.group(1)
        transcript += vm.read_available(1)

        out = command('ps; echo PS_BG_END', 'PS_BG_END')
        # The job id printed by the shell may be a wrapper /bin/sh;
        # the program we launched must itself appear in the table.
        sm = re.search(r'(?m)^(\d+)\t(\d+)\t[RSTSZ?]\t/bin/sleep(?: |\r|$)', out)
        assert sm, 'ps did not list /bin/sleep after background start:\n' + out
        sleep_pid = sm.group(1)
        assert re.search(r'(?m)^' + re.escape(job_pid) + r'\t', out), out

        # Kill the sleeper, let the shell reap it, confirm it leaves the table.
        command('kill ' + sleep_pid + '; wait; echo KILL_END', 'KILL_END')
        out = command('ps; echo PS_AFTER_END', 'PS_AFTER_END')
        assert not re.search(r'(?m)^\d+\t.+\t/bin/sleep', out), out
        assert not re.search(r'(?m)^' + re.escape(sleep_pid) + r'\t', out), out

        print('PASS: daily /proc + ps + free login workflow; smp=' + str(args.smp))
    finally:
        Path(args.log).write_text(transcript, errors='surrogateescape')
        vm.close()
        print('Serial log: ' + args.log)


if __name__ == '__main__':
    main()
