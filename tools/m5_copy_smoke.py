#!/usr/bin/env python3
"""Safe-copy guest acceptance; always creates a new disposable 4 MiB disk."""
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
    work = Path(tempfile.mkdtemp(prefix='fakeos-copy-'))
    disk = work / 'disposable.img'
    subprocess.run([sys.executable, 'tools/mkext2.py', str(disk), '4'], check=True)
    print('Disposable copy artifacts:', work, flush=True)
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

    def run(cmd, status=0, timeout=90):
        nonlocal serial
        serial += 1
        tag = f'COPY_{serial}'
        vm.send(cmd + '; echo ' + tag + ':$?; echo ' + tag + '_END')
        out = wait('\r\n' + tag + '_END\r\n', timeout)
        assert re.search(r'(?m)^' + tag + ':' + str(status) + r'\r?$', out), out
        return out

    try:
        wait('fakeos login:')
        vm.send('root'); wait('Password:', 30)
        vm.send('root'); wait('fakeos:', 30)
        if 'System idle. Yielding for IRQ-driven TTY/net I/O.' not in transcript:
            wait('System idle. Yielding for IRQ-driven TTY/net I/O.')
        assert '[U] cwd -> /home (persistent Ext2)' in transcript
        run("printf 'keep-me\\n' > /tmp/expected")
        run("printf 'do-not-replace\\n' > /tmp/other")
        for base in ('/tmp/copy', '/home/copy'):
            run('mkdir -p ' + base + '/sub')
            run('/bin/cp /tmp/expected ' + base + '/file')
            run('ln ' + base + '/file ' + base + '/hard')
            run('ln -s ' + base + '/file ' + base + '/sym')
            for src, dst in ((base+'/file', base+'/file'),
                             (base+'/file', base+'/./file'),
                             (base+'/file', base+'/sub/../file'),
                             (base+'/file', base+'/hard'),
                             (base+'/file', base+'/sym'),
                             (base+'/sym', base+'/file'),
                             (base+'/hard', base+'/file'),
                             (base+'/file', base)):
                out = run('/bin/cp ' + src + ' ' + dst, 1)
                assert 'are the same file' in out, out
                run('cmp /tmp/expected ' + base + '/file')
            run('/bin/cp -n ' + base + '/file ' + base + '/file')
            run('/bin/cp /tmp/other ' + base + '/target')
            run('/bin/cp -n ' + base + '/file ' + base + '/target')
            run('cmp /tmp/other ' + base + '/target')
            run('/bin/cp ' + base + '/file ' + base + '/target')
            run('cmp /tmp/expected ' + base + '/target')
            run('ln -s ' + base + '/absent ' + base + '/dangling')
            run('/bin/cp -n ' + base + '/file ' + base + '/dangling')
            run('test -L ' + base + '/dangling')
            run('test ! -e ' + base + '/absent')
            run('/bin/cp ' + base + '/missing ' + base + '/file', 1)
            run('/bin/cp ' + base + '/file ' + base + '/no-dir/target', 1)
            run('cmp /tmp/expected ' + base + '/file')
        for path, dev in (('/tmp/copy/file', 1), ('/home/copy/file', 2)):
            out = run("/bin/stat -c '%d' " + path)
            assert re.search(r'(?m)^' + str(dev) + r'\r?$', out), out
        # Cross-device identity is not just an inode number: copying in both
        # directions remains possible even when namespaces reuse inode values.
        run('/bin/cp /home/copy/file /tmp/from-disk')
        run('cmp /tmp/expected /tmp/from-disk')
        run('/bin/cp /tmp/from-disk /home/copy/from-ram')
        run('cmp /tmp/expected /home/copy/from-ram')
        # Genuine write error on an isolated disk, not a stubbed success path.
        # Ramfs contiguous growth currently fails above ~2 MiB. Keep the
        # source below that independent limit; two successful filler copies
        # leave insufficient space for the third. The tested failure must be
        # the destination's real ENOSPC, never a failed source setup.
        run('dd if=/dev/zero of=/tmp/oversize bs=8192 count=192 status=none', timeout=180)
        out = run("/bin/stat -c '%s' /tmp/oversize")
        assert re.search(r'(?m)^1572864\r?$', out), out
        run('/bin/cp /tmp/oversize /home/filler-a', timeout=180)
        run('/bin/cp /tmp/oversize /home/filler-b', timeout=180)
        out = run('/bin/cp -v /tmp/oversize /home/no-space', 1, timeout=180)
        assert 'write failed' in out, out
        assert not re.search(r"(?m)^'.*' -> '", out), out
        run('test -s /home/no-space')
        run('cmp /tmp/oversize /home/no-space', 1)
        run('rm /home/no-space /home/filler-a /home/filler-b')
        run('/bin/cp /tmp/expected /home/after-error')
        run('cmp /tmp/expected /home/after-error')
        run('cmp /tmp/expected /home/copy/file')
        run('sync')
        assert not [s for s in transcript.splitlines() if '[FAIL]' in s or '[FATAL]' in s]
        print('PASS: same-file/hardlink/symlink aliases, no-clobber, overwrite, cross-FS, ENOSPC + recovery; smp=' + str(args.smp), flush=True)
    finally:
        (work / 'guest.log').write_text(transcript, errors='surrogateescape')
        vm.close()


if __name__ == '__main__':
    main()
