#!/usr/bin/env python3
"""Focused M5 smoke; does not claim unrelated boot selftests pass."""
import argparse
import os
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from test_qemu import VM


PROCESS_PASSES = (
    '[PASS] pipe kmutex + wait_queue_sleep_unlock blocking',
    '[U][PASS] kill SIGKILL + wait status',
    '[U][PASS] SIGTERM handler + sigreturn',
    '[U][PASS] SIGSTOP/CONT + WUNTRACED/WCONTINUED',
    '[U][PASS] pipe read EINTR + handler',
    '[U][PASS] pipe SA_RESTART keeps read',
    '[U][PASS] orphan reparented to pid 1',
)


def verify_process_selftests(transcript):
    missing = [marker for marker in PROCESS_PASSES if marker not in transcript]
    failures = [line for line in transcript.splitlines()
                if '[FAIL]' in line and re.search(
                    r'pipe|kill|SIG|WUNTRACED|WCONTINUED|SA_RESTART|orphan', line)]
    assert not missing and not failures, 'process selftests: missing=' + repr(missing) + ' failures=' + repr(failures)
    print('PASS: all seven pipe/signal/wait/orphan boot regressions')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--smp', type=int, default=1)
    parser.add_argument('--require-process-selftests', action='store_true',
                        help='require pipe, signal/wait and orphan boot regressions')
    parser.add_argument('--log', default='/tmp/fakeos-m5-guest.log')
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

    def command(text, marker, expected):
        # The echoed command itself must not satisfy the result assertion.
        vm.send(text)
        out = wait('\r\n' + marker + '\r\n', 180)
        assert re.search(r'(?m)^' + re.escape(expected) + r'\r?$', out), out
        return out

    try:
        wait('fakeos login:', 120)
        vm.send('root')
        wait('Password:', 30)
        vm.send('root')
        wait('fakeos:/', 30)
        if args.require_process_selftests:
            command('/bin/true; echo M5_EARLY:$?; echo M5_EARLY_END',
                    'M5_EARLY_END', 'M5_EARLY:0')
        # Keep the expensive compiler smoke separate from boot's monitor window.
        if 'interactive shell still running after 30 s monitor window' not in transcript:
            wait('interactive shell still running after 30 s monitor window')
        # The monitor precedes AP release in kmain. Wait for actual SMP
        # initialization/selftests, not just the shell grace-period marker,
        # so the compiler workload genuinely exercises the online APs.
        if 'System idle. Yielding for IRQ-driven TTY/net I/O.' not in transcript:
            wait('System idle. Yielding for IRQ-driven TTY/net I/O.')
        platform_pass = ('[PASS] AP SYSRET MSRs: skipped (-smp 1)'
                         if args.smp == 1 else
                         '[PASS] AP SYSRET MSRs: STAR/LSTAR/EFER.SCE+NXE/current AS')
        assert platform_pass in transcript, 'missing AP MSR/address-space regression'
        platform_fails = [line for line in transcript.splitlines()
                          if '[FAIL]' in line and re.search(
                              r'SYSRET|STAR/LSTAR|EFER|current AS', line)]
        assert not platform_fails, platform_fails
        assert "[PASS] concurrent buddy/slab allocation canaries" in transcript
        if args.require_process_selftests:
            verify_process_selftests(transcript)
        vm.send('export FAKECC_RTCACHE=0')
        transcript += vm.read_available(1)
        out = command('cd /src; /bin/fakecc --target=x86_64-linux -O0 ping.c -o /tmp/ping && /tmp/ping; echo M5_C:$?; echo M5_C_END',
                      'M5_C_END', 'M5_C:0')
        assert re.search(r'(?m)^pong-from-fakeos-compiler\r?$', out), out
        command('grep source_revision /etc/fakecc.provenance.json; echo M5_PROV:$?; echo M5_PROV_END',
                'M5_PROV_END', 'M5_PROV:0')
        vm.send("printf '%s\\n' 'bits 64' 'global _start' '_start:' '  mov rax, 60' '  xor rdi, rdi' '  syscall' > /tmp/m5.asm")
        transcript += vm.read_available(1)
        command('/bin/fakecc -f elf64 -o /tmp/m5.o /tmp/m5.asm; echo M5_ELF:$?; echo M5_ELF_END',
                'M5_ELF_END', 'M5_ELF:0')
        command('/bin/fakecc -f bin -o /tmp/m5.bin /tmp/m5.asm; echo M5_BIN:$?; echo M5_BIN_END',
                'M5_BIN_END', 'M5_BIN:0')
        failures = [line for line in transcript.splitlines()
                    if '[FAIL]' in line or '[FATAL]' in line]
        assert not failures, failures
        print('PASS: guest C compile/exec, provenance, ELF64 and flat binary; smp=' + str(args.smp))
    finally:
        Path(args.log).write_text(transcript, errors='surrogateescape')
        vm.close()
        print('Serial log: ' + args.log)


if __name__ == '__main__':
    main()
