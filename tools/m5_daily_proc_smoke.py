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


def wait_job_pid(wait):
    """Consume a complete job notice, including a UART-split CR/LF pair."""
    notice = wait('[1] ', 20)
    if not re.search(r'\[1\] \d+\r?\n', notice):
        # The previous read may already end in CR. Waiting for a fresh CRLF
        # would skip its LF and block until an unrelated later output line
        # (for sleep, until it exits), invalidating the live-process snapshot.
        notice += wait('\n', 20)
    match = re.search(r'\[1\] (\d+)\r?\n', notice)
    assert match, 'background job did not print [1] pid: ' + notice
    return match.group(1)


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

        # Default termios permits background writes. This also catches a
        # fast exec writing before its parent's foreground handoff.
        vm.send("/bin/printf 'BACKGROUND_WRITE\\n' &")
        wait('[1] ', 20)
        out = command('wait; echo BG_WRITE_END', 'BG_WRITE_END')
        assert re.search(r'(?m)^BACKGROUND_WRITE\r?$', transcript), out

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
        job_pid = wait_job_pid(wait)
        transcript += vm.read_available(1)

        out = command('ps; echo PS_BG_END', 'PS_BG_END')
        # The job id printed by the shell may be a wrapper /bin/sh;
        # the program we launched must itself appear in the table.
        sm = re.search(r'(?m)^(\d+)\t(\d+)\t[RSTSZ?]\t/bin/sleep(?: |\r|$)', out)
        assert sm, 'ps did not list /bin/sleep after background start:\n' + out
        sleep_pid = sm.group(1)
        assert re.search(r'(?m)^' + re.escape(job_pid) + r'\t', out), out

        out = command('cat /proc/' + sleep_pid + '/maps; echo CHILD_MAPS_END',
                      'CHILD_MAPS_END')
        assert '[stack]' in out, out

        # Kill the sleeper, let the shell reap it, confirm it leaves the table.
        command('kill ' + sleep_pid + '; wait; echo KILL_END', 'KILL_END')
        out = command('ps; echo PS_AFTER_END', 'PS_AFTER_END')
        assert not re.search(r'(?m)^\d+\t.+\t/bin/sleep', out), out
        assert not re.search(r'(?m)^' + re.escape(sleep_pid) + r'\t', out), out

        # Race ps against background fork/exec/exit, then wait for cleanup.
        # More than MAX_THREADS worth of launches catches leaked DEAD slots.
        for i in range(20):
            vm.send('/bin/true &')
            churn_pid = wait_job_pid(wait)
            marker = 'CHURN_' + str(i) + '_END'
            command('ps; wait; echo ' + marker, marker)
            marker = 'REAP_' + str(i) + '_END'
            out = command('ps; echo ' + marker, marker)
            assert not re.search(r'(?m)^\d+\t\d+\tZ\t', out), out
            assert not re.search(r'(?m)^' + churn_pid + r'\t', out), out
        failures = [line for line in transcript.splitlines()
                    if '[FAIL]' in line or '[FATAL]' in line]
        assert not failures, failures
        print('PASS: daily /proc + ps + free + 20 exec/exit/reap races; smp=' + str(args.smp))
    finally:
        Path(args.log).write_text(transcript, errors='surrogateescape')
        vm.close()
        print('Serial log: ' + args.log)


if __name__ == '__main__':
    main()
