# Gas-subset `.S` assembly (fakeos Should S3)

`fakecc -c foo.S` (also `.s`, `.fakecc.S`) assembles a **gas-compatible AT&T
x86-64 subset** into an ELF `ET_REL` object. Encoding reuses the M1 inline
assembler (`iasm`).

## Directives

| Directive | Support |
|-----------|---------|
| `.text` / `.data` / `.bss` / `.section …` | Section select; code → `.text`, data dirs → `.data`/`.rodata`/`.bss` |
| `.globl` / `.global` | Export label (`STT_FUNC` in `.text`, `STT_OBJECT` in data) |
| `.extern` | `SHN_UNDEF` |
| `.quad` / `.long` / `.word` / `.byte` (aliases `dq`/`dd`/`dw`/`db`) | Integers or symbol[+addend]; symbol → `R_X86_64_64` in `.data`/`.rodata`. In `.text`, `.byte`/`.long`/`.quad` stay in insn order |
| `.zero` / `.space N` | Reserve N bytes (`.bss` grows; others write zeros) |
| `.align` / `.p2align` | Honored in data/rodata/bss; ignored in `.text` |
| `.type` / `.size` | Ignored |
| `.macro name args…` / `.endm` | Expanded; `\arg` and `\()` |
| `.rept N` / `.endr` | Repeat body N times (fixed count only — **no** `%assign`-style counters) |
| `.incbin "path"` | Embed file bytes in `.rodata` (S5) |
| `#` `//` `;` comments | Stripped |

## Instructions (non-exhaustive)

`mov`/`movb`/`movw`/`movl`/`movq`, `lea`, `push`/`pop`, `add`/`sub`/`xor`/
`and`/`or`/`cmp`/`test`/`xchg`, `shl`/`shr`/`sal`/`sar` `$imm,%reg`,
`div`/`idiv`, `jmp`/`call`/`jcc` (incl. indirect `call *%reg` /
`jmp *%reg`), `ret`/`retq`/`retfq`/`lretq`,
`in`/`inb`/`inw`/`inl`, `out`/`outb`/`outw`/`outl`,
`cli`/`sti`/`hlt`/`pause`/`nop`/`ud2`/`syscall`/`sysretq`/`iretq`,
`pushfq`/`popfq`/`pushf`/`popf`,
`lgdt`/`lidt` (mem or `sym(%rip)`) / `ltr` / `rdmsr`/`wrmsr`/`invlpg`,
`mov` `%crN` ↔ GPR and `%reg` ↔ `%ds`/`%es`/`%ss`/`%fs`/`%gs`,
`movdqa` `%xmmN` ↔ `disp(%reg)` (and `%xmm`↔`%xmm`) for sysentry/desc XMM
save/restore,
RIP-relative `sym(%rip)` for `lea`/`mov`/`lgdt`/`lidt` (local labels resolved
in-file; externals → `R_X86_64_PC32`).

Local labels and forward `jmp`/`call` work within one file; externals get
`R_X86_64_PC32` / `PLT32`.

## Priority migrate (fakeos)

1. **io / usys / crt0** — see `examples/io.S`, `examples/usys.S`, `examples/crt0.S`
2. **switch.asm** — **done (example-ready)**: `examples/switch.S`
3. **sysentry / desc** — **example-ready** for a Makefile nasm drop of those
   objects (boot still out of scope):
   - `examples/sysentry.S` — full `syscall_entry` / `syscall_epilogue`
     (GPR+XMM), `user_iret_trampoline`, `fork_ret_trampoline`,
     `.data` `syscall_user_rsp` + RIP stores/loads, `sysretq`
   - `examples/desc_stubs.S` — ISR_NOERR / ISR_ERR macros for vectors
     0..47 + 255, `isr_common` (GPR+XMM + `isr_dispatch` + `iretq`),
     `isr_stub_table` `.quad`s, `gdt_flush`/`idt_load`/`tss_load`,
     MSR/CR/`invlpg`/`exc_*` helpers
   - Remaining gaps vs a one-shot Makefile swap: no `%assign` counters inside
     `.rept` (vectors are listed explicitly); real fakeos link still needs C
     `syscall_dispatch` / `syscall_kstack_top` / `isr_dispatch` / `gdt` / `idt`;
     `.rela.*` for rodata abs symbols may appear under the data reloc stream
     (same ET_REL contract). Prefer copying the examples into
     `arch/x86_64/*.S` and dropping the matching nasm objects.
4. boot.asm (32-bit — out of scope for this subset)

## C + M1/M2 alternative (no `.S`)

When a stub is a thin SysV wrapper, prefer **naked + GNU inline asm**
(already on this branch):

```c
package main;
__attribute__((naked)) void cpu_halt(void) {
    __asm__ volatile("hlt; ret");
}
__attribute__((naked)) unsigned char inb(unsigned short port) {
    __asm__ volatile(
        "mov %di, %dx\n"
        "xor %eax, %eax\n"
        "in %dx, %al\n"
        "ret");
}
```

Use `.S` when you need file-level `.macro` / `.rept` (IRQ stubs) or
RIP-relative data (`syscall_user_rsp(%rip)`).

## CLI

```text
fakecc -c arch/x86_64/io.S -o io.o
fakecc -c arch/x86_64/switch.S -o switch.o
fakecc -c docs/fakecc-toolchain/examples/sysentry.S -o sysentry.o
fakecc -c docs/fakecc-toolchain/examples/desc_stubs.S -o desc_stubs.o
fakecc --kernel-multiboot -nostdlib boot.o io.o … -o fakeos.elf
```

See also [embed-incbin.md](embed-incbin.md) (S5) and [env-and-flags.md](env-and-flags.md) (N1/N2).

## Related: `--target=` (Should S4)

Unrecognized `--target=` triples print a warning and fall back to the host
default (instead of exiting), so Makefile / `/etc/fakecc.flags` probing stays
safe on single-backend builds. Explicit `--target=x86_64-linux` always selects
the Linux x86-64 ELF backend.
