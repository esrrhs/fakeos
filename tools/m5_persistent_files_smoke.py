#!/usr/bin/env python3
"""Two-boot /home acceptance, always on a newly created disposable Ext2 disk.

No existing image argument is accepted. Logs and the test disk remain in the
printed unique temporary directory. Relaunch after guest sync + shell exit;
this is not a power-loss or guest reboot(2) test.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from test_qemu import VM


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--smp', type=int, default=2)
    args = ap.parse_args()
    os.chdir(ROOT)
    work = Path(tempfile.mkdtemp(prefix='fakeos-persistent-'))
    disk = work / 'disposable.img'
    subprocess.run([sys.executable, 'tools/mkext2.py', str(disk), '4'], check=True)
    print('Disposable test artifacts:', work, flush=True)
    metadata = None
    for boot in (1, 2):
        vm = VM('build/fakeos.elf', mem='256M', extra_args=[
            '-smp', str(args.smp), '-drive',
            f'id=testdisk,file={disk},format=raw,if=none',
            '-device', 'virtio-blk-pci,disable-modern=on,drive=testdisk'])
        transcript = ''
        serial = 0

        def wait(marker, timeout=120):
            nonlocal transcript
            out = vm.wait_for(marker, timeout)
            transcript += out
            assert marker in out, 'timeout: ' + repr(marker)
            return out

        def run(cmd, status=0):
            nonlocal serial
            serial += 1
            tag = f'FILES_{boot}_{serial}'
            vm.send(cmd + '; echo ' + tag + ':$?; echo ' + tag + '_END')
            out = wait('\r\n' + tag + '_END\r\n', 60)
            assert re.search(r'(?m)^' + tag + ':' + str(status) + r'\r?$', out), out
            return out

        try:
            wait('fakeos login:')
            vm.send('root')
            wait('Password:', 30)
            vm.send('root')
            wait('fakeos:', 30)
            if 'System idle. Yielding for IRQ-driven TTY/net I/O.' not in transcript:
                wait('System idle. Yielding for IRQ-driven TTY/net I/O.')
            assert '[U] cwd -> /home (persistent Ext2)' in transcript
            assert '[PASS] concurrent buddy/slab allocation canaries' in transcript
            if boot == 1:
                run('echo RAM_ONLY > /tmp/boot-only')
            else:
                run('test ! -e /tmp/boot-only')
            # Recreate the comparison data in RAM on each boot (not on disk).
            run("printf 'alpha\\nbeta\\ngamma\\n' > /tmp/expected")
            if boot == 1:
                run('mkdir -p /home/daily/nested')
                run("printf 'alpha\\nbeta\\n' > /home/daily/nested/note")
                run("printf 'a\\ngamma\\n.\\nw\\nq\\n' | /bin/ed /home/daily/nested/note")
                run('cmp /tmp/expected /home/daily/nested/note')
                run('/bin/cp /home/daily/nested/note /home/daily/copy')
                run('mv /home/daily/copy /home/daily/renamed')
                run('cmp /tmp/expected /home/daily/renamed')
                run('echo discard > /home/daily/trash')
                run('rm /home/daily/trash')
                run('/bin/cp -r /home/daily/nested /home/daily/tree')
                run('cmp /tmp/expected /home/daily/tree/note')
                run("printf 'echo PERSIST_SCRIPT_OK\\n' > /home/daily/check.sh")
                run('chmod 750 /home/daily/check.sh')
                run('chmod 640 /home/daily/renamed')
                # More than 12 ext2 blocks: exercise single-indirect storage.
                run('seq 1 5000 > /tmp/large')
                run('/bin/cp /tmp/large /home/daily/large')
                run('cmp /tmp/large /home/daily/large')
            for name in ('nested/note', 'renamed', 'tree/note'):
                run('cmp /tmp/expected /home/daily/' + name)
            run('seq 1 5000 > /tmp/large')
            run('cmp /tmp/large /home/daily/large')
            run('test ! -e /home/daily/trash')
            out = run('. /home/daily/check.sh')
            assert re.search(r'(?m)^PERSIST_SCRIPT_OK\r?$', out), out
            out = run("/bin/stat -c '%a:%s:%Y' /home/daily/renamed")
            match = re.search(r'(?m)^(640:17:[0-9]+)\r?$', out)
            assert match, out
            if boot == 1:
                metadata = match.group(1)
            else:
                assert match.group(1) == metadata, (metadata, match.group(1))
            out = run("/bin/stat -c '%a' /home/daily/check.sh")
            assert re.search(r'(?m)^750\r?$', out), out
            # Repeated immediate send after result markers exposed SMP RX
            # ring races (e.g. "seq" arrived as "eq"). Assert byte order,
            # not just the final shell status, without retries or delays.
            for i in range(32):
                token = f'RX_ORDER_{boot}_{i}_abcdefghijklmnopqrstuvwxyz_0123456789'
                out = run('echo ' + token)
                assert re.search(r'(?m)^' + token + r'\r?$', out), out
            run('sync')
            vm.send('exit 0')
            wait('[UPROC] pid 1 exited (code 0)', 30)
            failures = [line for line in transcript.splitlines() if '[FAIL]' in line]
            assert not failures, failures
            print(f'PASS: boot {boot} file workflow, metadata and sync', flush=True)
        finally:
            (work / f'boot{boot}.log').write_text(transcript, errors='surrogateescape')
            vm.close()
    print('PASS: create/edit/copy/rename/delete/tree/source + permissions/mtime survive relaunch')


if __name__ == '__main__':
    main()
