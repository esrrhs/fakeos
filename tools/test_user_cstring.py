#!/usr/bin/env python3
"""Actual user-string copy core; only page/VMA lookup is simulated."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PRE=r'''
package test;
typedef unsigned long u64;typedef unsigned int u32;
static char arena[16384];static u64 first;static int reject_second;
static u64 sched_current_as_h(void){return 1;}
static u32 range_ok(u64 as,u64 base,u64 len) {
 return as==1 && len==4096 && base>=first && base<first+(reject_second?4096:8192);
}
'''
POST=r'''
int main(void) {
 char out[544];char *p;int k,i,j;int limits[10]={0,1,126,127,128,254,255,510,511,512};
 first=((u64)arena+4095)&~4095UL;p=(char *)(first+128);
 for(k=0;k<10;k++)for(j=-1;j<=1;j++) {
  int max=limits[k];int n=max+j;if(n<0)continue;
  for(i=0;i<n;i++)p[i]='a';p[n]=0;
  for(i=0;i<544;i++)out[i]=(char)0xa5;
  if(copy_user_cstring(out+8,(u64)p,max)!=(u32)(n<=max))return 2;
  for(i=0;i<8;i++)if((unsigned char)out[i]!=0xa5)return 3;
  for(i=8+max+1;i<544;i++)if((unsigned char)out[i]!=0xa5)return 4;
  if(n<=max){for(i=0;i<n;i++)if(out[8+i]!='a')return 5;if(out[8+n])return 6;}
 }
 p=(char *)(first+4090);for(i=0;i<20;i++)p[i]='z';p[20]=0;
 if(!copy_user_cstring(out,(u64)p,20))return 7;
 reject_second=1;if(copy_user_cstring(out,(u64)p,20))return 8;
 p=(char *)(first+4095);p[0]=0;
 if(!copy_user_cstring(out,(u64)p,0))return 9;
 if(copy_user_cstring(out,first-4096,511))return 10;
 return 0;
}
'''
def main():
    source=(ROOT/'kernel/sysfile.c').read_text()
    start=source.index('u32 copy_user_cstring(')
    end=source.index('\n\n\n/* Kernel readlink',start)
    core=source[start:end].replace('mem.as_user_range_ok','range_ok')
    with tempfile.TemporaryDirectory(prefix='fakeos-cstring-unit-') as temp:
        code=Path(temp)/'test.c';binary=Path(temp)/'test'
        code.write_text(PRE+core+POST)
        subprocess.run([os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc'),str(code),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('PASS: actual copy_user_cstring accepts exact max including511/127, rejects max+1, preserves guards and checks page crossing/faults')
if __name__=='__main__':main()
