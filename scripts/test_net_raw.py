#!/usr/bin/env python3
"""Zero-copy raw ring probe (AF_XDP analogue) under QEMU user-net.

The guest probe maps the driver frame pool into user space, builds a UDP
frame directly in a TX pool slot, kicks, and verifies the SAME physical
slot comes back on the RX ring (zero-copy turnaround) while the protocol
stack also delivers the datagram to a bound socket (dual path)."""
import argparse,json,re,subprocess,tempfile
from pathlib import Path
from test_qemu import VM

QEMU_NET = ['-netdev','user,id=net0,net=10.0.2.0/24,dhcpstart=10.0.2.15',
            '-device','virtio-net-pci,disable-modern=on,netdev=net0']

def main():
 p=argparse.ArgumentParser();p.add_argument('--evidence',default='build/net-raw');a=p.parse_args()
 e=Path(a.evidence);e.mkdir(parents=True,exist_ok=True);rows=[]
 with tempfile.TemporaryDirectory(prefix='net-raw-',dir='build') as d:
  vm=VM('build/fakeos.elf',mem='256M',extra_args=QEMU_NET+['-smp','2'])
  out=''
  try:
   out+=vm.wait_for('System idle. Yielding',300,failure_markers=('[PANIC]','[EXCEPTION]','[FAIL]'))
   idle='System idle. Yielding'
   assert idle in out, 'boot-complete barrier missing'
   vm.send('root');out+=vm.wait_for('Password:',20);vm.send('root');out+=vm.wait_for('Welcome root',30)
   out+=vm.wait_for('fakeos:',30)
   vm.send('/bin/init --net-raw; echo __RAW_RC=$?; echo __RAW_DONE__')
   out+=vm.wait_for('__RAW_DONE__',60,failure_markers=('[PANIC]',))
  finally:
   out+=vm.read_available(.2);vm.close();(e/'smp2.serial.log').write_text(out)
  assert '__RAW_RC=0' in out, f'net-raw probe failed: see {e}'
  assert '[NETRAW] zero-copy turnaround: TX slot == RX slot, payload intact' in out
  assert '[NETRAW] PASS zero-copy rings + protocol dual-path' in out
  assert '[FAIL]' not in out
  assert '[WAIT] PASS interruptible admission cases=18 failures=0' in out
  rows.append({'cpus':2,'pass':True})
  (e/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
  print('SMP2 PASS: zero-copy turnaround + protocol dual-path',flush=True)
if __name__=='__main__':main()
