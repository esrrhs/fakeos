/* Guest-compiled raw ABI regression. No host libraries or fabricated time. */
package main;
import runtime;
static volatile int seen;
/* Normal executable-text restorer; naked avoids changing the signal-frame RSP.
 * Emitted by fakecc itself, not a host assembler or writable executable page. */
__attribute__((naked)) void restorer(void) {
    __asm__ volatile (".byte 0xb8,15,0,0,0,0x0f,0x05,0x0f,0x0b\n");
}
static long now_ns(void) {
    long t[2];
    if (__syscall(228,1,(long)t)!=0) return -1;
    return t[0]*1000000000L+t[1];
}
static int eq(const char*a,const char*b) {
    int i=0;while(a[i] && a[i]==b[i])i++;return a[i]==b[i];
}
static void handler(int sig) { seen++; }
static long sleep_ns(long ns,long *rem) {
    long t[2];t[0]=ns/1000000000L;t[1]=ns%1000000000L;
    return __syscall(35,(long)t,(long)rem);
}
static int interrupted(int mode) {
    long action[4],child,parent,status,rem[2],tv[2],start,end,rc;
    int fds[2];
    int i;char token='R';
    action[0]=(long)handler;action[1]=mode>=3?0x10000000:0;action[2]=(long)restorer;action[3]=0;
    if(__syscall(13,15,(long)action,0,8)!=0)return 32;
    if(__syscall(22,(long)fds)!=0)return 33;
    parent=__syscall(39);seen=0;child=__syscall(57);
    if(child<0)return 34;
    if(child==0) {
        __syscall(3,fds[1]);
        if(__syscall(0,fds[0],(long)&token,1)!=1)__syscall(60,90);
        __syscall(3,fds[0]);
        /* Repeated delivery prevents a pre-entry signal from making an
         * otherwise valid blocking-syscall EINTR experiment race-dependent. */
        for(i=0;i<100;i++) {
            sleep_ns(20000000L,0);
            if(__syscall(62,parent,15)!=0)break;
        }
        __syscall(60,0);return 91;
    }
    __syscall(3,fds[0]);
    if(__syscall(1,fds[1],(long)&token,1)!=1)return 35;
    __syscall(3,fds[1]);
    rem[0]=-1;rem[1]=-1;tv[0]=2;tv[1]=0;start=now_ns();
    if(mode%3==0)rc=sleep_ns(2000000000L,rem);
    else if(mode%3==1)rc=__syscall(7,0,0,2000);
    else rc=__syscall(23,0,0,0,0,(long)tv);
    end=now_ns();
    __syscall(62,child,9);
    do { status=0;parent=__syscall(61,child,(long)&status,0,0); } while(parent==-4);
    if(parent!=child)return 36;
    if(rc!=-4 || seen<1 || start<0 || end<start)return 37;
    if(mode%3==0 && (rem[0]<0 || rem[1]<0 || rem[1]>=1000000000L
        || rem[0]*1000000000L+rem[1]<=0
        || rem[0]*1000000000L+rem[1]>2000000000L))return 38;
    runtime.printf("CLOCKCHECK EINTR mode=%d elapsed_ns=%ld rem=%ld/%ld seen=%d reaped=1\n",mode,end-start,rem[0],rem[1],seen);
    return 0;
}
int main(int argc,char**argv) {
    long start,end,rc,t[2],u[2];int i,mode;
    if(argc!=2)return 1;
    if(eq(argv[1],"ignore")) {
        long action[4];action[0]=1;action[1]=0;action[2]=0;action[3]=0;
        if(__syscall(13,15,(long)action,0,8)!=0)return 11;
        runtime.printf("CLOCKCHECK IGNORE READY\n");
        return sleep_ns(5000000000L,0)!=0;
    }
    if(eq(argv[1],"eintr")) {
        for(mode=0;mode<6;mode++){i=interrupted(mode);if(i)return i;}
        runtime.printf("CLOCKCHECK EINTR PASS\n");return 0;
    }
    start=now_ns();if(start<0)return 2;
    if(eq(argv[1],"nano"))rc=sleep_ns(2000000000L,0);
    else if(eq(argv[1],"poll"))rc=__syscall(7,0,0,1000);
    else if(eq(argv[1],"select")){t[0]=1;t[1]=0;rc=__syscall(23,0,0,0,0,(long)t);}
    else if(eq(argv[1],"short")) {
        rc=0;for(i=0;i<12;i++) {
            long a=now_ns();
            if(sleep_ns(15000000L,0)!=0 || now_ns()-a<15000000L)return 3;
        }
    } else if(eq(argv[1],"zero")) {
        t[0]=0;t[1]=0;
        if(__syscall(35,(long)t,0)!=0 || __syscall(7,0,0,0)!=0
           || __syscall(23,0,0,0,0,(long)t)!=0)return 4;
        t[1]=1000000000L;if(__syscall(35,(long)t,0)!=-22)return 5;
        if(__syscall(228,99,(long)t)!=-22)return 6;
        if(__syscall(228,0,(long)t)!=0 || __syscall(228,7,(long)u)!=0)return 7;
        if(u[0]<t[0] || (u[0]==t[0] && u[1]<t[1]))return 8;
        rc=0;
    } else return 9;
    end=now_ns();if(rc!=0 || end<start)return 10;
    runtime.printf("CLOCKCHECK %s elapsed_ns=%ld\n",argv[1],end-start);
    return 0;
}
