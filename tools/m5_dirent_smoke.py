#!/usr/bin/env python3
"""Lossless names/record continuation and recursive workflows; private disk, two boots."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import struct
import tempfile
import time
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from test_qemu import VM


def names_for(maximum):
    lengths=[1,27,28,29,30,30,31,31,32,63,127,128,254,255]
    names=[]
    for i,n in enumerate(lengths[:8] if maximum==31 else lengths):
        name='n'*n
        if i in (4,6):name=name[:-1]+'a'
        if i==5:name=name[:-1]+'b'
        names.append(name)
    return names,'d'*maximum


def corrupt_private_directory(disk, work):
    """Offline fault injection in THIS script's new image, never an input path."""
    assert disk.parent == work and disk.name == 'disposable.img'
    image = bytearray(disk.read_bytes())
    u32 = lambda at: struct.unpack_from('<I', image, at)[0]
    u16 = lambda at: struct.unpack_from('<H', image, at)[0]
    block = 1024 << u32(1024 + 24)
    ipg = u32(1024 + 40)
    inode_size = u16(1024 + 88)
    gdt = (2 if block == 1024 else 1) * block
    def inode(ino):
        group, index = divmod(ino - 1, ipg)
        return u32(gdt + group * 32 + 8) * block + index * inode_size
    def entry(ino, wanted):
        node = inode(ino)
        for index in range(12):
            data = u32(node + 40 + 4 * index)
            if not data: continue
            off = data * block
            end = off + block
            while off + 8 <= end:
                size = u16(off + 4)
                assert size >= 8 and off + size <= end
                name = bytes(image[off + 8:off + 8 + image[off + 6]])
                if u32(off) and name == wanted: return off
                off += size
        raise AssertionError('missing private corruption fixture ' + repr(wanted))
    home_inode = u32(entry(2, b'home'))
    bad_inode = u32(entry(home_inode, b'bad'))
    at = entry(bad_inode, b'broken')
    struct.pack_into('<H', image, at + 4, 0)
    disk.write_bytes(image)
    (work / 'injected-fault.txt').write_text('offline directory record reclen=0 at byte ' + str(at + 4) + '\n')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--smp',type=int,default=2)
    args=ap.parse_args()
    os.chdir(ROOT)
    work=Path(tempfile.mkdtemp(prefix='fakeos-dirents-'))
    disk=work/'disposable.img'
    subprocess.run([sys.executable,'tools/mkext2.py',str(disk),'16'],check=True)
    print('Disposable dirent artifacts:',work,flush=True)
    for boot in (1,2):
        vm=VM('build/fakeos.elf',mem='256M',extra_args=['-smp',str(args.smp),'-drive',f'id=testdisk,file={disk},format=raw,if=none','-device','virtio-blk-pci,disable-modern=on,drive=testdisk'])
        transcript='';serial=0
        def wait(marker,timeout=150):
            nonlocal transcript
            out=vm.wait_for(marker,timeout);transcript+=out
            # Preserve complete progress even if a later long command or the
            # orchestrator is interrupted; never wait for finally to save UART.
            (work/f'boot{boot}.log').write_text(transcript,errors='surrogateescape')
            assert marker in out,'timeout '+repr(marker)
            return out
        def run(cmd,status=0):
            nonlocal serial
            serial+=1;tag=f'DENTS_{boot}_{serial}'
            started=time.monotonic()
            print(f'RUN boot={boot} smp={args.smp} command={cmd}',flush=True)
            vm.send(cmd+'; echo '+tag+':$?; echo '+tag+'_END')
            out=wait('\r\n'+tag+'_END\r\n')
            assert re.search(r'(?m)^'+tag+':'+str(status)+r'\r?$',out),out
            print(f'PASS {tag} exit={status} elapsed={time.monotonic()-started:.3f}s',flush=True)
            return out
        def check(base,maximum,limit=None):
            out=run('/tmp/dentcheck check '+base+' '+str(maximum)+((' '+str(limit)) if limit else ''))
            assert re.search(r'(?m)^DENTCHECK PASS '+re.escape(base)+' max='+str(maximum)+r'\r?$',out),out
        def listing(base,maximum):
            files,folder=names_for(maximum)
            out=run('/bin/ls -1 '+base)
            listed=[line.rstrip('\r') for line in out.splitlines() if re.fullmatch(r'[ndab]+\r?',line)]
            assert sorted(listed)==sorted(files+[folder]),listed
            out=run('/bin/find '+base+' -type f')
            actual=[line.rstrip('\r') for line in out.splitlines() if line.startswith(base+'/')]
            children=names_for(31)[0][3:8]
            expected=[base+'/'+n for n in files]+[base+'/'+folder+'/'+n for n in children]
            assert sorted(actual)==sorted(expected),(actual,expected)
            out=run('/bin/du -a '+base)
            for path in expected:assert re.search(r'(?m)^\d+\s+'+re.escape(path)+r'\r?$',out),path
        try:
            wait('fakeos login:');vm.send('root');wait('Password:');vm.send('root');wait('fakeos:')
            if 'System idle. Yielding for IRQ-driven TTY/net I/O.' not in transcript:wait('System idle. Yielding for IRQ-driven TTY/net I/O.')
            assert not [line for line in transcript.splitlines() if '[FAIL]' in line or '[FATAL]' in line]
            run('export FAKECC_RTCACHE=0')
            run('cd /src')
            run('/bin/fakecc --target=x86_64-linux -O0 /src/dirent_check.c -o /tmp/dentcheck')
            if boot==1:
                run('echo RAM_ONLY > /tmp/ram-only')
                run('mkdir /home/bad')
                run('printf RETAIN > /home/bad/keep')
                run('printf BROKEN > /home/bad/broken')
                run('mkdir /home/names')
                run('/tmp/dentcheck seed /home/names 255')
                run('/bin/cp -r /home/names /home/saved')
            else:
                run('test ! -e /tmp/ram-only')
                # The offline malformed record follows valid entries. Kernel
                # partial reads must preserve a later error, not turn it to EOF.
                for command in ('/bin/cp -r /home/bad /home/badcopy',
                                '/bin/ls -1 /home/bad',
                                '/bin/find /home/bad',
                                '/bin/du /home/bad',
                                '/bin/rm -r /home/bad'):
                    run(command,1)
                out=run('cat /home/bad/keep')
                assert 'RETAIN' in out,out
            check('/home/names',255);check('/home/saved',255)
            run('mkdir /tmp/names');run('/tmp/dentcheck seed /tmp/names 31')
            for base,maximum,clone in (('/tmp/names',31,'/tmp/clone'),('/home/names',255,'/home/clone')):
                listing(base,maximum)
                run('/bin/chmod -R 755 '+base)
                run('chmod -R 755 '+base)
                check(base,maximum)
                run('/bin/cp -r '+base+' '+clone)
                check(clone,maximum);listing(clone,maximum)
                run('/bin/rm -r '+clone);run('test ! -e '+clone)
            run('/bin/cp -r /tmp/names /home/fromram');check('/home/fromram',31,255)
            run('/bin/cp -r /home/names /tmp/unsupported',1)
            # Ext2 names exceeding Ramfs31 must fail, never replace the valid
            # 29-byte prefix with another entry's bytes or report full success.
            prefix='n'*29
            run('cmp /home/names/'+prefix+' /tmp/unsupported/'+prefix)
            check('/home/names',255)
            run('/bin/rm -r /tmp/unsupported')
            run('/bin/mv /tmp/names /home/moved')
            run('test ! -e /tmp/names');check('/home/moved',31,255)
            run('/bin/rm -r /home/moved /home/fromram')
            # Maximal syscall paths and deep walking, without oversized argv.
            # 16MiB private image supplies512 inodes; other ENOSPC suites retain
            # their original4MiB capacity fixture unchanged.
            run('/tmp/dentcheck deep-seed /home/dee 250')
            leaf='/home/dee'+'/a'*250+'/f'
            out=run('/bin/find /home/dee -type f')
            assert re.search(r'(?m)^'+re.escape(leaf)+r'\r?$',out),out
            out=run('/bin/du -a /home/dee')
            assert re.search(r'(?m)^\d+\s+'+re.escape(leaf)+r'\r?$',out),out
            run('/tmp/dentcheck deep-check /home/dee 250')
            run('/bin/rm -r /home/dee')
            run('test ! -e /home/dee')
            run('/tmp/dentcheck path-seed /home/fmt 0')
            tail='/'+'p'*255+'/'+'q'*245
            out=run('/bin/cp -rv /home/fmt /home/out')
            assert "'/home/fmt"+tail+"' -> '/home/out"+tail+"'" in out,out
            run('/tmp/dentcheck path-check /home/out 0')
            for utility in ('/bin/find','/bin/du -a'):
                out=run(utility+' /home/out')
                assert '/home/out'+tail in out,out
            run('/bin/rm -r /home/fmt /home/out')
            run('/bin/sync')
            assert not [line for line in transcript.splitlines() if '[FAIL]' in line or '[FATAL]' in line]
            print(f'PASS: names31/255, colliding prefixes, raw/runtime buffer cookies, listing/find/du/chmod/cp/mv/rm, retained bytes; boot={boot} smp={args.smp}',flush=True)
            vm.send('exit 0');wait('[UPROC] pid 1 exited (code 0)',60)
        finally:
            (work/f'boot{boot}.log').write_text(transcript,errors='surrogateescape')
            vm.close()
        if boot == 1:
            corrupt_private_directory(disk,work)
    print('PASS: maximum Ext2 names and copied bytes survive ordered sync/relaunch on the same disposable disk',flush=True)

if __name__=='__main__':main()
