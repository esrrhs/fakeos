#!/usr/bin/env python3
"""Pure-fakecc unit coverage for cp syscall errors/short writes, not a guest substitute."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
static int scenario;
static int reads;
static int written_total;
static int truncates;
static int closes;
static int bad_write;
static int write_calls;
void printf(const char *fmt, ...) { }
unsigned long strlen(const char *s) { unsigned long n=0; while(s[n]) n++; return n; }
int strcmp(const char *a,const char *b) { while(*a && *a==*b){a++;b++;} return (int)*a-(int)*b; }
char *strcpy(char *d,const char *s) { char *r=d; while((*d++=*s++)){} return r; }
long u_open(const char *p,long f,long m) { return strcmp(p,"src")==0 ? 3 : 4; }
long u_stat(const char *p,void *b) { return -1; }
long u_lstat(const char *p,void *b) { return -1; }
long u_mkdir(const char *p) { return -1; }
long u_getdents(long fd,void *p,unsigned long n) { return -1; }
long u_fstat(long fd,void *buf) {
 char *b=(char *)buf; int i;
 if(scenario==7 && fd==4) return -1;
 for(i=0;i<144;i++)b[i]=0;
 b[0]=(scenario==10 && fd==4)?2:1; b[8]=(fd==3 || scenario==1 || scenario==10) ? 10 : 11; b[25]=(char)0x80;
 return 0;
}
long u_ftruncate(long fd,unsigned long n) { truncates++; return scenario==8 ? -1 : 0; }
long u_close(long fd) { closes++; return scenario==9 && fd==4 ? -1 : 0; }
long u_read(long fd,void *buf,unsigned long n) {
 char *b=(char *)buf; int i; reads++;
 if(scenario==2)return -1;
 if(scenario==6 && reads==1)return -4;
 if(reads>(scenario==6 ? 2 : 1))return 0;
 for(i=0;i<5;i++)b[i]=(char)('A'+i);
 return 5;
}
long u_write(long fd,const void *buf,unsigned long n) {
 const char *b=(const char *)buf; unsigned long i;
 write_calls++;
 if(scenario==11 && write_calls==1)return -4;
 if(scenario==4)return 0;
 if(scenario==5)return -1;
 if(scenario==3 && n>2)n=2;
 for(i=0;i<n;i++)if(b[i]!=(char)('A'+written_total+(int)i))bad_write=1;
 written_total+=(int)n;
 return (long)n;
}
int main(int argc,char **argv) {
 int i; int rc; int errors=0;
 for(i=1;i<=11;i++) {
  scenario=i; reads=0;written_total=0;truncates=0;closes=0;bad_write=0;write_calls=0;
  rc=copy_file("src","dst",0,1);
  if(rc!=((i==3 || i==6 || i==10 || i==11)?0:1))errors++;
  if(closes!=2 || bad_write)errors++;
  if((i==1 || i==7) && (truncates || reads || written_total))errors++;
  if((i==3 || i==6 || i==10 || i==11) && written_total!=5)errors++;
 }
 return errors;
}
'''

def main():
    compiler = os.environ.get('FCC', '/tmp/fakecc-pr116-merge/build/fakecc')
    with tempfile.TemporaryDirectory(prefix='fakeos-cp-unit-') as tmp:
        src = Path(tmp) / 'cp_unit.c'
        code = (ROOT / 'user/cp.c').read_text().replace('int main(int argc, char **argv)', 'int cp_main(int argc, char **argv)')
        src.write_text(code + HARNESS)
        exe = Path(tmp) / 'cp_unit'
        subprocess.run([compiler, str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: real cp with injected same-inode, read/stat/truncate/close failures, EINTR, short/zero/failed writes')

if __name__ == '__main__':
    main()
