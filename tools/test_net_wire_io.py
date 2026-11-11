#!/usr/bin/env python3
"""Actual legacy vector helper and IPv4/TCP checksum/truncation rejection."""
import os,subprocess,tempfile,struct,sys
sys.dont_write_bytecode=True
from pathlib import Path
from test_net_tcp_io import body,ROOT
PRE='''package test;
typedef unsigned char u8;typedef unsigned short u16;typedef unsigned int u32;typedef unsigned long u64;
'''
def run(text):
 with tempfile.TemporaryDirectory(prefix='fakeos-net-wire-') as d:
  p=Path(d)/'test.c';o=Path(d)/'test';p.write_text(text)
  subprocess.run([os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc'),'--target=x86_64-linux','-O0',str(p),'-o',str(o)],cwd=os.environ.get('FAKECC_SRC','/tmp/fakecc-pr116-merge'),env=dict(os.environ,FAKECC_RTCACHE='0'),check=True)
  subprocess.run([str(o)],check=True)
def main():
 raw=(ROOT/'build/switch.o').read_bytes()
 off=struct.unpack_from('<Q',raw,40)[0];width,count=struct.unpack_from('<HH',raw,58)
 secs=[struct.unpack_from('<IIQQQQIIQQ',raw,off+i*width) for i in range(count)]
 code=None
 for sec in secs:
  if sec[1]!=2:continue
  namesec=secs[sec[6]];names=raw[namesec[4]:namesec[4]+namesec[5]]
  for pos in range(sec[4],sec[4]+sec[5],sec[9]):
   no,info,other,ndx,value,size=struct.unpack_from('<IBBHQQ',raw,pos)
   name=names[no:names.index(0,no)].decode()
   if name=='spin_test_and_set':code=raw[secs[ndx][4]+value:secs[ndx][4]+value+8]
 assert code==bytes.fromhex('b8 01 00 00 00 87 07 c3'),code
 # Execute the actual emitted primitive in ordinary executable text. No host
 # assembler, RWX mapping, or assumed source-to-instruction translation.
 emitted=','.join(str(x) for x in code)
 run(PRE+'__attribute__((naked)) u32 tas(u32*p){__asm__ volatile(".byte '+emitted+'\\n");}'+  'int main(void){u32 w=0;if(tas(&w)||w!=1)return 1;if(tas(&w)!=1||w!=1)return 2;w=0;if(tas(&w)||w!=1)return 3;return 0;}')
 driver=(ROOT/'drivers/virtio_net.c').read_text()
 run(PRE+'''enum{REG_MSIX_CONFIG=20,REG_MSIX_QUEUE=22,VIRTIO_MSI_NO_VECTOR=65535};
 static struct{u32 msix;}vn;static int writes,bad,reject;static u16 cfg,queue;
 void kprintf(const char*f,...){}
 void iow16(u16 r,u16 v){writes++;if(r==20)cfg=v;else if(r==22)queue=v;else bad++;}
 u16 ior16(u16 r){if(r!=22)bad++;return reject?65535:queue;}
 '''+body(driver,'static u32 legacy_msix_queue(')+'''
 int main(void){if(!legacy_msix_queue()||writes)return 1;
 vn.msix=1;if(!legacy_msix_queue()||writes!=2||cfg!=65535||queue||bad)return 2;
 reject=1;if(legacy_msix_queue()||writes!=4||bad)return 3;return 0;}
 ''')
 net=(ROOT/'kernel/net.c').read_text()
 funcs='\n'.join(body(net,s) for s in ('static u16 get16(', 'static u32 get32(', 'static void put16(', 'static void put32(', 'static u16 checksum(', 'static u16 tcp_checksum(', 'static void handle_tcp(u32 src_ip, u32 dst_ip, const u8 *tcp, u32 len) {','static void handle_ip('))
 run(PRE+'''enum{IP_HDR=20,TCP_HDR=20,IP_PROTO_ICMP=1,IP_PROTO_UDP=17,IP_PROTO_TCP=6,LOOPBACK_IP=0x7f000001};
 static u32 local_ip=0x0a00020f,dhcp_pending,calls,other;
 void arp_learn(u32 ip,const u8*mac){}
 void tcp_input(u32 a,u32 b,const u8*p,u32 n){calls++;}
 void handle_udp(u32 a,u32 b,const u8*p,u32 n){other++;}
 void handle_icmp(u32 a,const u8*p,u32 n,const u8*m){other++;}
 '''+funcs+'''
 static u8 pkt[64],mac[6];
 int main(void){u32 i;u8*t=pkt+20;
 pkt[0]=0x45;put16(pkt+2,40);pkt[8]=64;pkt[9]=6;
 put32(pkt+12,0x0a000202);put32(pkt+16,local_ip);t[12]=0x50;
 put16(t+16,tcp_checksum(0x0a000202,local_ip,t,20));
 put16(pkt+10,checksum(pkt,20));handle_ip(pkt,40,mac);if(calls!=1)return 1;
 handle_ip(pkt,39,mac);if(calls!=1)return 2;
 pkt[8]^=1;handle_ip(pkt,40,mac);if(calls!=1)return 3;pkt[8]^=1;
 t[1]^=1;handle_ip(pkt,40,mac);if(calls!=1)return 4;t[1]^=1;
 put16(pkt+6,0x2000);put16(pkt+10,0);put16(pkt+10,checksum(pkt,20));
 handle_ip(pkt,40,mac);if(calls!=1)return 5;
 put16(pkt+6,1);put16(pkt+10,0);put16(pkt+10,checksum(pkt,20));
 handle_ip(pkt,40,mac);if(calls!=1)return 6;
 for(i=0;i<20;i++)handle_tcp(0x0a000202,local_ip,t,i);
 if(calls!=1||other)return 7;return 0;}
 ''')
 print('PASS: actual emitted TAS old-value return contract; actual legacy MSI-X routing/readback/rejection; IPv4/TCP checksum, wire truncation and unsupported IP fragment rejection')
if __name__=='__main__':main()
