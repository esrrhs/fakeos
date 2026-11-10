#!/usr/bin/env python3
"""Pure-fakecc tests of the real kernel writer and both bounded decoders."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
int main(void) {
 char guarded[320];char name[257];int sizes[8]={1,27,28,29,30,31,254,255};
 int t,i,j;unsigned int n;
 for(i=0;i<256;i++)name[i]='n';name[256]=0;
 for(t=0;t<8;t++) {
  int len=sizes[t];char saved=name[len];name[len]=0;
  for(i=0;i<320;i++)guarded[i]=(char)0xa5;
  n=dirent_size(len);
  if(!n || n>280 || (n&7))return 1;
  if(dirent_write(guarded+8,n-1,12,99,8,name,len))return 2;
  for(i=0;i<320;i++)if((unsigned char)guarded[i]!=0xa5)return 3;
  if(dirent_write(guarded+8,n,12,99,8,name,len)!=n)return 4;
  if(dirent_record_size(guarded+8,n)!=n || u_dirent_reclen(guarded+8,n)!=(long)n)return 5;
  for(i=0;i<8;i++)if((unsigned char)guarded[i]!=0xa5 || (unsigned char)guarded[8+n+i]!=0xa5)return 6;
  for(i=0;i<len;i++)if(guarded[27+i]!='n')return 7;
  for(i=27+len;i<8+n;i++)if(guarded[i])return 8;
  for(j=0;j<(int)n;j++)if(dirent_record_size(guarded+8,j) || u_dirent_reclen(guarded+8,j)>=0)return 9;
  name[len]=saved;
 }
 if(dirent_size(0) || dirent_size(256) || dirent_size(0xffffffffu))return 10;
 for(i=0;i<320;i++)guarded[i]=0;
 guarded[16]=48;guarded[19]='x';
 if(u_dirent_reclen(guarded,48)!=48)return 11;
 guarded[19]=0;if(u_dirent_reclen(guarded,48)>=0)return 12;
 guarded[19]='/';if(u_dirent_reclen(guarded,48)>=0)return 13;
 for(i=19;i<48;i++)guarded[i]='x';
 if(dirent_record_size(guarded,48) || u_dirent_reclen(guarded,48)>=0)return 14;
 guarded[19]=0;guarded[16]=0;
 if(u_dirent_reclen(guarded,48)>=0)return 15;
 guarded[16]=47;if(u_dirent_reclen(guarded,48)>=0)return 16;
 guarded[16]=56;if(u_dirent_reclen(guarded,48)>=0)return 17;
 name[0]=0;if(dirent_write(guarded,320,1,1,8,name,1))return 18;
 name[0]='/';if(dirent_write(guarded,320,1,1,8,name,1))return 19;
 if(dirent_write(guarded,320,1,1,8,name,256))return 20;
 return 0;
}
'''
def main():
    kernel = (ROOT/'fs/dirent.c').read_text().replace('package fs;', 'package test;').replace('import types;', '')
    kernel = kernel.replace('types.uint32_t', 'unsigned int').replace('types.uint64_t', 'unsigned long')
    libc = (ROOT/'user/libc.c').read_text().split('/* Validated getdents64 decoder:',1)[1]
    libc = libc[libc.index('long u_dirent_reclen'):]
    with tempfile.TemporaryDirectory(prefix='fakeos-dirent-unit-') as tmp:
        source=Path(tmp)/'test.c';binary=Path(tmp)/'test'
        source.write_text(kernel+libc+HARNESS)
        subprocess.run([os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc'),str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('PASS: actual dirent writer + kernel/libc decoders: exact/short buffers, guards, padding, 1..255-byte names, invalid/unterminated/over-limit records')
if __name__=='__main__':main()
