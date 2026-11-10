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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--smp', type=int, default=1)
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
        # Startup signal selftests can interfere with early foreground execs.
        if 'interactive shell still running after 30 s monitor window' not in transcript:
            wait('interactive shell still running after 30 s monitor window')
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
        print('PASS: guest C compile/exec, provenance, ELF64 and flat binary; smp=' + str(args.smp))
    finally:
        Path(args.log).write_text(transcript, errors='surrogateescape')
        vm.close()
        print('Serial log: ' + args.log)


if __name__ == '__main__':
    main()
