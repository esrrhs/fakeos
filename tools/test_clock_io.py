#!/usr/bin/env python3
"""fakecc-native tests of actual timer publication and syscall timeout bodies."""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def body(path,signature):
    text=(ROOT/path).read_text();start=text.index(signature);opening=text.index('{',start);depth=1;end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
PRE=r"""package test;
typedef unsigned long u64;typedef long i64;typedef unsigned int u32;typedef int s32;typedef unsigned short u16;
enum{VRUNTIME_SLICE=1,FD_BAD=0xffffffff,EINTR_SEL=-4,EINVAL_NEG=-22,EINTR_NEG=-4,POLLIN_F=1,POLLOUT_F=4,POLLNVAL_F=32,TICKS_PER_SEC=100,NS_PER_TICK=10000000,NS_PER_SEC=1000000000,CLOCK_REALTIME_ID=0,CLOCK_MONOTONIC_ID=1,CLOCK_BOOTTIME_ID=7};
static u64 sched_ticks,clock_ticks;static u32 active,sched_bsp_cpu,cpu,irq,charges,switches,locks,yields,interrupt_at,bad_range;
static u32 select_wait_token;static u32 sched_run_lock;
void test_irq(void){irq++;}void spin_lock(u32*p){locks++;}void cfs_charge(u32 t,u32 v){charges++;}void cfs_update_min(void){}
u32 sched_cpu_index(void){return cpu;}u32 sched_cur(void){return cpu;}u32 pick_next(u32 t){return t;}void sched_switch_to(u32 t){switches++;}
u64 sched_current_as_h(void){return 1;}u32 test_range(u64 a,u64 p,u64 n){return !bad_range && p!=0;}
void sched_yield(void){clock_ticks++;yields++;}void sched_sleep(u64 p){sched_yield();}void sched_check_signals(void){}u32 sched_should_eintr_timer(void){return yields>=interrupt_at;}
u32 sched_fd_get(u32 fd){return FD_BAD;}u32 fd_ready_read(u32 f){return 0;}u32 fd_ready_write(u32 f){return 0;}
void reset(void){clock_ticks=100;yields=0;interrupt_at=0xffffffff;bad_range=0;}
"""
SIGNAL_PRE=r"""package test;
typedef unsigned long u64;typedef unsigned int u32;
enum{SIGKILL_N=9,SIGINT_N=2,SIGTERM_N=15,SIGSTOP_N=19,SIGTSTP_N=20,SIGCONT_N=18,SIGTTIN_N=21,SIGTTOU_N=22,SA_RESTART_F=0x10000000};
struct task{u32 is_user,pid,pending_sigs,sig_ignore;u64 sig_handler[32],sig_flags[32];};
static struct task threads[1];static int terminations,stops,delivered;static u64 code;
u32 sched_cur(void){return 0;}void proc_terminate(u64 c){terminations++;code=c;}void sched_stop_self(u32 s){stops++;}
u32 sig_deliver_frame(u32 s,u64*a,u64*b,u64*c,u64*d,u64*e){delivered++;threads[0].pending_sigs &=~(1u<<s);return 1;}
"""
SIGNAL_MAIN=r"""
int main(void){u64 f[5];threads[0].is_user=1;threads[0].pid=1;
 threads[0].pending_sigs=1u<<15;threads[0].sig_handler[15]=4096;
 sched_check_signals_frame(0,0,0,0,0);
 if(terminations||stops||threads[0].pending_sigs!=(1u<<15))return 1;
 if(!sched_should_eintr_timer()||!sched_should_eintr())return 2;
 threads[0].sig_flags[15]=SA_RESTART_F;
 if(!sched_should_eintr_timer()||sched_should_eintr())return 3;
 sched_check_signals_frame(f,f+1,f+2,f+3,f+4);
 if(delivered!=1||threads[0].pending_sigs||terminations)return 4;
 threads[0].pending_sigs=1u<<10;threads[0].sig_handler[10]=8192;
 sched_check_signals_frame(0,0,0,0,0);
 if(threads[0].pending_sigs!=(1u<<10)||terminations||!sched_should_eintr_timer())return 5;
 threads[0].sig_ignore=1u<<10;
 if(sched_should_eintr_timer()||sched_should_eintr())return 6;
 sched_check_signals_frame(0,0,0,0,0);if(threads[0].pending_sigs)return 7;
 threads[0].sig_ignore=0;threads[0].pending_sigs=1u<<20;threads[0].sig_handler[20]=4096;
 sched_check_signals_frame(0,0,0,0,0);if(stops||threads[0].pending_sigs!=(1u<<20))return 8;
 threads[0].pending_sigs=1u<<15;threads[0].sig_handler[15]=0;
 sched_check_signals_frame(0,0,0,0,0);if(terminations!=1||code!=143)return 9;
 threads[0].pending_sigs=1u<<9;sched_check_signals_frame(0,0,0,0,0);
 if(terminations!=2||code!=137)return 10;
 return 0;}
"""
TIMEOUT_PRE=r"""package test;
enum{WNOHANG=1};static long ms,step=60,done_after,waits,naps,wait_interrupt,sleep_error,clock_error;
long u_clock_gettime(long id,void*p){long*t=(long*)p;if(clock_error)return -1;t[0]=ms/1000;t[1]=(ms%1000)*1000000;return 0;}
long u_wait4(long pid,long*st,long opt,long*r){waits++;if(wait_interrupt){if(wait_interrupt>0)wait_interrupt--;return -4;}return done_after&&waits>=done_after?pid:0;}
long u_nanosleep(const void*t,void*r){naps++;ms+=step;return sleep_error;}
void reset(void){ms=0;step=60;done_after=0;waits=0;naps=0;wait_interrupt=0;sleep_error=0;clock_error=0;}
"""
TIMEOUT_MAIN=r"""
int main(void){long status,value;reset();
 if(wait_reap(7,&status,1000)!=0||ms<1000||ms>1060||naps!=17)return 1;
 reset();step=300;if(wait_reap(7,&status,1000)!=0||ms!=1200)return 2;
 reset();done_after=3;if(wait_reap(7,&status,1000)!=1||ms!=120)return 3;
 reset();if(wait_reap(7,&status,0)!=0||naps)return 4;
 reset();done_after=1;if(wait_reap(7,&status,0)!=1||naps)return 5;
 reset();clock_error=1;if(wait_reap(7,&status,1000)!=-1||naps)return 6;
 reset();sleep_error=-5;if(wait_reap(7,&status,1000)!=-1||naps!=1)return 7;
 reset();wait_interrupt=1;if(wait_reap(7,&status,1000)!=0||ms!=1020)return 8;
 reset();sleep_error=-4;if(wait_reap(7,&status,1000)!=0||ms!=1020)return 9;
 reset();wait_interrupt=-1;if(wait_reap(7,&status,1000)!=0||ms!=1020)return 13;
 if(!parse_secs("2",&value)||value!=2)return 10;
 if(!parse_secs("9223372036854775",&value)||value!=9223372036854775L)return 11;
 if(parse_secs("9223372036854776",&value)||parse_secs("18446744073709551616",&value)||parse_secs("-1",&value))return 12;
 return 0;}
"""
def main():
    funcs=[body('kernel/sched.c','void sched_tick(void)'),body('kernel/sched.c','u64 sched_clock_ticks(void)'),body('kernel/sysproc.c','static void ticks_to_timespec('),body('kernel/sysproc.c','u64 sys_clock_gettime('),body('kernel/sysproc.c','u64 sys_nanosleep('),body('kernel/sysfile.c','u64 sys_select('),body('kernel/sysfile.c','u64 sys_poll(')]
    source=PRE+'\n'.join(funcs).replace('arch.lapic_irq()','test_irq()').replace('mem.as_user_range_ok','test_range')
    tests=[]
    def check(expression):tests.append('if(!('+expression+'))return '+str(len(tests)+1)+';')
    def add(s):tests.append(s)
    add('int main(void){u64 req[2],rem[2],stamp[2],tv[2];u32 n,c;')
    add('active=1;sched_bsp_cpu=0;sched_ticks=0;clock_ticks=0;for(n=0;n<100;n++)for(cpu=0;cpu<8;cpu++)sched_tick();')
    check('clock_ticks==100 && sched_ticks==800 && irq==800 && charges==800 && switches==800 && locks==800')
    add('sched_bsp_cpu=3;for(n=0;n<50;n++)for(cpu=0;cpu<4;cpu++)sched_tick();')
    check('clock_ticks==150 && sched_ticks==1000 && charges==1000 && switches==1000')
    add('active=0;sched_tick();');check('irq==1001 && clock_ticks==150 && sched_ticks==1000 && switches==1000')
    add('reset();');check('sys_clock_gettime(1,(u64)stamp)==0 && stamp[0]==1 && stamp[1]==0')
    check('sys_clock_gettime(7,(u64)stamp)==0');check('sys_clock_gettime(0,(u64)stamp)==0');check('sys_clock_gettime(99,(u64)stamp)==(u64)-22')
    add('req[0]=0;req[1]=0;rem[0]=77;');check('sys_nanosleep((u64)req,(u64)rem)==0 && yields==0 && rem[0]==77')
    for ns in (1,9999999,10000000,10000001,999999999):
        add(f'reset();req[0]=0;req[1]={ns};');check(f'sys_nanosleep((u64)req,0)==0 && yields=={(ns+9999999)//10000000+1}')
    add('reset();req[0]=2;req[1]=0;interrupt_at=3;');check('sys_nanosleep((u64)req,(u64)rem)==(u64)-4 && rem[0]==1 && rem[1]==980000000')
    add('reset();interrupt_at=0;');check('sys_nanosleep((u64)req,(u64)rem)==(u64)-4 && rem[0]==2 && rem[1]==0 && yields==0')
    add('reset();req[0]=~0ULL;');check('sys_nanosleep((u64)req,0)==(u64)-22 && yields==0')
    add('req[0]=0x7fffffffffffffffULL;');check('sys_nanosleep((u64)req,0)==(u64)-22 && yields==0')
    add('req[0]=0;req[1]=1000000000;');check('sys_nanosleep((u64)req,0)==(u64)-22 && yields==0')
    add('req[1]=1;bad_range=1;');check('sys_nanosleep((u64)req,0)==(u64)-1 && yields==0')
    add('reset();clock_ticks=~0ULL-1;req[0]=2;req[1]=0;');check('sys_nanosleep((u64)req,0)==(u64)-22 && yields==0')
    add('reset();tv[0]=0;tv[1]=0;');check('sys_select(0,0,0,0,(u64)tv)==0 && yields==0')
    for us in (1,10000,10001,15001):
        add(f'reset();tv[0]=0;tv[1]={us};');check(f'sys_select(0,0,0,0,(u64)tv)==0 && yields=={(us+9999)//10000+1}')
    add('reset();tv[1]=1000000;');check('sys_select(0,0,0,0,(u64)tv)==(u64)-1 && yields==0')
    add('tv[0]=0x7fffffffffffffffULL;tv[1]=0;');check('sys_select(0,0,0,0,(u64)tv)==(u64)-1 && yields==0')
    add('reset();interrupt_at=2;');check('sys_select(0,0,0,0,0)==(u64)-4 && yields==2')
    for ms in (0,1,10,11,15,1000):
        add('reset();');check(f'sys_poll(0,0,{ms})==0 && yields=={(ms+9)//10+1 if ms else 0}')
    add('reset();interrupt_at=3;');check('sys_poll(0,0,(u64)-1)==(u64)-4 && yields==3')
    add('reset();interrupt_at=2;');check('sys_poll(0,0,1000)==(u64)-4 && yields==2')
    add('reset();clock_ticks=~0ULL-1;');check('sys_poll(0,0,1000)==(u64)-1 && yields==0')
    add('return 0;}')
    with tempfile.TemporaryDirectory(prefix='fakeos-clock-unit-') as d:
        src=Path(d)/'clock.c';exe=Path(d)/'clock';src.write_text(source+'\n'+'\n'.join(tests))
        subprocess.run([os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        sig=SIGNAL_PRE+'\n'.join(body('kernel/sched.c',signature) for signature in ('u32 sched_has_pending_fatal(void) {','u32 sched_should_eintr(void) {','u32 sched_should_eintr_timer(void) {','void sched_check_signals_frame('))+SIGNAL_MAIN
        src.write_text(sig)
        subprocess.run([os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        src.write_text(TIMEOUT_PRE+'\n'.join(body('user/timeout.c',signature) for signature in ('static int parse_secs(', 'static long clock_millis(', 'static int wait_reap('))+TIMEOUT_MAIN)
        subprocess.run([os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('PASS: actual timeout monotonic budgets resist oversleep/EINTR, zero/early child completion, clock/sleep errors and duration overflow')
    print('PASS: actual caught-signal unframed deferral, framed delivery, ignored/default/KILL handling; timer SA_RESTART-independent EINTR and unchanged restartable-I/O selection')
    print('PASS: actual tick ISR preserves all8 CPU preemptions, nonzero BSP and inactive behavior; actual clock/nanosleep/select/poll bodies: round-up/phase, zero, invalid/overflow, EINTR and bounded remainder')
if __name__=='__main__':main()
