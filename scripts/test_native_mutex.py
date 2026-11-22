#!/usr/bin/env python3
"""Native robust mutex probes after boot: kernel-side gate-3 selftest at
boot, then user ABI steps (basics/rights, same-process and cross-process
FIFO handoff with owner-pid proof, kill-while-blocked, owner-death recovery,
poison chain, close-while-locked drain, object-pool baseline)."""
import argparse,json,re,subprocess,tempfile
from pathlib import Path
from test_qemu import VM,disk_args

def main():
 p=argparse.ArgumentParser();p.add_argument('--observe',action='store_true');p.add_argument('--evidence',default='build/native-mutex');p.add_argument('--cpus',default='1,2');a=p.parse_args()
 e=Path(a.evidence);e.mkdir(parents=True,exist_ok=True);rows=[]
 for cpu in map(int,a.cpus.split(',')):
  with tempfile.TemporaryDirectory(prefix='native-mutex-',dir='build') as d:
   disk=str(Path(d)/'disk.img');subprocess.run(['python3','tools/mkext2.py',disk,'32'],check=True)
   vm=VM('build/fakeos.elf',mem='256M',extra_args=disk_args(disk)+['-smp',str(cpu),'-nic','none']);out=''
   try:
    out+=vm.wait_for('[NATIVE] PASS kernel-only object/channel suite',300,failure_markers=('[NATIVE] FAIL','[PANIC]','[EXCEPTION]'))
    assert '[NATIVE] PASS kernel-only object/channel suite' in out
    assert 'native mutex: FIFO handoff' in out, 'mutex selftest sections missing'
    idle='System idle. Yielding for IRQ-driven TTY/net I/O.'
    if idle not in out: out+=vm.wait_for(idle,60,failure_markers=('[PANIC]','[EXCEPTION]'))
    assert idle in out, 'boot-complete barrier missing'
    vm.send('root');out+=vm.wait_for('Password:',10);vm.send('root');out+=vm.wait_for('Welcome root',20)
    vm.send('/bin/init --native-mutex; echo __NM_RC=$?; echo __NM_DONE__')
    out+=vm.wait_for('\r\n__NM_DONE__\r\n',300,failure_markers=('[PANIC]',))
   finally:
    out+=vm.read_available(.2);vm.close();(e/f'smp{cpu}.serial.log').write_text(out)
   steps=re.findall(r'^\[NATIVE-MUTEX\] (step\d+) PASS (.+)$',out,re.M)
   assert len(steps)==8, f'SMP{cpu}: mutex steps incomplete ({len(steps)}/8)'
   rows.append({'cpus':cpu,'steps':[s[0] for s in steps],
                'desired_pass':'[NATIVE-MUTEX] PASS robust mutex: handoff, barging, kill, recovery, poison' in out})
   (e/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
   if not a.observe:
    assert rows[-1]['desired_pass'], f'SMP{cpu}: mutex suite failed'
    assert '__NM_RC=0' in out, f'SMP{cpu}: probe exited nonzero'
    assert '[FAIL]' not in out, f'SMP{cpu}: boot regression marker'
    assert '[WAIT] PASS interruptible admission cases=18 failures=0' in out, f'SMP{cpu}: wait/cancel suite regressed'
   print(f'SMP{cpu} ' +('observed baseline' if a.observe else 'PASS')+f': {len(steps)}/8 steps',flush=True)
if __name__=='__main__':main()
