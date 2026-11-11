#!/usr/bin/env python3
"""Actual IRQ notification/take paths: coalescing, IRQ state and wake race."""
import os,subprocess,tempfile,sys
from pathlib import Path
sys.dont_write_bytecode=True
from test_net_tcp_io import body
ROOT=Path(__file__).resolve().parents[1]
def main():
 text=(ROOT/'kernel/net.c').read_text();isr=(ROOT/'kernel/isr.c').read_text()
 # Hard IRQs must never rebuild the former 19KiB syscall/NIC stack chain.
 dispatch=body(isr,'void isr_dispatch(')
 assert 'net_poll(' not in dispatch and 'net_tcp_timer(' not in dispatch
 assert dispatch.count('net_schedule_work();')==2
 pre='''package test;
 typedef unsigned long u64;typedef unsigned int u32;
 static u32 net_up,net_work_pending,net_worker_ready;static volatile u32 net_work_lock;
 static int irq=1,bad,wakes,sleeps,armed,inject;
 void net_schedule_work(void);
 u64 cpu_irq_save(void){u64 f=irq;irq=0;return f;}
 void cpu_irq_restore(u64 f){irq=(int)f;}
 void spin_lock(volatile u32*p){if(irq||*p)bad++;*p=1;}
 void spin_unlock(volatile u32*p){if(irq||!*p)bad++;*p=0;}
 void sched_wakeup(u64 c){if(irq||net_work_lock||c!=(u64)&net_work_pending)bad++;wakes++;armed=0;}
 void sched_sleep_unlock_spin(u64 c,volatile u32*p){
  if(irq||!*p||p!=&net_work_lock||c!=(u64)&net_work_pending)bad++;
  armed=1;sleeps++;spin_unlock(p);
  if(inject){inject=0;net_schedule_work();}
 }
 '''
 funcs='\n'.join(body(text,sig)for sig in ('void net_schedule_work(', 'static u32 net_work_take('))
 tests='''
 int main(void){
  net_schedule_work();if(net_work_pending||wakes||!irq)return 1;
  net_up=1;net_schedule_work();if(!net_work_pending||wakes||!irq)return 2;
  net_worker_ready=1;net_schedule_work();if(wakes)return 3;
  if(!net_work_take()||net_work_pending||!irq||sleeps)return 4;
  net_schedule_work();net_schedule_work();if(wakes!=1||!net_work_pending)return 5;
  if(!net_work_take()||net_work_pending)return 6;
  inject=1;if(net_work_take()||!net_work_pending||armed||sleeps!=1||wakes!=2||!irq)return 7;
  if(!net_work_take()||net_work_pending)return 8;
  irq=0;net_schedule_work();if(irq||!net_work_pending||wakes!=3)return 9;
  if(!net_work_take()||irq||net_work_pending||net_work_lock||bad)return 10;
  return 0;
 }
 '''
 with tempfile.TemporaryDirectory(prefix='fakeos-net-irq-native-')as d:
  p=Path(d)/'work.c';exe=Path(d)/'work';p.write_text(pre+funcs+tests)
  subprocess.run([os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc'),'--target=x86_64-linux','-O0',str(p),'-o',str(exe)],cwd=os.environ.get('FAKECC_SRC','/tmp/fakecc-pr116-merge'),env=dict(os.environ,FAKECC_RTCACHE='0'),check=True)
  subprocess.run([str(exe)],check=True)
 print('PASS: actual deferred IRQ/timer notification and consumption; coalescing, pre-start work, sleep-publication remote wake, IRQ0/1 restoration; no protocol work in hard IRQ')
if __name__=='__main__':main()
