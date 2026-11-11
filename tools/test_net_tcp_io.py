#!/usr/bin/env python3
"""Compile actual bounded TCP RX transitions with released fakecc, not a model."""
import os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def body(text,sig):
 a=text.index(sig);b=text.index('{',a);d=1;e=b+1
 while d:
  d+=(text[e]=='{')-(text[e]=='}');e+=1
 return text[a:e]
def main():
 text=(ROOT/'kernel/net.c').read_text()
 pre='''package test;
typedef unsigned char u8;typedef unsigned short u16;typedef unsigned int u32;typedef unsigned long u64;
struct kmutex{u32 locked,owner;};struct wait_queue_head{u32 pad;};
'''+text[text.index('enum {'):text.index('static u8  local_mac')]+'''
static int irq=1,bad,wakes;
u64 cpu_irq_save(void){u64 f=irq;irq=0;return f;}
void cpu_irq_restore(u64 f){irq=(int)f;}
void spin_lock(volatile u32*p){if(irq||*p)bad++;*p=1;}
void spin_unlock(volatile u32*p){if(irq||!*p)bad++;*p=0;}
void sock_wake(struct sock*s){if(s->rx_lock)bad++;wakes++;}
'''
 funcs='\n'.join(body(text,sig) for sig in ('static u32 stream_push(', 'static u32 stream_pop(', 'static u32 tcp_before(', 'static void tcp_receive('))
 tests=r'''
static struct sock s;
static u8 data[4096],out[4096];
static void reset(u32 seq){u32 i;u8*p=(u8*)&s;for(i=0;i<sizeof(s);i++)p[i]=0;
 s.used=1;s.tcp_state=TCPS_ESTABLISHED;s.rcv_nxt=seq;wakes=0;}
int main(void){u32 i,n;for(i=0;i<4096;i++)data[i]=(u8)(i%251);
 reset(100);tcp_receive(&s,100,data,3000,TCPF_FIN);
 if(s.s_count!=2048||s.rcv_nxt!=2148||s.peer_fin||wakes!=1)return 1;
 tcp_receive(&s,2148,data+2048,952,TCPF_FIN);
 if(s.rcv_nxt!=2148||s.peer_fin||s.s_count!=2048)return 2;
 n=stream_pop(&s,out,1000);if(n!=1000)return 3;
 for(i=0;i<n;i++)if(out[i]!=data[i])return 4;
 tcp_receive(&s,100,data,3000,TCPF_FIN);
 if(s.rcv_nxt!=3101||!s.peer_fin||s.tcp_state!=TCPS_CLOSE_WAIT||s.s_count!=2000)return 5;
 n=stream_pop(&s,out,4096);if(n!=2000)return 6;
 for(i=0;i<n;i++)if(out[i]!=data[i+1000])return 7;
 tcp_receive(&s,3100,data,0,TCPF_FIN);if(s.rcv_nxt!=3101)return 8;
 tcp_receive(&s,3101,data,10,0);if(s.s_count||s.rcv_nxt!=3101)return 9;
 reset(200);tcp_receive(&s,205,data,20,TCPF_FIN);
 if(s.s_count||s.rcv_nxt!=200||s.peer_fin||wakes)return 10;
 tcp_receive(&s,200,data,40,0);tcp_receive(&s,200,data,40,0);
 if(s.s_count!=40||s.rcv_nxt!=240)return 11;
 reset(0xfffffff0u);tcp_receive(&s,0xfffffff0u,data,32,0);
 if(s.rcv_nxt!=16||s.s_count!=32)return 12;
 tcp_receive(&s,0,data+16,32,TCPF_FIN);
 if(s.rcv_nxt!=33||s.s_count!=48||!s.peer_fin)return 13;
 n=stream_pop(&s,out,48);if(n!=48)return 14;
 for(i=0;i<n;i++)if(out[i]!=data[i])return 15;
 reset(50);s.shut_rd=1;tcp_receive(&s,50,data,3000,TCPF_FIN);
 if(s.s_count||s.rcv_nxt!=3051||!s.peer_fin)return 16;
 if(!irq||bad||s.rx_lock)return 17;
 irq=0;reset(100);tcp_receive(&s,100,data,30,0);
 if(irq||s.s_count!=30||s.rx_lock||bad)return 18;irq=1;
 return 0;}
'''
 with tempfile.TemporaryDirectory(prefix='fakeos-net-native-') as d:
  source=Path(d)/'rx.c';exe=Path(d)/'rx';source.write_text(pre+funcs+tests)
  fcc=os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc')
  src=os.environ.get('FAKECC_SRC','/tmp/fakecc-pr116-merge')
  env=dict(os.environ,FAKECC_RTCACHE='0')
  subprocess.run([fcc,'--target=x86_64-linux','-O0',str(source),'-o',str(exe)],cwd=src,env=env,check=True)
  subprocess.run([str(exe)],check=True)
 print('PASS: actual TCP RX partial/full ring, overlap/duplicate/out-of-order, FIN ordering, sequence wrap, explicit read shutdown, byte retention and IRQ/lock discipline')
if __name__=='__main__':main()
