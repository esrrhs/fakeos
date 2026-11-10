#!/usr/bin/env python3
"""Host-monotonic + real guest clock/sleep/poll/select/EINTR checks; own disk only."""
import argparse,json,os,re,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT/'scripts'));sys.path.insert(0,str(ROOT/'tools'))
from test_qemu import VM
from m5_guest_smoke import verify_process_selftests

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--smp',type=int,default=2);args=ap.parse_args()
    os.chdir(ROOT);work=Path(tempfile.mkdtemp(prefix='fakeos-clock-'));disk=work/'disposable.img'
    subprocess.run([sys.executable,'tools/mkext2.py',str(disk),'4'],check=True)
    print('Disposable clock artifacts:',work,flush=True)
    vm=VM('build/fakeos.elf',mem='256M',extra_args=['-smp',str(args.smp),'-drive',f'id=clocktest,file={disk},format=raw,if=none','-device','virtio-blk-pci,disable-modern=on,drive=clocktest'])
    transcript='';serial=0;results=[]
    def wait(marker,timeout=150):
        nonlocal transcript
        out=vm.wait_for(marker,timeout);transcript+=out;(work/'guest.log').write_text(transcript,errors='surrogateescape')
        assert marker in out,'timeout '+repr(marker);return out
    def run(cmd,status=0):
        nonlocal serial
        serial+=1;tag=f'CLOCK_{serial}';start=time.monotonic()
        vm.send(cmd+'; echo '+tag+':$?; echo '+tag+'_END');out=wait('\r\n'+tag+'_END\r\n');elapsed=time.monotonic()-start
        assert re.search(r'(?m)^'+tag+':'+str(status)+r'\r?$',out),out
        results.append({'command':cmd,'status':status,'host_elapsed_seconds':elapsed,'out':out})
        (work/'timings.json').write_text(json.dumps(results,indent=2)+'\n')
        print(f'PASS smp={args.smp} {cmd} status={status} host={elapsed:.6f}s',flush=True)
        return out,elapsed
    def timed(cmd,seconds,status=0):
        out,elapsed=run(cmd,status)
        # At most20ms early allowance for100Hz calibration/host observation;
        # 3x+0.5s upper budget permits host descheduling, NEVER early2x/4x clocks.
        assert seconds-.02<=elapsed<=seconds*3+.5,(cmd,elapsed,seconds)
        return out,elapsed
    try:
        wait('fakeos login:');vm.send('root');wait('Password:');vm.send('root');wait('fakeos:')
        if 'System idle. Yielding for IRQ-driven TTY/net I/O.' not in transcript:wait('System idle. Yielding for IRQ-driven TTY/net I/O.')
        assert not [line for line in transcript.splitlines() if '[FAIL]' in line or '[FATAL]' in line]
        verify_process_selftests(transcript)
        run('export FAKECC_RTCACHE=0');run('cd /src')
        run('/bin/fakecc --target=x86_64-linux -O0 /src/clock_check.c -o /tmp/clockcheck')
        run('/tmp/clockcheck zero')
        for mode,seconds in (('nano',2),('poll',1),('select',1),('short',.18)):
            out,elapsed=timed('/tmp/clockcheck '+mode,seconds)
            ns=int(re.search(r'CLOCKCHECK '+mode+r' elapsed_ns=(\d+)',out).group(1))
            assert ns>=int(seconds*1e9),(mode,ns)
            if mode!='short':assert .65<=ns/(elapsed*1e9)<=1.15,(mode,ns,elapsed)
        out,_=run('/tmp/clockcheck eintr')
        assert 'CLOCKCHECK EINTR PASS' in out and out.count('reaped=1')==6,out
        run('printf CLOCK > /tmp/clock-before');run('printf CLOCK > /home/clock-before')
        before=[]
        for fs in ('/tmp','/home'):
            out,_=run("/bin/stat -c '%Y' "+fs+'/clock-before');before.append(int(re.search(r'(?m)^(\d+)\r?$',out).group(1)))
        timed('/bin/sleep 2',2)
        for index,fs in enumerate(('/tmp','/home')):
            run('printf CLOCK > '+fs+'/clock-after')
            out,_=run("/bin/stat -c '%Y' "+fs+'/clock-after');after=int(re.search(r'(?m)^(\d+)\r?$',out).group(1))
            assert 2<=after-before[index]<=4,(fs,before[index],after)
        out,_=timed('/bin/time /bin/timeout 2 /bin/sleep 4',2,124)
        assert 1.98<=float(re.search(r'(?m)^real ([0-9.]+)',out).group(1))<2.25,out
        run('/bin/timeout 2 /bin/true')
        out,_=timed('/bin/time /bin/timeout -s15 -k1 2 /tmp/clockcheck ignore',3,124)
        assert 2.98<=float(re.search(r'(?m)^real ([0-9.]+)',out).group(1))<3.3,out
        assert 'CLOCKCHECK IGNORE READY' in out,out
        run('/bin/timeout 9223372036854776 /bin/true',1)
        out,_=run('/bin/ps');assert '/tmp/clockcheck' not in out and '/bin/sleep' not in out,out
        out,elapsed=timed('/bin/time /bin/sleep 1',1)
        assert 1<=float(re.search(r'(?m)^real ([0-9.]+)',out).group(1))<2,out
        run('/bin/sync');assert not [line for line in transcript.splitlines() if '[FAIL]' in line or '[FATAL]' in line]
        print(f'PASS: real clock/nanosleep/poll/select/short waits/EINTR remainder+cleanup/filesystem mtime/sleep/timeout/time smp={args.smp}',flush=True)
        vm.send('exit 0');wait('[UPROC] pid 1 exited (code 0)',60)
    finally:vm.close()
if __name__=='__main__':main()
