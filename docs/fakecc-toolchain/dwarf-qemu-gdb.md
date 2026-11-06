# DWARF (`-g`) for QEMU+gdb (fakeos Nice N3)

fakecc emits DWARF4 debug info with `-g`. Addresses in `.debug_info` /
`.debug_frame` / line programs are **linked virtual addresses**
(`text_base_vaddr` / image base), so higher-half kernels
(`KERNEL_VMA = 0xFFFFFFFF80000000`) and user images (`--image-base=0x400000`)
map correctly in gdb when you load the **ELF64** that still carries debug
sections.

## What works

- Breakpoints by symbol / file:line on normal C functions (prologue present).
- Backtrace through framed functions (FDE advances CFA only when
  `after_push_rbp_pc` was recorded).
- Naked / frameless functions (`__attribute__((naked))`, hand-written
  `syscall_entry` / `isr_*` stubs): FDE keeps the CIE default
  (`CFA = rsp+8` on x86-64; ARM64 CIE default) instead of inventing a fake
  `push rbp` / `mov rbp` sequence. `DW_AT_frame_base` is `rsp`/`sp` for those.
- Linked objects retain prologue landmarks when merging `.o` debug
  (previously dropped at link).

## Limits (documented, non-blocking)

1. **`--kernel-multiboot` / ELF32 Multiboot** output strips or omits DWARF.
   Debug against `fakeos64.elf` (or whatever ELF64 you keep before
   `objcopy` / multiboot rewrite), not the QEMU Multiboot ELF32 alone.
2. **Line info inside pure asm / naked bodies** is coarse: one DIE spans the
   whole function; there are no per-instruction `.loc` rows unless the C
   frontend emitted statements.
3. **Locals / parameters in naked functions** usually have no reliable
   locations (no frame, compiler did not allocate slots).
4. **ISR / SYSCALL** frames that switch stacks mid-function are beyond
  what a static FDE can describe; expect `bt` to stop or look odd past
   the stack switch.
5. Dual-load LMA≠VMA boot stubs: prefer symbol addresses from the
   higher-half VMA view gdb is told about (`add-symbol-file` / `file` on
   the VMA-linked ELF).

## Suggested gdb session (kernel)

```text
qemu-system-x86_64 -s -S -kernel build/fakeos.elf …
gdb build/fakeos64.elf
(gdb) target remote :1234
(gdb) break kmain
(gdb) break syscall_entry   # if linked with -g from a naked C stub
(gdb) continue
```

See also `elf-et-rel-higher-half.md` for VMA / relocatable contract.
