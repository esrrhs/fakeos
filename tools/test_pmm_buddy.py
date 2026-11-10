#!/usr/bin/env python3
"""Run the real PMM buddy core with only HHDM/IRQ hardware adapted for native tests."""
import os
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
package pmmtest;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
static unsigned char test_memory[33554432];
'''
HARNESS = r'''
char _kernel_end;
u8 boot_pml4[4096];
void tlb_flush(void) {}
u64 vmm_kernel_root(void) { return 0; }
void vmm_hhdm_extend(u64 root,u64 start,u64 end) {}
u64 cpu_irq_save(void) { return 0; }
void cpu_irq_restore(u64 flags) {}
void spin_lock(volatile u32 *p) {}
void spin_unlock(volatile u32 *p) {}
static void reset_arena(void) {
 u32 i;
 for(i=0;i<MAX_FRAMES;i++)frame_state[i]=ST_RESERVED;
 for(i=0;i<=MAX_ORDER;i++)free_head[i]=NIL;
 highest_pfn=8192;managed_total=0;free_count=0;
 add_run(4096,8192); /* one private aligned 16 MiB arena, no physical zero */
}
static int cycle(u32 order,u32 mode) {
 u64 blocks[4096];u32 n=4096>>order;u32 i;u64 full;
 reset_arena();
 for(i=0;i<n;i++) {
  blocks[i]=pmm_alloc_pages(order);
  if(blocks[i]!=(u64)(4096+(i<<order))*4096)return 10;
 }
 if(free_count || pmm_alloc_pages(order))return 11;
 for(i=0;i<n;i++) {
  u32 at=mode==0?i:(mode==1?n-1-i:((i&1)?n-1-i/2:i/2));
  pmm_free_pages(blocks[at],order);
 }
 if(free_count!=4096)return 12;
 full=pmm_alloc_pages(12);
 if(full!=16777216) {
  return 13;
 }
 pmm_free_pages(full,12);
 if(free_count!=4096 || free_head[12]!=4096)return 14;
 for(i=0;i<12;i++)if(free_head[i]!=NIL)return 15;
 for(i=4097;i<8192;i++)if(frame_state[i]!=ST_INTERIOR)return 16;
 return 0;
}
int main(void) {
 u32 order;u32 mode;int rc;
 for(order=0;order<=11;order++)for(mode=0;mode<3;mode++) {
  rc=cycle(order,mode);if(rc)return rc;
 }
 /* A reserved page must prevent a cross-hole merge, even after all
  * surrounding pages are returned in ascending order. */
 reset_arena();
 for(order=0;order<=MAX_ORDER;order++)free_head[order]=NIL;
 for(order=4096;order<8192;order++)frame_state[order]=ST_RESERVED;
 managed_total=0;free_count=0;
 add_run(4096,6144);add_run(6145,8192);
 u64 pages[4095];u32 i;
 *phys32(25165824)=0x1234abcd;
 for(i=0;i<4095;i++){pages[i]=pmm_alloc_page();if(!pages[i])return 17;}
 if(pmm_alloc_page())return 18;
 for(i=0;i<4095;i++)pmm_free_page(pages[i]);
 if(free_count!=4095 || pmm_alloc_pages(12) || *phys32(25165824)!=0x1234abcd)return 19;
 if(pmm_alloc_pages(11)!=16777216)return 22;
 reset_arena();
 if(pmm_alloc_pages(13) || pmm_alloc_pages(0xffffffffu))return 20;
 if(free_count!=4096)return 21;
 return 0;
}
'''


def main():
    source = (ROOT / 'mem/pmm.c').read_text()
    source = source.replace('package mem;\n', '').replace('import types;\n', '')
    for name in ('uint8_t', 'uint16_t', 'uint32_t', 'uint64_t'):
        source = re.sub(r'typedef types\.' + name + r'\s+u\d+;\n', '', source)
    old = 'return (volatile u32 *)(HHDM_BASE + paddr);'
    assert source.count(old) == 1
    source = source.replace(old, 'return (volatile u32 *)(test_memory + paddr);')
    compiler = os.environ.get('FCC', '/tmp/fakecc-pr116-merge/build/fakecc')
    with tempfile.TemporaryDirectory(prefix='fakeos-pmm-unit-') as tmp:
        src = Path(tmp) / 'pmm-test.c'
        exe = Path(tmp) / 'pmm-test'
        src.write_text(PRELUDE + source + HARNESS)
        subprocess.run([compiler, str(src), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)])
        assert result.returncode == 0, "real PMM buddy test exit=" + str(result.returncode) + " (13=4096 free pages cannot form one16MiB block; first case=order0 ascending free)"
    print("PASS: real PMM orders0..11 ascending/reverse/interleaved free; OOM/bounds/reserved-hole/full16MiB coalescence")


if __name__ == '__main__':
    main()
