#!/usr/bin/env python3
"""Run the actual Ext2 insertion core with private in-memory block I/O."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PRE=r'''
package test;
typedef unsigned char u8;typedef unsigned short u16;typedef unsigned int u32;
enum {NAME_MAX=255,EXT2_S_IFMT=0xf000,EXT2_S_IFDIR=0x4000,I_MODE=0,I_SIZE_LO=4};
struct super {u32 block_size;};static struct super sb;
static u8 inode[128],ino_raw[128],blk_buf[4096],disk[4][1024];
static int reads;static int allocations;
static u16 rd16(const u8 *p){return p[0]|((u16)p[1]<<8);}
static u32 rd32(const u8 *p){return p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);}
static void wr16(u8*p,u16 n){p[0]=(u8)n;p[1]=(u8)(n>>8);}
static void wr32(u8*p,u32 n){wr16(p,(u16)n);wr16(p+2,(u16)(n>>16));}
static int inode_read(u32 n,u8*p){u32 i;reads++;for(i=0;i<128;i++)p[i]=inode[i];return 1;}
static int inode_write(u32 n,u8*p){u32 i;for(i=0;i<128;i++)inode[i]=p[i];return 1;}
static u32 inode_map(u8*p,u32 f,int create){if(f>=4)return 0;if(f>=2)allocations++;return f+1;}
static int bread(u32 n,u8*p){u32 i;for(i=0;i<1024;i++)p[i]=disk[n-1][i];return 1;}
static int bwrite(u32 n,u8*p){u32 i;for(i=0;i<1024;i++)disk[n-1][i]=p[i];return 1;}
'''
POST=r'''
static void full_block(u8 *p) {
 u32 at=0;int j;
 for(j=0;j<4;j++) {
  u16 n=j==3?232:264;u32 len=j==3?220:255;u32 k;
  wr32(p+at,20+j);wr16(p+at+4,n);p[at+6]=(u8)len;p[at+7]=1;
  for(k=0;k<len;k++)p[at+8+k]='x';at+=n;
 }
}
int main(void) {
 char name[257];u32 i;
 sb.block_size=1024;wr16(inode,EXT2_S_IFDIR|0755);wr32(inode+4,2048);
 full_block(disk[0]);
 wr32(disk[1],11);wr16(disk[1]+4,1024);disk[1][6]=255;disk[1][7]=1;
 for(i=0;i<255;i++){disk[1][8+i]='n';name[i]='d';}name[255]=0;
 if(!dir_add(2,name,255,12,2))return 1;
 if(rd32(inode+4)!=2048 || rd32(disk[1])!=11)return 2;
 for(i=0;i<255;i++)if(disk[1][8+i]!='n')return 3;
 if(rd32(disk[1]+264)!=12 || disk[1][264+6]!=255)return 4;
 if(allocations)return 5;
 full_block(disk[1]);
 if(!dir_add(2,name,255,13,1))return 6;
 if(rd32(inode+4)!=3072 || rd32(disk[1])!=20 || rd32(disk[2])!=13)return 7;
 if(allocations!=1)return 8;
 reads=0;if(dir_add(2,name,256,14,1) || reads)return 9;
 /* Malformed occupied/free records cannot underflow slack or be rewritten. */
 wr16(disk[0]+4,8);disk[0][6]=255;
 if(dir_add(2,name,255,14,1) || rd16(disk[0]+4)!=8 || rd32(disk[0])!=20)return 10;
 wr16(disk[0]+4,263);disk[0][6]=1;
 if(dir_add(2,name,255,14,1) || rd16(disk[0]+4)!=263)return 11;
 return 0;
}
'''
def main():
    text=(ROOT/'fs/ext2.c').read_text()
    start=text.index('static u32 dir_add(')
    end=text.index('\nstatic u32 dir_remove(',start)
    with tempfile.TemporaryDirectory(prefix='fakeos-ext2-unit-') as temp:
        source=Path(temp)/'test.c';binary=Path(temp)/'test'
        source.write_text(PRE+text[start:end]+POST)
        subprocess.run([os.environ.get('FCC','/tmp/fakecc-pr116-merge/build/fakecc'),str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('PASS: actual Ext2 dir_add preserves populated later blocks, extends only at real EOF, retains max255 names and rejects256')
if __name__=='__main__':main()
