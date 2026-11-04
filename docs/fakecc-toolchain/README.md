# fakecc toolchain notes (for fakeos)

Capability lives in [fakecc](https://github.com/esrrhs/fakecc); these notes document how fakeos uses that toolchain (code models, asm subset, linker scripts, examples).

| Doc | Topic |
|-----|-------|
| [elf-et-rel-higher-half.md](elf-et-rel-higher-half.md) | ET_REL / `-mcmodel=kernel` / higher-half |
| [ld-script-subset.md](ld-script-subset.md) | Linker script subset |
| [asm-s-subset.md](asm-s-subset.md) | Assembler `.S` subset |
| [embed-incbin.md](embed-incbin.md) | `.incbin` embedding |
| [env-and-flags.md](env-and-flags.md) | Env / flags |
| [dwarf-qemu-gdb.md](dwarf-qemu-gdb.md) | DWARF + QEMU/gdb |
| [examples/](examples/) | Sample `.S` stubs |

Related: `docs/fakecc-support-requests.md` (when present).
