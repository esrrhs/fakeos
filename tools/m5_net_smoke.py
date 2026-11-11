#!/usr/bin/env python3
import sys,os,socket,threading,tempfile,subprocess,json,time,re,hashlib,argparse,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path[:0]=[str(ROOT/'scripts'),str(ROOT/'tools')]
sys.dont_write_bytecode=True
from test_qemu import VM
from m5_guest_smoke import verify_process_selftests
SOURCE=(ROOT/'user/progs/net_check.c').read_text()

assert max(map(len,SOURCE.splitlines()))<120
class Fixtures:
 def __init__(self,work):
  self.work=work;self.stop=threading.Event();self.events=[];self.socks=[];self.threads=[];self.event_lock=threading.Lock()
  self.dns=0;self.dns_unavailable=''
  for kind in ('udp','tcp','burst','rawburst','refuse','dns'):
   s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM if kind in ('udp','dns') else socket.SOCK_STREAM)
   try:s.bind(('127.0.0.1',53 if kind=='dns' else 0))
   except OSError as e:
    s.close()
    if kind!='dns':raise
    self.dns_unavailable=str(e);continue
   s.settimeout(.3);self.socks.append(s);setattr(self,kind,s.getsockname()[1])
   if kind!='refuse':
    if kind not in ('udp','dns'):s.listen(4)
    t=threading.Thread(target=self.serve,args=(kind,s),daemon=True);t.start();self.threads.append(t)
 def log(self,**kw):
  with self.event_lock:
   self.events.append(kw);(self.work/'host.json').write_text(json.dumps(self.events,indent=2)+'\n')
 def serve(self,kind,s):
  while not self.stop.is_set():
   try:
    if kind=='dns':
     b,a=s.recvfrom(512)
     if len(b)<17:continue
     n=12;labels=[]
     while n<len(b) and b[n] and b[n]<=63:
      l=b[n];labels.append(b[n+1:n+1+l].decode('ascii'));n+=1+l
     if n+5>len(b):continue
     q=b[12:n+5];name='.'.join(labels);answer=name=='fixture.fakeos.test'
     h=b[:2]+struct.pack('!HHHHH',0x8180 if answer else 0x8183,1,int(answer),0,0)
     rr=b'\xc0\x0c'+struct.pack('!HHIH',1,1,0,4)+bytes((10,0,2,2)) if answer else b''
     s.sendto(h+q+rr,a);self.log(kind='dns',name=name,answer=answer);continue
    if kind=='udp':
     b,a=s.recvfrom(65535);self.log(kind=kind,bytes=len(b),sha256=hashlib.sha256(b).hexdigest())
     s.sendto(bytes(x^90 for x in b),a);continue
    c,a=s.accept();c.settimeout(12)
    t=threading.Thread(target=self.connection,args=(kind,c),daemon=True)
    self.threads.append(t);t.start()
   except socket.timeout:continue
   except OSError as e:
    if not self.stop.is_set():self.log(kind=kind,error=str(e))
 def connection(self,kind,c):
  try:
   with c:
    if kind in ('burst','rawburst'):
     b=bytes(i%251 for i in range(8192))
     if kind=='burst':
      # Two 1024-byte deliveries fill the 2048-byte advertised window
      # despite slirp's small-window avoidance (one 1440-byte MSS leaves
      # 608 bytes which it may legitimately hold until the reader runs).
      # Connections remain simultaneous; no guest workload is serialized.
      c.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
      c.sendall(b[:1024]);self.stop.wait(.02)
      c.sendall(b[1024:2048]);self.stop.wait(.02)
      c.sendall(b[2048:])
     else:c.sendall(b) # Preserve the exact original crashing traffic pattern.
     c.shutdown(socket.SHUT_WR)
     self.log(kind=kind,sent=len(b),sha256=hashlib.sha256(b).hexdigest())
     while c.recv(1024):pass
    else:
     b=bytearray()
     while True:
      r=c.recv(4096)
      if not r:break
      b+=r;c.sendall(bytes(x^90 for x in r))
     self.log(kind=kind,received=len(b),sha256=hashlib.sha256(b).hexdigest(),eof=True)
  except OSError as e:
   if not self.stop.is_set():self.log(kind=kind,error=str(e))
 def close(self):
  self.stop.set()
  for s in self.socks:s.close()
  for t in self.threads:t.join(2)
def main(smp,nic,kernel):
 os.chdir(ROOT);work=Path(tempfile.mkdtemp(prefix=f'fakeos-net-smoke-smp{smp}-'))
 print('NETWORK SMOKE',work,flush=True);print('NIC',nic,flush=True);(work/'net_check.c').write_text(SOURCE)
 disk=work/'disposable.img';subprocess.run([sys.executable,'tools/mkext2.py',str(disk),'4'],check=True)
 fixtures=Fixtures(work);vm=None;transcript='';rows=[];serial=0
 (work/'ports.json').write_text(json.dumps({k:getattr(fixtures,k) for k in ('udp','tcp','burst','rawburst','refuse')}))
 try:
  vm=VM(kernel,mem='256M',extra_args=['-smp',str(smp),
   '-drive',f'id=testdisk,file={disk},format=raw,if=none','-device','virtio-blk-pci,disable-modern=on,drive=testdisk',
   '-netdev','user,id=n0','-device',{'legacy':'virtio-net-pci,disable-modern=on,netdev=n0','intx':'virtio-net-pci,disable-modern=on,netdev=n0,vectors=0','modern':'virtio-net-pci,disable-legacy=on,netdev=n0'}[nic],
   '-object',f'filter-dump,id=netcap,netdev=n0,file={work}/wire.pcap'])
  def wait(marker,timeout=180):
   nonlocal transcript
   out=vm.wait_for(marker,timeout);transcript+=out;(work/'guest.log').write_text(transcript,errors='surrogateescape')
   assert marker in out,('timeout',marker,out[-2000:]);return out
  def run(cmd):
   nonlocal serial
   serial+=1;tag=f'NET_{serial}';start=time.monotonic()
   vm.send(cmd+'; echo '+tag+':$?; echo '+tag+'_END');out=wait('\r\n'+tag+'_END\r\n',90)
   status=int(re.search(r'(?m)^'+tag+r':(\d+)\r?$',out).group(1));dt=time.monotonic()-start
   row={'command':cmd,'status':status,'host_seconds':dt,'out':out};rows.append(row)
   (work/'commands.json').write_text(json.dumps(rows,indent=2)+'\n');return row
  wait('fakeos login:');vm.send('root');wait('Password:');vm.send('root');wait('fakeos:')
  if 'System idle. Yielding for IRQ-driven TTY/net I/O.' not in transcript:wait('System idle. Yielding for IRQ-driven TTY/net I/O.')
  assert '[NET] DHCP lease ip=10.0.2.15' in transcript
  assert '[NET] deferred RX/timer worker ready' in transcript
  for marker in ('[PASS] ICMP echo reply from gateway','[PASS] UDP loopback','[PASS] TCP handshake + stream loopback'):
   assert marker in transcript,marker
  verify_process_selftests(transcript)
  # DHCP learns the gateway MAC, so explicitly exercise a different LOCAL
  # QEMU neighbor to prove ARP request/reply rather than assuming it occurred.
  r=run('/bin/ping -c 1 10.0.2.3');assert r['status']==0 and 'reply from 10.0.2.3' in r['out'],r
  assert not [l for l in transcript.splitlines() if '[FAIL]' in l or '[FATAL]' in l]
  assert run('export FAKECC_RTCACHE=0')['status']==0
  assert run('cd /src')['status']==0
  source_row=run('cat /src/net_check.c')
  assert SOURCE.strip() in source_row['out'].replace('\r',''), 'staged helper differs'
  r=run('/bin/fakecc --target=x86_64-linux -O0 /src/net_check.c -o /tmp/netcheck');assert r['status']==0,r
  for mode,port in [('u',fixtures.udp),('t',fixtures.tcp),('r',fixtures.refuse),('b',fixtures.burst),('c',fixtures.burst),('b',fixtures.rawburst),('c',fixtures.rawburst)]:
   r=run(f'/bin/timeout 12 /tmp/netcheck {mode} {port}')
   print('RESULT',smp,mode,r['status'],r['out'],flush=True)
   assert r['status']==0,r
  if fixtures.dns:
   assert run("printf 'nameserver 10.0.2.2\\n' > /etc/resolv.conf")['status']==0
   r=run('/bin/ping -c 1 fixture.fakeos.test');assert r['status']==0 and 'reply from' in r['out'],r
   r=run('/bin/ping -c 1 absent.fakeos.test');assert r['status']==1 and 'unknown host' in r['out'],r
   assert any(e.get('kind')=='dns' and e.get('answer') for e in fixtures.events)
   print('PASS local deterministic DNS A + NXDOMAIN via real libc resolver',flush=True)
  else:print('DNS NOT TESTED: local127.0.0.1:53 unavailable: '+fixtures.dns_unavailable,flush=True)
  assert not [e for e in fixtures.events if 'error' in e],fixtures.events
 finally:
  if vm:vm.close()
  fixtures.close()
 # QEMU must close first so the capture is complete. Never print overall PASS
 # unless actual packet ACK/window/FIN and byte-integrity evidence passes too.
 from m5_net_wire_audit import audit
 audit(work)
 print('PASS: real NIC DHCP/ARP/gateway/UDP1024/TCP4096/halfclose/refused/RX8192+three concurrent readers; smp='+str(smp)+' nic='+nic,flush=True)
if __name__=='__main__':
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument('--smp',type=int,choices=(1,2,4),default=2)
 ap.add_argument('--nic',choices=('legacy','intx','modern'),default='legacy')
 ap.add_argument('--kernel',default='build/fakeos.elf')
 args=ap.parse_args();main(args.smp,args.nic,args.kernel)
