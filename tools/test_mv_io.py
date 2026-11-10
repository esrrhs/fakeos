#!/usr/bin/env python3
"""Compile the real mv implementation with fakecc and injected I/O outcomes."""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
PRE='package test;\nstatic int scenario,reads,writes,written,closes,truncates,unlinks,gone,opens,open_trunc,removed,links,link_reads,cwd_reads,cwd_changes,exits;\n'
STUB=r'''
unsigned long strlen(const char*s){unsigned long n=0;while(s[n])n++;return n;}
int strcmp(const char*a,const char*b){while(*a && *a==*b){a++;b++;}return (int)*a-(int)*b;}
char *strcpy(char*d,const char*s){int i=0;while((d[i]=s[i]))i++;return d;}
void printf(const char*fmt,...){ }
long u_open(const char*p,long flags,long mode){opens++;if(flags&0x200)open_trunc++;if((scenario==12 && !flags)||(scenario==13 && flags))return -1;return flags?4:3;}
long u_close(long fd){closes++;if((scenario==7 && fd==3)||(scenario==8 && fd==4)||scenario>=22)return -5;return 0;}
long u_read(long fd,void *buf,unsigned long cap){
 int i;char*p=(char*)buf;reads++;
 if(scenario==20)return -5;
 if(scenario==2 && reads==1)return -4;
 if(scenario==21 && reads==2)return -4;
 if(scenario==1 && reads==2)return -5;
 if(reads>(scenario==2?2:1))return 0;
 for(i=0;i<16;i++)p[i]=(char)('A'+i);return 16;
}
long u_write(long fd,const void*buf,unsigned long len){
 const char*p=(const char*)buf;int i;long n=(long)len;writes++;
 if(scenario==3 && writes==1)return -4;
 if(scenario==5)return 0;if(scenario==6)return -5;if(scenario==16)return (long)len+1;
 if(scenario==4 && n>3)n=3;
 for(i=0;i<n;i++)if(p[i]!=(char)('A'+written+i))return -5;
 written+=(int)n;return n;
}


static long guard_stat(const char*p,void*buf,int nofollow){
 char*b=(char*)buf;int i;int dev=1,ino=11,mode=0x40;
 if(scenario==104 && strcmp(p,"/src")==0)return -5;
 if(scenario==110 && strcmp(p,"/")==0)return -5;
 if(strcmp(p,"/src")==0 || strcmp(p,"/mnt/home/src")==0)ino=10;
 else if(strcmp(p,"/")==0)ino=1;
 else if(strcmp(p,"/outside/new")==0 || strcmp(p,"/alias/new")==0 || strcmp(p,"/mnt/home/src/sub/new")==0){if(scenario<112)return -1;}
 if(scenario==103 && strcmp(p,"/outside")==0){ino=10;dev=2;}
 if(scenario==109 && nofollow && strcmp(p,"/alias")==0)mode=0xA0;
 for(i=0;i<144;i++)b[i]=0;b[0]=(char)dev;b[8]=(char)ino;b[25]=(char)mode;return 0;
}
long u_getcwd(char*out,unsigned long cap){
 cwd_reads++;if((scenario==105&&cwd_reads==1)||(scenario==107&&cwd_reads==2))return -5;
 if(cwd_reads==1)strcpy(out,"/oldcwd");
 else if(scenario==101)strcpy(out,"/src/sub");
 else if(scenario==102)strcpy(out,"/mnt/home/src/sub");
 else if(scenario==109)strcpy(out,"/alias/sub");
 else strcpy(out,"/outside");return 0;
}
long u_chdir(const char*p){cwd_changes++;return (scenario==106&&cwd_changes==1)||(scenario==108&&cwd_changes==2)?-5:0;}
void u_exit(long code){exits++;}
long u_lstat(const char*p,void*buf){
 char*b=(char*)buf;int i;
 if(scenario>=100)return guard_stat(p,buf,1);
 if(scenario>=24){
  if(strcmp(p,"/dst")==0 && scenario!=26 && scenario!=29)return -1;
  for(i=0;i<144;i++)b[i]=0;b[25]=(char)0xA0;return 0;
 }
 return u_stat(p,buf);
}
long u_readlink(const char*p,char*buf,unsigned long len){
 const char*t="target";int i;link_reads++;if(scenario==25)return -5;
 for(i=0;i<6;i++)buf[i]=t[i];return 6;
}
long u_symlink(const char*t,const char*p){if(strcmp(t,"target")!=0)return -5;links++;return scenario==27?-5:0;}
long u_fstat(long fd,void*buf){
 char*p=(char*)buf;int i;if((scenario==9 && fd==3)||(scenario==10 && fd==4))return -5;
 for(i=0;i<144;i++)p[i]=0;
 p[0]=(scenario==15 && fd==4)?2:1;
 p[8]=(scenario==14 || scenario==15)?9:(fd==3?9:10);p[25]=(scenario==18 && fd==3)||(scenario==19 && fd==4)?0x20:(char)0x80;
 return 0;
}
long u_ftruncate(long fd,unsigned long size){truncates++;return scenario==11?-5:0;}
long u_unlink(const char*p){unlinks++;if(scenario==17)return -5;gone++;return 0;}
long u_rename(const char*a,const char*b){return scenario==113?0:-1;}
long u_stat(const char*p,void*buf){if(scenario>=100)return guard_stat(p,buf,0);char*b=(char*)buf;int i;for(i=0;i<144;i++)b[i]=0;b[25]=0x40;return 0;}
long u_mkdir(const char*p){return 0;}long u_rmdir(const char*p){removed++;return 0;}
long u_getdents(long fd,void*p,unsigned long len){return 0;}
long u_dirent_reclen(const char*p,unsigned long avail){return -1;}
int main(void){
 int s;
 for(s=0;s<24;s++){
  int rc;int success=(s==0||s==2||s==3||s==4||s==15||s==21);
  scenario=s;reads=0;writes=0;written=0;closes=0;truncates=0;unlinks=0;gone=0;opens=0;open_trunc=0;removed=0;
  rc=s==22?copy_dir_rec("/src","/dst"):(s==23?rm_dir_rec("/src"):copy_unlink("/src","/dst"));
  if((rc==0)!=success)return 10+s;
  if(open_trunc)return 40+s;
  if(success){if(written!=16 || gone!=1 || unlinks!=1 || closes!=2 || truncates!=1)return 70+s;}
  else {if(gone || (s!=17 && unlinks))return 100+s;}
  if((s==9||s==10||s==12||s==13||s==14||s==18||s==19) && truncates)return 130+s;
  if((s==10||s==11||s==14||s==19) && (reads||writes))return 160+s;
  if(s>=22 && removed)return 190+s;
 }
 for(s=24;s<30;s++){
  int rc;scenario=s;reads=0;writes=0;unlinks=0;gone=0;removed=0;links=0;link_reads=0;
  rc=s==28?rm_path("/src"):move_one("/src","/dst",s==29,0);
  if((rc==0)!=(s==24||s==28||s==29))return 220+s;
  if(reads||writes||removed)return 250+s;
  if(s==24){if(links!=1||gone!=1||link_reads!=1)return 280+s;}
  else if(s==28){if(gone!=1||links||link_reads)return 310+s;}
  else if(s==27){if(links!=1||gone)return 340+s;}
  else if(links||gone)return 370+s;
 }
 for(s=100;s<114;s++){
  int ok;char longpath[520];const char*dst="/outside/new";int i;
  scenario=s;cwd_reads=0;cwd_changes=0;exits=0;opens=0;removed=0;unlinks=0;
  if(s==101||s==109)dst="/alias/new";
  if(s==102)dst="/mnt/home/src/sub/new";
  if(s==111){for(i=0;i<519;i++)longpath[i]='x';longpath[519]=0;dst=longpath;}
  if(s<112){
   ok=directory_target_safe("/src",dst);
   if(ok!=(s==100||s==103))return 400+s;
   if(s==108 && exits!=1)return 520+s;
   if(s==107 && cwd_changes!=2)return 640+s;
  }else{
   int rc=move_one("/src",dst,0,0);
   if((rc==0)!=(s==113)||opens||removed||unlinks)return 760+s;
  }
 }
 return 0;
}
'''
def main():
    source=(ROOT/'user/mv.c').read_text().replace('package user;','').replace('int main(int argc, char **argv)', 'int mv_program_main(int argc, char **argv)')
    with tempfile.TemporaryDirectory(prefix='fakeos-mv-unit-') as temp:
        code=Path(temp)/'test.c';binary=Path(temp)/'test'
        code.write_text(PRE+source+STUB)
        subprocess.run([os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc'),str(code),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('PASS: actual mv44 cases (24 I/O +6 no-follow +14 physical alias/ancestor/cwd/fail-closed/merge/rename cases): partial read errors, read/write EINTR, short/zero/failed/oversized writes, source/destination close/stat/open/truncate errors, descriptor aliases/device IDs, special-file refusal, unlink failure and directory-close failure retain source')
if __name__=='__main__':main()
