#!/usr/bin/env python3
"""Real shell Ramfs growth through its declared 16 MiB limit; no disk attached."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from test_qemu import VM

MIB = 1024 * 1024


def expected_hash(size):
    # Independent expected bytes: HEAD, zeros to offset 4 MiB, TAIL, zeros.
    h = hashlib.sha256()
    h.update(b'HEAD')
    h.update(bytes(4 * MIB - 4))
    if size > 4 * MIB:
        h.update(b'TAIL'[:min(4, size - 4 * MIB)])
    if size > 4 * MIB + 4:
        h.update(bytes(size - 4 * MIB - 4))
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--smp', type=int, default=1)
    args = parser.parse_args()
    os.chdir(ROOT)
    work = Path(tempfile.mkdtemp(prefix='fakeos-ramfs-growth-'))
    print('Ramfs growth artifacts:', work, flush=True)
    vm = VM('build/fakeos.elf', mem='256M', extra_args=['-smp', str(args.smp)])
    transcript = ''
    serial = 0

    def wait(marker, timeout=150):
        nonlocal transcript
        out = vm.wait_for(marker, timeout)
        transcript += out
        assert marker in out, 'timeout: ' + repr(marker)
        return out

    def run(cmd, status=0):
        nonlocal serial
        serial += 1
        tag = 'GROW_' + str(serial)
        vm.send(cmd + '; echo ' + tag + ':$?; echo ' + tag + '_END')
        out = wait('\r\n' + tag + '_END\r\n')
        match = re.search(r'(?m)^' + tag + r':(\d+)\r?$', out)
        assert match, out
        if status is not None:
            assert int(match.group(1)) == status, out
        return out

    def check(size):
        out = run("/bin/stat -c '%s' /tmp/grow")
        assert re.search(r'(?m)^' + str(size) + r'\r?$', out), out
        out = run('/bin/sha256sum /tmp/grow')
        assert re.search(r'(?m)^' + expected_hash(size) + r'\s', out), out

    try:
        wait('fakeos login:')
        vm.send('root')
        wait('Password:')
        vm.send('root')
        wait('fakeos:')
        if 'System idle. Yielding for IRQ-driven TTY/net I/O.' not in transcript:
            wait('System idle. Yielding for IRQ-driven TTY/net I/O.')
        assert not [s for s in transcript.splitlines() if '[FAIL]' in s or '[FATAL]' in s]
        assert '[PASS] Large kmalloc: 8/16 MiB canaries, limits and exact reclamation' in transcript
        run('dd if=/dev/zero of=/tmp/grow bs=8192 count=512 status=none')
        run("printf HEAD | dd of=/tmp/grow bs=1 conv=notrunc status=none")
        check(4 * MIB)
        # Actual write, not just metadata: previously failed at 4 MiB.
        run("printf TAIL | dd of=/tmp/grow bs=4 seek=1048576 conv=notrunc status=none")
        check(4 * MIB + 4)
        for size in (5 * MIB, 8 * MIB, 9 * MIB, 16 * MIB):
            run('/bin/truncate -s ' + str(size) + ' /tmp/grow')
            check(size)
        # Declared-size failure must leave size/content intact.
        run('/bin/truncate -s 16777217 /tmp/grow', 1)
        run('dd if=/dev/zero of=/tmp/grow bs=1 count=1 seek=16777216 conv=notrunc status=none', 1)
        check(16 * MIB)
        run('/bin/cp /tmp/grow /tmp/grow-copy')
        run('cmp /tmp/grow /tmp/grow-copy')
        run('/bin/truncate -s 4194308 /tmp/grow')
        check(4 * MIB + 4)
        run('/bin/truncate -s 16777216 /tmp/grow')
        run('cmp /tmp/grow /tmp/grow-copy')
        run('/bin/rm /tmp/grow /tmp/grow-copy')
        run('test ! -e /tmp/grow')
        run('test ! -e /tmp/grow-copy')
        # Real allocation-pressure failure, distinct from FILE_MAX rejection.
        # Keep an existing 4 MiB file live while exhausting 16 MiB blocks in
        # this private 256 MiB VM; no disks or shared files are attached.
        run('dd if=/dev/zero of=/tmp/retain bs=8192 count=512 status=none')
        run('printf HEAD | dd of=/tmp/retain bs=1 conv=notrunc status=none')

        def check_retained(size):
            out = run("/bin/stat -c '%s' /tmp/retain")
            assert re.search(r'(?m)^' + str(size) + r'\r?$', out), out
            out = run('/bin/sha256sum /tmp/retain')
            digest = hashlib.sha256(b'HEAD' + bytes(size - 4)).hexdigest()
            assert re.search(r'(?m)^' + digest + r'\s', out), out

        check_retained(4 * MIB)
        fillers = []
        for i in range(32):
            path = '/tmp/fill' + str(i)
            out = run('/bin/truncate -s 16777216 ' + path, status=None)
            code = int(re.search(r'(?m)^GROW_\d+:(\d+)\r?$', out).group(1))
            if code:
                assert code == 1, out
                out = run("/bin/stat -c '%s' " + path)
                assert re.search(r'(?m)^0\r?$', out), out
                run('/bin/rm ' + path)
                break
            fillers.append(path)
        else:
            raise AssertionError('private VM never reached 16 MiB allocation pressure')
        assert fillers, 'no 16 MiB allocation succeeded before pressure'
        print('Real 16 MiB allocation-pressure failure after ' + str(len(fillers)) + ' live filler files', flush=True)
        run('/bin/truncate -s 9437184 /tmp/retain', 1)
        check_retained(4 * MIB)
        run('printf X | dd of=/tmp/retain bs=1 seek=8388608 conv=notrunc status=none', 1)
        check_retained(4 * MIB)
        # Reclaim one full block and require the very same failed growth to
        # succeed, retaining original bytes and zero-filling the extension.
        run('/bin/rm ' + fillers.pop())
        run('/bin/truncate -s 9437184 /tmp/retain')
        check_retained(9 * MIB)
        # exec currently permits at most 8 argv slots, including argv[0].
        # Use individual checked removals rather than an over-cap cleanup.
        run('/bin/rm /tmp/retain')
        run('test ! -e /tmp/retain')
        for path in fillers:
            run('/bin/rm ' + path)
            run('test ! -e ' + path)
        assert not [s for s in transcript.splitlines() if '[FAIL]' in s or '[FATAL]' in s]
        assert '[PASS] Large kmalloc: 8/16 MiB canaries, limits and exact reclamation' in transcript
        print('PASS: Ramfs write/zero-grow/copy/shrink/regrow/delete through 16 MiB, full-byte hashes and retained data on limit AND real allocation-pressure errors, recovery after reclamation; smp=' + str(args.smp), flush=True)
    finally:
        (work / 'guest.log').write_text(transcript, errors='surrogateescape')
        vm.close()


if __name__ == '__main__':
    main()
