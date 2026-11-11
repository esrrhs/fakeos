#!/usr/bin/env python3
"""Check actual NIC packet evidence for bounded zero-window/reopen/FIN integrity."""
import json,struct,hashlib
from pathlib import Path
def audit(work):
 work=Path(work);ports=json.loads((work/'ports.json').read_text())
 raw=(work/'wire.pcap').read_bytes();assert raw[:4]==bytes.fromhex('d4c3b2a1')
 off=24;flows={};arp=icmp=0;dest=set();arp_seen=set()
 while off+16<=len(raw):
  _,_,cap,_=struct.unpack_from('<IIII',raw,off);off+=16;b=raw[off:off+cap];off+=cap
  assert len(b)==cap
  if len(b)<14:continue
  if b[12:14]==b'\x08\x06':
   assert len(b)>=42 and b[14:20]==bytes.fromhex('000108000604')
   arp+=1
   arp_seen.add((int.from_bytes(b[20:22],'big'),'.'.join(map(str,b[28:32])),'.'.join(map(str,b[38:42]))))
   continue
  if b[12:14]!=b'\x08\x00' or len(b)<34:continue
  ip=b[14:];h=(ip[0]&15)*4;total=int.from_bytes(ip[2:4],'big')
  assert h>=20 and h<=total<=len(ip)
  if ip[12:16] in (bytes((10,0,2,15)),bytes(4)):
   dest.add('.'.join(map(str,ip[16:20])))
  if ip[9]==1:icmp+=1
  if ip[9]!=6 or total<h+20:continue
  tcp=ip[h:total];sp,dp,seq,ack=struct.unpack_from('!HHII',tcp)
  role=next((role for role in ('burst','rawburst') if ports[role] in (sp,dp)),None)
  if role is None:continue
  th=(tcp[12]>>4)*4;assert 20<=th<=len(tcp)
  guest=ip[12:16]==bytes((10,0,2,15));gp=sp if guest else dp
  f=flows.setdefault(gp,{'packets':[],'role':role})
  f['packets'].append({'guest':guest,'seq':seq,'ack':ack,'flags':tcp[13],
                       'window':int.from_bytes(tcp[14:16],'big'),'data':tcp[th:]})
 assert arp>=2 and icmp>=2,(arp,icmp)
 assert (1,'10.0.2.15','10.0.2.3') in arp_seen and (2,'10.0.2.3','10.0.2.15') in arp_seen,arp_seen
 assert dest <= {'255.255.255.255','10.0.2.2','10.0.2.3'},dest
 assert len(flows)==8,flows.keys() # delayed reader + three children, shaped AND original raw traffic
 assert sum(f['role']=='burst' for f in flows.values())==4
 evidence=[]
 for gp,f in flows.items():
  packets=f['packets'];synack=next(p for p in packets if not p['guest'] and p['flags']&18==18)
  base=(synack['seq']+1)&0xffffffff;guest=[p for p in packets if p['guest']]
  assert all(0<=p['window']<=2048 for p in guest)
  zeroes=[i for i,p in enumerate(guest) if p['window']==0]
  reopened=bool(zeroes) and any(p['window']>0 for p in guest[zeroes[0]+1:])
  if f['role']=='burst':
   assert zeroes and reopened,('controlled fill did not reach zero/reopen',gp)
  assert max((p['ack']-base)&0xffffffff for p in guest if p['flags']&16)==8193
  data={}
  for p in packets:
   if p['guest']:continue
   start=(p['seq']-base)&0xffffffff
   for i,v in enumerate(p['data']):
    pos=start+i
    if pos>=8192:continue # a keepalive/probe outside the data interval is not data
    assert v==pos%251,(gp,pos,v)
    assert pos not in data or data[pos]==v
    data[pos]=v
  assert len(data)==8192,(gp,len(data))
  digest=hashlib.sha256(bytes(data[i] for i in range(8192))).hexdigest()
  evidence.append({'guest_port':gp,'role':f['role'],'wire_bytes':8192,'sha256':digest,
                   'max_advertised_window':max(p['window'] for p in guest),
                   'zero_then_reopened':reopened,'final_ack_data_plus_fin':8193})
 out={'arp_packets':arp,'icmp_packets':icmp,'guest_ipv4_destinations':sorted(dest),'flows':evidence}
 (work/'wire-audit.json').write_text(json.dumps(out,indent=2)+'\n')
 print('PASS: real NIC ARP/ICMP, local-only destinations; all8 TCP8192-byte flows (including original raw traffic), bounded2048 window, four controlled zero/reopen and complete-data-before-FIN ACK',flush=True)
 return out
if __name__=='__main__':
 import sys
 audit(sys.argv[1])
