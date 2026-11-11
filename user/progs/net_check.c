/* Raw-ABI NIC integrity fixture, guest-compiled with fakecc. */
package main;
import runtime;
static long u_socket(long a,long b,long c) { return __syscall(41,a,b,c); }
static long u_connect(long a,const void*b,unsigned long c) {
 return __syscall(42,a,(long)b,c);
}
static long u_sendto(long a,const void*b,unsigned long c,long d,const void*e,unsigned long f) {
 return __syscall(44,a,(long)b,c,d,(long)e,f);
}
static long u_recvfrom(long a,void*b,unsigned long c,long d,void*e,unsigned int*f) {
 return __syscall(45,a,(long)b,c,d,(long)e,(long)f);
}
static long u_shutdown(long a,long b) { return __syscall(48,a,b); }
static long u_poll(void*a,unsigned long b,long c) { return __syscall(7,(long)a,b,c); }
static long u_read(long a,void*b,unsigned long c) { return __syscall(0,a,(long)b,c); }
static long u_write(long a,const void*b,unsigned long c) { return __syscall(1,a,(long)b,c); }
static long u_close(long a) { return __syscall(3,a); }
static long u_nanosleep(const void*a,void*b) { return __syscall(35,(long)a,(long)b); }
struct pf { int fd; short events; short revents; };
static unsigned char buf[1024];
static void addr(unsigned char *a,int p) {
 int i; for(i=0;i<16;i++) a[i]=0;
 a[0]=2; a[2]=(unsigned char)(p>>8); a[3]=(unsigned char)p;
 a[4]=10; a[6]=2; a[7]=2;
}
static int ready(long s) {
 struct pf p; p.fd=(int)s; p.events=1; p.revents=0;
 return (int)u_poll(&p,1,5000);
}
static int exercise(int mode,int p) {
 unsigned char a[16]; long s,r,n; int i,total=0,bad=0; long ts[2];
 addr(a,p); s=u_socket(2,mode==117?2:1,0); if(s<0) return 3;
 if(mode==117) {
  for(i=0;i<1024;i++) buf[i]=(unsigned char)(i%251);
  r=u_sendto(s,buf,1024,0,a,16); if(r!=1024) return 4;
  if(ready(s)<=0) return 5;
  r=u_recvfrom(s,buf,1024,0,0,0);
  for(i=0;i<r;i++) if(buf[i]!=(unsigned char)((i%251)^90)) bad++;
  runtime.printf("NETCHECK udp received=%ld bad=%d\n",r,bad); u_close(s);
  return r==1024 && !bad ? 0:6;
 }
 r=u_connect(s,a,16);
 if(mode==114) { runtime.printf("NETCHECK refused result=%ld\n",r); u_close(s); return r<0?0:7; }
 if(r<0) { runtime.printf("NETCHECK connect error=%ld\n",r); return 8; }
 if(mode==116) {
  for(total=0;total<4096;total+=512) {
   for(i=0;i<512;i++) buf[i]=(unsigned char)((total+i)%251);
   r=u_write(s,buf,512); if(r!=512) return 9;
   n=0; while(n<512) {
    if(ready(s)<=0) return 10;
    r=u_read(s,buf,512-n); if(r<=0) return 11;
    for(i=0;i<r;i++) if(buf[i]!=(unsigned char)(((total+n+i)%251)^90)) bad++;
    n+=r;
   }
  }
  if(u_shutdown(s,1)<0) return 12;
  if(ready(s)<=0) return 13;
  r=u_read(s,buf,1024);
  runtime.printf("NETCHECK tcp bytes=%d bad=%d eof=%ld\n",total,bad,r); u_close(s);
  return !bad && !r ? 0:14;
 }
 if(u_read(s,buf,0)!=0) return 32;
 ts[0]=1; ts[1]=0; u_nanosleep(ts,0);
 if(u_read(s,(void*)1,32)>=0) return 33;
 while(total<8192) {
  if(ready(s)<=0) { runtime.printf("NETCHECK burst timeout bytes=%d\n",total); return 15; }
  r=u_read(s,buf,1024); if(r<0) return 16; if(!r) break;
  for(i=0;i<r;i++) if(buf[i]!=(unsigned char)((total+i)%251)) bad++;
  total+=r;
 }
 runtime.printf("NETCHECK burst expected=8192 received=%d bad=%d\n",total,bad);
 u_close(s); return total==8192 && !bad ? 0:17;
}

int main(int argc,char **argv) {
 long kids[3],status,r; int i,p=0,rc=0;
 if(argc!=3) return 2;
 for(i=0;argv[2][i];i++) p=p*10+argv[2][i]-48;
 if(argv[1][0]!=99) return exercise(argv[1][0],p);
 for(i=0;i<3;i++) kids[i]=0;
 for(i=0;i<3;i++) {
  kids[i]=__syscall(57);
  if(kids[i]==0) { rc=exercise(98,p); __syscall(60,rc); return 99; }
  if(kids[i]<0) { rc=30; break; }
 }
 for(i=0;i<3;i++) {
  if(kids[i]<=0) continue;
  do { status=0; r=__syscall(61,kids[i],(long)&status,0,0); } while(r==-4);
  if(r!=kids[i] || status) rc=31;
 }
 runtime.printf("NETCHECK concurrent children=3 reaped rc=%d\n",rc);
 return rc;
}
