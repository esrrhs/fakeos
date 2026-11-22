#!/usr/bin/env python3
"""vDSO-lite clock probe: user-space TSC clock page (no syscall), monotonic
reads, vDSO-vs-syscall agreement, nanosleep precision. SMP2 covers a real
cross-CPU boot; the calibration itself is CPU-independent (PIT window)."""
import argparse,json,re,subprocess,tempfile
from pathlib import Path
from test_qemu import VM

def run_cpu(cpus, evidence_dir):
 with tempfile.TemporaryDirectory(prefix='vdso-',dir='build') as d:
  vm=VM('build/fakeos.elf',mem='256M',extra_args=['-smp',str(cpus),'-nic','none'])
  out=''
  try:
   out+=vm.wait_for('System idle. Yielding',120,failure_markers=('[PANIC]','[EXCEPTION]'))
   assert '[TSC] calibrated:' in out, 'TSC calibration missing'
   m=re.search(r'\[TSC\] calibrated: (\d+) Hz',out)
   hz=int(m.group(1))
   assert 500000000 <= hz <= 20000000000, f'implausible TSC rate {hz}'
   vm.send('root');out+=vm.wait_for('Password:',5);vm.send('root');out+=vm.wait_for('Welcome root',10)
   vm.send('/bin/init --vdso; echo __VD_RC=$?; echo __VD_DONE__')
   out+=vm.wait_for('__VD_DONE__',60,failure_markers=('[PANIC]',))
  finally:
   out+=vm.read_available(.2);vm.close();(evidence_dir/f'smp{cpus}.serial.log').write_text(out)
  assert '__VD_RC=0' in out, f'vdso probe failed on SMP{cpus}'
  assert '[VD] PASS vDSO clock: monotonic, matches syscall, sleep precise' in out
  assert '[FAIL]' not in out
  return {'cpus':cpus,'tsc_hz':hz,'pass':True}

def main():
 p=argparse.ArgumentParser();p.add_argument('--evidence',default='build/vdso');a=p.parse_args()
 e=Path(a.evidence);e.mkdir(parents=True,exist_ok=True);rows=[]
 for cpus in (2,1):
  rows.append(run_cpu(cpus,e))
  print(f'SMP{cpus} PASS: vDSO clock monotonic + syscall agreement + sleep precision',flush=True)
 (e/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
if __name__=='__main__':main()
