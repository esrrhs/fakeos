#!/usr/bin/env python3
"""Real Ext2 read errors must not make cross-FS mv unlink originals; private disk only."""
import argparse,json,os,re,struct,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT/'scripts'))
from test_qemu import VM


def inject_faults(disk,work):
    assert disk.parent==work and disk.name=='disposable.img'
    image=bytearray(disk.read_bytes())
    u32=lambda at:struct.unpack_from('<I',image,at)[0]
    u16=lambda at:struct.unpack_from('<H',image,at)[0]
    block=1024<<u32(1048);ipg=u32(1064);isz=u16(1112)
    def inode(ino):
        group,index=divmod(ino-1,ipg)
        return u32(2048+group*32+8)*block+index*isz
    def entry(parent,wanted):
        at=inode(parent)
        for index in range(12):
            data=u32(at+40+index*4)
            if not data:continue
            pos=data*block;end=pos+block
            while pos+8<=end:
                rec=u16(pos+4);assert rec>=8 and pos+rec<=end
                if u32(pos) and bytes(image[pos+8:pos+8+image[pos+6]])==wanted:return u32(pos)
                pos+=rec
        raise AssertionError('missing private fault fixture '+repr(wanted))
    home=entry(2,b'home')
    sources=[entry(home,b'mvsrc'),entry(entry(home,b'mvtree'),b'file')]
    faults=[]
    for source in sources:
        at=inode(source);old=u32(at+44);size=u32(at+4);bad=u32(1028)+7
        assert size==2292 and 0<old<u32(1028)
        faults.append({'inode':source,'size':size,'pointer_at':at+44,'original':old,'invalid':bad})
        struct.pack_into('<I',image,at+44,bad)
    disk.write_bytes(image)
    (work/'injected-faults.json').write_text(json.dumps(faults,indent=2)+'\n')
    return faults


def restore_faults(disk,work,faults):
    assert disk.parent==work and disk.name=='disposable.img'
    image=bytearray(disk.read_bytes())
    for fault in faults:
        assert struct.unpack_from('<I',image,fault['pointer_at'])[0]==fault['invalid']
        struct.pack_into('<I',image,fault['pointer_at'],fault['original'])
    disk.write_bytes(image)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--smp',type=int,default=2);args=ap.parse_args()
    os.chdir(ROOT);work=Path(tempfile.mkdtemp(prefix='fakeos-move-io-'));disk=work/'disposable.img'
    subprocess.run([sys.executable,'tools/mkext2.py',str(disk),'4'],check=True)
    print('Disposable move I/O artifacts:',work,flush=True)
    faults=[]
    for boot in (1,2,3):
        vm=VM('build/fakeos.elf',mem='256M',extra_args=['-smp',str(args.smp),'-drive',f'id=mvtest,file={disk},format=raw,if=none','-device','virtio-blk-pci,disable-modern=on,drive=mvtest'])
        transcript='';serial=0
        def wait(marker,timeout=150):
            nonlocal transcript
            out=vm.wait_for(marker,timeout);transcript+=out
            (work/f'boot{boot}.log').write_text(transcript,errors='surrogateescape')
            assert marker in out,'timeout '+repr(marker)
            return out
        def run(command,status=0):
            nonlocal serial
            serial+=1;tag=f'MOVE_{boot}_{serial}';started=time.monotonic()
            print(f'RUN boot={boot} smp={args.smp} {command}',flush=True)
            vm.send(command+'; echo '+tag+':$?; echo '+tag+'_END')
            out=wait('\r\n'+tag+'_END\r\n')
            assert re.search(r'(?m)^'+tag+':'+str(status)+r'\r?$',out),out
            print(f'PASS {tag} exit={status} elapsed={time.monotonic()-started:.3f}s',flush=True)
            return out
        try:
            wait('fakeos login:');vm.send('root');wait('Password:');vm.send('root');wait('fakeos:')
            if 'System idle. Yielding for IRQ-driven TTY/net I/O.' not in transcript:wait('System idle. Yielding for IRQ-driven TTY/net I/O.')
            assert not [line for line in transcript.splitlines() if '[FAIL]' in line or '[FATAL]' in line]
            if boot==1:
                for command in ['seq 1 600 > /home/mvsrc','/bin/cp /home/mvsrc /home/mvoriginal','/bin/cmp /home/mvsrc /home/mvoriginal','mkdir /home/mvtree','/bin/cp /home/mvoriginal /home/mvtree/file','printf KEEPTREE > /home/mvtree/keep']:
                    run(command)
            elif boot==2:
                out=run('/bin/mv -v /home/mvsrc /tmp/mvdst',1)
                assert 'read failed' in out and "'/home/mvsrc' -> '/tmp/mvdst'" not in out,out
                run('test -e /home/mvsrc')
                out=run('/bin/wc -c /tmp/mvdst');assert re.search(r'(?m)^\s*1024 /tmp/mvdst\r?$',out),out
                run('/bin/cmp /home/mvoriginal /tmp/mvdst',1)
                out=run('/bin/mv -v /home/mvtree /tmp/mvtree',1)
                assert 'read failed' in out and "'/home/mvtree' -> '/tmp/mvtree'" not in out,out
                run('test -e /home/mvtree/file')
                out=run('cat /home/mvtree/keep');assert 'KEEPTREE' in out,out
                run('/bin/cmp /home/mvtree/keep /tmp/mvtree/keep')
                for command in ['/bin/cp /home/mvoriginal /home/mvgood','/bin/mv /home/mvgood /tmp/mvgood','test ! -e /home/mvgood','/bin/cmp /home/mvoriginal /tmp/mvgood','/bin/mv /tmp/mvgood /home/mvback','test ! -e /tmp/mvgood','/bin/cmp /home/mvoriginal /home/mvback']:
                    run(command)
                run('/bin/mv /dev/zero /home/nospecial',1)
                run('test -e /dev/zero');run('test ! -e /home/nospecial')
                # A source directory symlink used to delete OUTSIDE's children
                # while returning1. Links must move verbatim, not be traversed.
                run('mkdir /home/outside');run('printf OUTSIDE > /home/outside/keep')
                run('/bin/ln -s /home/outside /home/dirlink')
                run('/bin/mv /home/dirlink /tmp/dirlink')
                out=run('/bin/readlink /tmp/dirlink');assert re.search(r'(?m)^/home/outside\r?$',out),out
                run('/bin/readlink /home/dirlink',1)
                out=run('cat /home/outside/keep');assert 'OUTSIDE' in out,out
                run('/bin/ln -s /home/mvoriginal /home/filelink')
                run('/bin/mv /home/filelink /tmp/filelink')
                out=run('/bin/readlink /tmp/filelink');assert re.search(r'(?m)^/home/mvoriginal\r?$',out),out
                # Existing VFS dispatch cannot dereference an absolute symlink
                # from Ramfs into Ext2. Preserve and expose that limitation,
                # not a fabricated claim that lossless link moves fixed VFS.
                run('/bin/cmp /home/mvoriginal /tmp/filelink',2)
                run('seq 1 600 > /tmp/linkref')
                run('/bin/cmp /home/mvoriginal /tmp/linkref')
                run('/bin/ln -s /tmp/linkref /home/localfilelink')
                run('/bin/mv /home/localfilelink /tmp/localfilelink')
                run('/bin/cmp /tmp/linkref /tmp/localfilelink')
                run('/bin/readlink /home/filelink',1)
                run('mkdir /home/linktree');run('printf TREE > /home/linktree/local')
                targets={'out':'/home/outside','self':'.','dangling':'nowhere','a':'b','b':'a','same':'local'}
                for name,target in targets.items():run('/bin/ln -s '+target+' /home/linktree/'+name)
                run('/bin/mv /home/linktree /tmp/linktree')
                run('test ! -e /home/linktree')
                for name,target in targets.items():
                    out=run('/bin/readlink /tmp/linktree/'+name)
                    assert re.search(r'(?m)^'+re.escape(target)+r'\r?$',out),out
                out=run('cat /tmp/linktree/local');assert 'TREE' in out,out
                out=run('cat /home/outside/keep');assert 'OUTSIDE' in out,out
                run('/bin/cmp /home/outside/keep /tmp/linktree/out/keep',2)
                run('/bin/cmp /tmp/linktree/local /tmp/linktree/same')
                run('/bin/ln -s /home/outside /tmp/revlink')
                run('/bin/mv /tmp/revlink /home/revlink')
                out=run('/bin/readlink /home/revlink');assert re.search(r'(?m)^/home/outside\r?$',out),out
                out=run('cat /home/revlink/keep');assert 'OUTSIDE' in out,out
                run('/bin/ln -s retained /home/linksave')
                run('/bin/ln -s missing /tmp/dangdest')
                run('/bin/mv -n /home/linksave /tmp/dangdest')
                run('/bin/mv /home/linksave /tmp/dangdest',1)
                out=run('/bin/readlink /home/linksave');assert re.search(r'(?m)^retained\r?$',out),out
                out=run('/bin/readlink /tmp/dangdest');assert re.search(r'(?m)^missing\r?$',out),out
                run('/bin/mv /home/linksave /no-such-parent/dest',1)
                out=run('/bin/readlink /home/linksave');assert re.search(r'(?m)^retained\r?$',out),out
                out=run('cat /home/outside/keep');assert 'OUTSIDE' in out,out

                # Refuse lexical, symlinked, relative and mount-alias
                # descendants BEFORE creating anything or calling rename.
                before=run('pwd');cwd=re.search(r'(?m)^/[^\r\n]*',before).group(0)
                for base,alias in (('/home/dsafe','/home/dalias'),('/tmp/dsafe','/tmp/dalias')):
                    run('mkdir '+base);run('mkdir '+base+'/sub')
                    run('printf SAFE > '+base+'/keep')
                    run('/bin/ln -s '+base+'/sub '+alias)
                    for destination in (base+'/sub/direct',alias+'/viaalias'):
                        run('/bin/mv '+base+' '+destination,1)
                    run('test ! -e '+base+'/sub/direct')
                    run('test ! -e '+base+'/sub/viaalias')
                    out=run('cat '+base+'/keep');assert 'SAFE' in out,out
                run('/bin/mv /home/dsafe /mnt/home/dsafe/sub/mountdup',1)
                run('test ! -e /home/dsafe/sub/mountdup')
                out=run('pwd');assert re.search(r'(?m)^'+re.escape(cwd)+r'\r?$',out),out
                run('cd /home');run('/bin/mv dsafe dalias/relative',1)
                run('test ! -e dsafe/sub/relative');run('cd '+cwd)
                # Failed rename is not permission to merge a pre-existing tree.
                run('mkdir /home/mgsrc');run('printf SOURCE > /home/mgsrc/shared')
                run('mkdir /tmp/mgdst');run('mkdir /tmp/mgdst/mgsrc')
                run('printf DEST > /tmp/mgdst/mgsrc/shared')
                run('/bin/mv /home/mgsrc /tmp/mgdst',1)
                out=run('cat /home/mgsrc/shared');assert 'SOURCE' in out,out
                out=run('cat /tmp/mgdst/mgsrc/shared');assert 'DEST' in out and 'SOURCE' not in out,out
                # Native backends treat same-inode aliases as no-op success.
                for base in ('/home/ifile','/tmp/ifile'):
                    run('printf IDENTITY > '+base);run('/bin/ln '+base+' '+base+'hard')
                    run('/bin/mv '+base+' '+base+'hard')
                    run('/bin/cmp '+base+' '+base+'hard')
                    out=run('cat '+base);assert 'IDENTITY' in out,out
                run('/bin/mv /home/ifile /mnt/home/ifile')
                out=run('cat /home/ifile');assert 'IDENTITY' in out,out
            else:
                # Restore only the original pointers offline. The failed move
                # must have retained the SAME inode/data for actual recovery.
                for command in ['/bin/cmp /home/mvsrc /home/mvoriginal','/bin/cmp /home/mvtree/file /home/mvoriginal','/bin/mv /home/mvsrc /tmp/recovered','test ! -e /home/mvsrc','/bin/cmp /home/mvoriginal /tmp/recovered','/bin/mv /home/mvtree /tmp/recoveredtree','test ! -e /home/mvtree','/bin/cmp /home/mvoriginal /tmp/recoveredtree/file']:
                    run(command)
                out=run('cat /tmp/recoveredtree/keep');assert 'KEEPTREE' in out,out
            run('/bin/sync')
            assert not [line for line in transcript.splitlines() if '[FAIL]' in line or '[FATAL]' in line]
            print(f'PASS: move I/O retention/recovery boot={boot} smp={args.smp}',flush=True)
            vm.send('exit 0');wait('[UPROC] pid 1 exited (code 0)',60)
        finally:vm.close()
        if boot==1:faults=inject_faults(disk,work)
        elif boot==2:restore_faults(disk,work,faults)
    print('PASS: real partial Ext2 read failure retains file and entire source tree; offline repair/relaunch proves original full bytes; good moves work both directions',flush=True)
if __name__=='__main__':main()
