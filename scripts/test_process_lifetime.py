#!/usr/bin/env python3
"""Actual user process/thread lifetime probes after AP scheduler release."""
import argparse,json,re,subprocess,tempfile
from pathlib import Path
from test_qemu import VM,disk_args

def main():
 p=argparse.ArgumentParser();p.add_argument('--observe',action='store_true');p.add_argument('--evidence',default='build/process-lifetime');p.add_argument('--cpus',default='1,2,4');a=p.parse_args()
 e=Path(a.evidence);e.mkdir(parents=True,exist_ok=True);rows=[]
 for cpu in map(int,a.cpus.split(',')):
  with tempfile.TemporaryDirectory(prefix='process-life-',dir='build') as d:
   disk=str(Path(d)/'disk.img');subprocess.run(['python3','tools/mkext2.py',disk,'32'],check=True)
   vm=VM('build/fakeos.elf',mem='256M',extra_args=disk_args(disk)+['-smp',str(cpu),'-nic','none']);out=''
   try:
    out+=vm.wait_for('[NATIVE] PASS kernel-only object/channel suite',300,failure_markers=('[NATIVE] FAIL','[PANIC]','[EXCEPTION]'))
    assert '[NATIVE] PASS kernel-only object/channel suite' in out
    # Native suite completion precedes kmain's final multi-line banner. Do not
    # mix user acceptance records with that concurrent kernel console writer.
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
    vm.send('/bin/init --process-lifetime; echo __LIFE_RC=$?; echo __LIFE_DONE__')
    out+=vm.wait_for('\r\n__LIFE_DONE__\r\n',150,failure_markers=('[PANIC]',))
   finally:
    out+=vm.read_available(.2);vm.close();(e/f'smp{cpu}.serial.log').write_text(out)
   found=re.findall(r'^\[LIFETIME\] mode=(\d+) pid=(\d+) got=(\d+) code=(\d+) early=(\d+) worker_done=(\d+)',out,re.M)
   assert len(found)==2, f'SMP{cpu}: probe incomplete'
   rows.append({'cpus':cpu,'observed_modes':found,'desired_pass':'[LIFETIME] PASS process/task semantics' in out})
   (e/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
   if not a.observe: assert '[PROCESS] owner table full: admission rolled back' in out and re.search(r'\[LIFETIME\] PASS owner capacity=\d+ admission rollback, pipe refs/EOF and process reuse',out) and '[LIFETIME] PASS bounded task allocation failure/rollback/reuse and concurrent process teardown' in out and '[LIFETIME] PASS 64 unique process completions and slot/AS reuse' in out and '[LIFETIME] PASS multithread fork/exec BUSY' in out and rows[-1]['desired_pass'] and '__LIFE_RC=0' in out, f'SMP{cpu}: process lifetime regression'
   if not a.observe:
    assert '[FAIL]' not in out
    for mode,pid,got,code,early,done in found:
     assert pid==got and early=='0' and ((mode=='0' and code=='29' and done=='1') or (mode=='1' and code=='42' and done=='0')), 'unexpected completion/order semantics'
    masks=[int(x,16) for x in re.findall(r'\[PROCESS\] AS drained pid=\d+ task-exit CPU-mask=0x([0-9a-fA-F]+)',out)]
    if cpu>1: assert any(bin(m).count("1")>1 for m in masks), 'no actual multi-CPU task teardown evidence'
   print(f'SMP{cpu} ' +('observed baseline' if a.observe else 'PASS')+f': {found}',flush=True)
if __name__=='__main__':main()
