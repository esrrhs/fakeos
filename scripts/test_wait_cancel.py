#!/usr/bin/env python3
"""State-rendezvoused real guest wait/cancel tests; sequential SMP1/2/4."""
import argparse,re,subprocess,tempfile,json
from pathlib import Path
from test_qemu import VM,disk_args
p=argparse.ArgumentParser();p.add_argument('--observe',action='store_true');p.add_argument('--evidence',required=True);a=p.parse_args();e=Path(a.evidence);e.mkdir(parents=True,exist_ok=True);rows=[]
for cpu in [1,2,4]:
 with tempfile.TemporaryDirectory(prefix='wait-cancel-',dir='build') as d:
  disk=str(Path(d)/'disk.img');subprocess.run(['python3','tools/mkext2.py',disk,'32'],check=True)
  vm=VM('build/fakeos.elf',mem='256M',extra_args=disk_args(disk)+['-smp',str(cpu),'-nic','none']);out=''
  try: out+=vm.wait_for('System idle. Yielding for IRQ-driven TTY/net I/O.',120,failure_markers=('[PANIC]','[EXCEPTION]'))
  finally: out+=vm.read_available(2.0);vm.close();(e/f'smp{cpu}.serial.log').write_text(out)
  cases=re.findall(r'^\[WAIT\] case=(\d+) round=(\d+) pid=(\d+) done=(\d+) code=(\d+) rescues=(\d+)',out,re.M)
  summary=re.search(r'\[WAIT\] (PASS|FAIL) interruptible admission cases=18 failures=(\d+) CPU-mask=0x([0-9a-f]+)',out)
  rows.append({'cpus':cpu,'cases':cases,'summary':summary.groups() if summary else None});(e/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
  assert len(cases)==18 and summary, 'incomplete wait probe'
  if not a.observe: assert summary[1]=='PASS' and all(c[3]=='1' and c[5]=='0' for c in cases) and '[FAIL]' not in out, 'wait admission regression'
  print('SMP',cpu,'OBSERVED' if a.observe else 'PASS',summary.groups(),flush=True)
