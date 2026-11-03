# Rebuilt-kernel order-3 selftest failure (diagnosed)

## Symptom

`ldfake`-linked `/tmp/fakeos-new` (phase 5b) printed:

```
[FAIL] order-3 block data mismatch at qword 0
```

and often `[U][FAIL] kill/wait status=0 reaped=0`. Stock `build/fakeos.elf`
(GNU ld) was fine. The failure was attributed to higher-half reloc in
`user/ldfake.c`.

## Root cause (not ldfake)

Host `ldfake` and GNU ld, given the **same** object set, either **both pass**
or **both fail**. Byte-identical reproduction:

1. Link `build/m5stage/ref/**.o` + `/asm` with host `ldfake` → matches
   `build/fakeos-rebuilt.elf` and fails order-3.
2. Link the same inputs with GNU `ld -T boot/linker.ld` → **also fails**.
3. Link current `build/*.o` (installed fakecc) with host `ldfake` → **passes**
   order-3 and kill/wait.

So the bug was in the **reference objects** (and the matching in-OS
`/bin/fakecc`), not in high-half reloc application.

### Codegen detail

`kernel/memtest.c` `test_block_split` used `p[k] = 0x5151C0DEC0DEULL ^ k`.
On fakecc tip after `2c1f4525` (O0 BXOR / remat work on
`combine/gcc16-macho-and-o0-ldouble`), the write loop emitted approximately:

```
mov $0x1000, %esi          ; loop bound 8*512
movslq %esi, %rsi
...
movabs $0x5151c0dec0de, %rdi
xor %rsi, %r9              ; still the BOUND, not k
; only then reload k into %rsi
```

Every store wrote `const ^ 0x1000`; the verify side correctly computed
`const ^ k` → mismatch at qword 0. Installed fakecc (and fakecc
`2c1f4525`) reload `k` before the XOR and are fine.

`kill/wait` failures on the same image are the same toolchain skew: tip
fakecc objects miscompile other kernel paths; they clear when refs are
built with `2c1f4525`.

## Fixes landed

1. **Harden** `test_block_split` to use `p[k] = block + k` (same shape as
   `test_single_pages`), avoiding `imm64 ^ induction`.
2. **Pin M5** to a known-good fakecc when restaging:
   `FAKECC_SRC=<tree@2c1f4525> make` (see Makefile comment). Tip of the
   combine branch remains unsafe for rebuilt-kernel selftests until the
   BXOR/home clobber is fixed upstream.

## ldfake notes (non-blocking)

ldfake still inserts a 4 KiB-aligned synthetic GOT between `.rodata` and
`.data`, so BSS/stack absolute addresses differ from GNU ld by ~one page;
that layout delta alone does **not** cause the order-3 failure.
