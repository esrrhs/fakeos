package main;
import runtime;
/* Regression: allocator chunks may span adjacent mmap VMAs. A copy must
 * accept complete coverage, reject holes and not consume a rejected chunk. */
int main(void) {
 long a=__syscall(9,0,1048576,3,34,-1,0);
 long b=__syscall(9,0,1048576,3,34,-1,0);
 if(a<0 || b!=a+1048576) return 2;
 long fd=__syscall(2,(long)"/src/mem/as.c",0,0);
 long n=__syscall(0,fd,a+1048576-16384+16,20000);
 long off=__syscall(8,fd,0,1);
 runtime.printf("VMAPROBE a=%p b=%p n=%ld off=%ld expected=20000\n",a,b,n,off);
 __syscall(8,fd,0,0);
 char *ref=(char *)runtime.malloc(20000);
 long nr=__syscall(0,fd,(long)ref,20000);
 int same=nr==20000 && runtime.memcmp((char *)(a+1048576-16384+16),ref,20000)==0;
 runtime.printf("VMAPROBE content_equal=%d\n",same);
 __syscall(11,b,1048576);
 __syscall(8,fd,0,0);
 long hole=__syscall(0,fd,a+1048576-16384+16,20000);
 long ho=__syscall(8,fd,0,1);
 runtime.printf("VMAPROBE gap_n=%ld gap_offset=%ld expected=12288/12288\n",hole,ho);
 if(n!=20000 || off!=20000 || !same || hole!=12288 || ho!=12288) return 1;
 runtime.printf("VMAIO adjacency/content/hole/offset OK\n");
 return 0;
}
