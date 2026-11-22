#!/usr/bin/env python3
"""Fork admission limit (P0-9): deterministic EAGAIN at the process-count
limit, exact reaping of every spawned child, slot recovery after reaps."""
import argparse,json,subprocess,tempfile
from pathlib import Path
from test_qemu import VM

def run_cpu(cpus, evidence_dir):
 with tempfile.TemporaryDirectory(prefix='rlimit-',dir='build') as d:
  vm=VM('build/fakeos.elf',mem='256M',extra_args=['-smp',str(cpus),'-nic','none'])
  out=''
  try:
   out+=vm.wait_for('System idle. Yielding',300,failure_markers=('[PANIC]','[EXCEPTION]'))
   # Login with retries: under host load the serial input processing can
   # lag far behind; retry until the Password prompt actually appears.
   got_login = False
   for attempt in range(4):
    vm.send('root')
    out+=vm.wait_for('Password:',30,failure_markers=('[PANIC]',))
    if 'Password:' in out:
     got_login = True
     break
   assert got_login, 'login prompt never appeared'
   vm.send('root');out+=vm.wait_for('Welcome root',60)
   out+=vm.wait_for('fakeos:',60)                    # shell prompt ready
   vm.send('/bin/init --rlimit; echo __RL_RC=$?; echo __RL_DONE__')
   out+=vm.wait_for('__RL_DONE__',150,failure_markers=('[PANIC]',))
  finally:
   out+=vm.read_available(.2);vm.close();(evidence_dir/f'smp{cpus}.serial.log').write_text(out)
  assert '__RL_RC=0' in out, f'rlimit probe failed on SMP{cpus}'
  assert '[RLIMIT] PASS deterministic EAGAIN limit + slot recovery' in out
  assert '[FAIL]' not in out
  return {'cpus':cpus,'pass':True}

def main():
 p=argparse.ArgumentParser();p.add_argument('--evidence',default='build/rlimit');a=p.parse_args()
 e=Path(a.evidence);e.mkdir(parents=True,exist_ok=True);rows=[]
 for cpus in (2,1):
  rows.append(run_cpu(cpus,e))
  print(f'SMP{cpus} PASS: deterministic EAGAIN limit + slot recovery',flush=True)
 (e/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
if __name__=='__main__':main()
