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
static int dir_reads;
static int cwd_reads, cwd_changes, exits, links;
static long fake_stat(const char *p,void *buf,int nofollow) {
 char *b=(char *)buf;int i;int ino=11;int dev=1;int mode=0x40;
 if(scenario<100)return -1;
 if(scenario>=200) { if(strcmp(p,"/src")==0)ino=10;else if(strcmp(p,"/")==0)ino=1; }
 else if(scenario>=115) {
  if(scenario==115 || scenario==116 || scenario==121 || scenario==122)return -1;
  mode=scenario==117?0x80:0xA0;
 } else if(strcmp(p,"/src")==0) {
  if(scenario==111)return -1;
  ino=10;
 } else if(strcmp(p,"/outside/new")==0)return -1;
 else if(strcmp(p,"/")==0)ino=1;
 else if(scenario==109 && !nofollow)return -1;
 else if(scenario==102 && strcmp(p,"/mnt/src")==0)ino=10;
 else if(scenario==106 && strcmp(p,"/alias")==0)mode=0xA0;
 else if(scenario==112) { ino=10;dev=2; }
 for(i=0;i<144;i++)b[i]=0;
 b[0]=(char)dev;b[8]=(char)ino;b[25]=(char)mode;
 return 0;
}
void printf(const char *fmt, ...) { }
unsigned long strlen(const char *s) { unsigned long n=0; while(s[n]) n++; return n; }
int strcmp(const char *a,const char *b) { while(*a && *a==*b){a++;b++;} return (int)*a-(int)*b; }
char *strcpy(char *d,const char *s) { char *r=d; while((*d++=*s++)){} return r; }
long u_open(const char *p,long f,long m) { return strcmp(p,"src")==0 ? 3 : 4; }
long u_stat(const char *p,void *b) { return fake_stat(p,b,0); }
long u_lstat(const char *p,void *b) { return fake_stat(p,b,1); }
long u_mkdir(const char *p) { return -1; }
long u_exit(long code) { exits++;return code; }
long u_chdir(const char *p) {
 cwd_changes++;
 if(scenario==104 || (scenario==107 && cwd_changes==2))return -1;
 return 0;
}
long u_getcwd(char *p,unsigned long n) {
 const char *v="/outside";
 cwd_reads++;
 if(scenario==103 || (scenario==105 && cwd_reads==2))return -1;
 if(cwd_reads==1)v="/original";
 else if(scenario==101)v="/src/sub";
 else if(scenario==102)v="/mnt/src";
 else if(scenario==106)v="/alias/sub";
 else if(scenario==108)v="relative";
 else if(scenario==110)v="/";
 strcpy(p,v);return (long)strlen(v)+1;
}
long u_readlink(const char *p,char *b,unsigned long n) {
 if(scenario==115)return -1;
 if(scenario==116)return (long)n;
 strcpy(b,(scenario==119 && strcmp(p,"dst")==0)?"different":"..");return (long)strlen(b);
}
long u_symlink(const char *t,const char *p) {
 links++;
 if(strcmp(t,"..")!=0 || strcmp(p,"dst")!=0)bad_write=1;
 return scenario==121?-1:0;
}
long u_getdents(long fd,void *p,unsigned long n) {
 char *b=(char *)p;int i;dir_reads++;
 if(scenario<200 || scenario==200)return -1;
 if(scenario==201)return dir_reads==1?-4:-1;
 if(scenario==207 || scenario==208)return 0;
 if(scenario==202 && dir_reads>1)return -1;
 for(i=0;i<56;i++)b[i]=0;
 b[16]=48; b[19]='c'; b[20]=0;
 if(scenario==203)b[16]=0;
 if(scenario==204)b[16]=56;
 if(scenario==205)for(i=19;i<48;i++)b[i]='x';
 if(scenario==206)b[16]=47;
 return scenario==209?49:48;
}
long u_fstat(long fd,void *buf) {
 char *b=(char *)buf; int i;
 if(scenario==7 && fd==4) return -1;
 for(i=0;i<144;i++)b[i]=0;
 b[0]=(scenario==10 && fd==4)?2:1; b[8]=(fd==3 || scenario==1 || scenario==10) ? 10 : 11; b[25]=(char)0x80;
 return 0;
}
long u_ftruncate(long fd,unsigned long n) { truncates++; return scenario==8 ? -1 : 0; }
long u_close(long fd) { closes++; return (scenario==9 && fd==4) || scenario==207 ? -1 : 0; }
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
 int i; int rc; int errors=0; char overlong[PATH_CAP + 8];
 for(i=0;i<PATH_CAP+7;i++)overlong[i]='a'; overlong[0]='/';overlong[PATH_CAP+7]=0;
 for(i=1;i<=11;i++) {
  scenario=i; reads=0;written_total=0;truncates=0;closes=0;bad_write=0;write_calls=0;
  rc=copy_file("src","dst",0,1);
  if(rc!=((i==3 || i==6 || i==10 || i==11)?0:1))errors++;
  if(closes!=2 || bad_write)errors++;
  if((i==1 || i==7) && (truncates || reads || written_total))errors++;
  if((i==3 || i==6 || i==10 || i==11) && written_total!=5)errors++;
 }
 /* Resolver failures must fail closed and restore the utility's cwd. */
 for(i=100;i<=114;i++) {
  scenario=i;cwd_reads=0;cwd_changes=0;exits=0;
  rc=directory_target_safe("/src",i==113?overlong:(i==114?"":"/outside/new"));
  if(rc!=((i==100 || i==110 || i==112)?1:0))errors++;
  if(i==107 && exits!=1)errors++;
  if(i!=107 && exits)errors++;
  if((i==103 || i==111 || i==113 || i==114) && cwd_changes)errors++;
  if(i==104 && cwd_changes!=1)errors++;
  if(i==105 && cwd_changes!=2)errors++;
 }
 for(i=115;i<=122;i++) {
  scenario=i;links=0;bad_write=0;
  rc=copy_link("src","dst",i==120,0);
  if(rc!=((i==118 || i==120 || i==122)?0:1))errors++;
  if(links!=((i==121 || i==122)?1:0) || bad_write)errors++;
 }
 for(i=200;i<=209;i++) {
  scenario=i;cwd_reads=0;cwd_changes=0;closes=0;dir_reads=0;
  rc=copy_dir_rec("/src","/dst",0,0);
  if(rc!=(i==208?0:1) || closes!=1)errors++;
  if(i==201 && dir_reads!=2)errors++;
  if(i==202 && dir_reads!=2)errors++;
 }
 return errors;
}
'''

def main():
    compiler = os.environ.get('FCC', '/tmp/fakecc-pr116-merge/build/fakecc')
    with tempfile.TemporaryDirectory(prefix='fakeos-cp-unit-') as tmp:
        src = Path(tmp) / 'cp_unit.c'
        code = (ROOT / 'user/cp.c').read_text().replace('int main(int argc, char **argv)', 'int cp_main(int argc, char **argv)')
        decoder = (ROOT / 'user/libc.c').read_text().split('/* Validated getdents64 decoder:', 1)[1]
        decoder = decoder[decoder.index('long u_dirent_reclen'):]
        src.write_text(code + decoder + HARNESS)
        exe = Path(tmp) / 'cp_unit'
        subprocess.run([compiler, str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: real cp: identity/I/O/EINTR/close errors; physical descendant/alias/cross-device resolution; fail-closed cwd restoration; link overwrite/create failures; ten directory read/record/close error scenarios')

if __name__ == '__main__':
    main()
