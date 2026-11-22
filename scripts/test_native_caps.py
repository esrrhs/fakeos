#!/usr/bin/env python3
"""User capability-namespace probes after boot: same-process threads,
separate processes over fork-inherited endpoints, isolation, and lifecycle."""
import argparse,json,re,subprocess,tempfile
from pathlib import Path
from test_qemu import VM,disk_args

def main():
 p=argparse.ArgumentParser();p.add_argument('--observe',action='store_true');p.add_argument('--evidence',default='build/native-caps');p.add_argument('--cpus',default='1,2,4');a=p.parse_args()
 e=Path(a.evidence);e.mkdir(parents=True,exist_ok=True);rows=[]
 for cpu in map(int,a.cpus.split(',')):
  with tempfile.TemporaryDirectory(prefix='native-caps-',dir='build') as d:
   disk=str(Path(d)/'disk.img');subprocess.run(['python3','tools/mkext2.py',disk,'32'],check=True)
   vm=VM('build/fakeos.elf',mem='256M',extra_args=disk_args(disk)+['-smp',str(cpu),'-nic','none']);out=''
   try:
    out+=vm.wait_for('[NATIVE] PASS kernel-only object/channel suite',300,failure_markers=('[NATIVE] FAIL','[PANIC]','[EXCEPTION]'))
    assert '[NATIVE] PASS kernel-only object/channel suite' in out
    idle='System idle. Yielding for IRQ-driven TTY/net I/O.'
    # The barrier is printed at the very end of the boot sequence, AFTER the
    # native suite and the slab/VM/wait-cancel self-tests that follow it, so
    # reaching it can take minutes on a loaded host. 30s was not enough and
    # the missing barrier was reported as a boot failure.
    if idle not in out: out+=vm.wait_for(idle,300,failure_markers=('[PANIC]','[EXCEPTION]'))
    assert idle in out, 'boot-complete barrier missing'
    # Login with retries: under host load the serial input processing can lag
    # far behind, so a missed Password prompt must not be read as a kernel
    # failure (same reasoning as test_rlimit.py).
    got_login = False
    for attempt in range(4):
     vm.send('root')
     out+=vm.wait_for('Password:',30,failure_markers=('[PANIC]',))
     if 'Password:' in out:
      got_login = True
      break
    assert got_login, 'login prompt never appeared'
    vm.send('root');out+=vm.wait_for('Welcome root',60)
    vm.send('/bin/init --native-caps; echo __CAPS_RC=$?; echo __CAPS_DONE__')
    out+=vm.wait_for('\r\n__CAPS_DONE__\r\n',150,failure_markers=('[PANIC]',))
   finally:
    out+=vm.read_available(.2);vm.close();(e/f'smp{cpu}.serial.log').write_text(out)
   steps=re.findall(r'^\[NATIVE-CAPS\] (step\d+) PASS (.+)$',out,re.M)
   assert len(steps)==9, f'SMP{cpu}: capability steps incomplete ({len(steps)}/9)'
   rows.append({'cpus':cpu,'steps':[s[0] for s in steps],
                'desired_pass':'[NATIVE-CAPS] PASS capability namespace and cross-process channels' in out})
   (e/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
   if not a.observe:
    assert rows[-1]['desired_pass'], f'SMP{cpu}: capability suite failed'
    assert '__CAPS_RC=0' in out, f'SMP{cpu}: probe exited nonzero'
    assert '[FAIL]' not in out, f'SMP{cpu}: boot regression marker'
    assert '[WAIT] PASS interruptible admission cases=18 failures=0' in out, f'SMP{cpu}: wait/cancel suite regressed'
   print(f'SMP{cpu} ' +('observed baseline' if a.observe else 'PASS')+f': {len(steps)}/9 steps',flush=True)
if __name__=='__main__':main()
