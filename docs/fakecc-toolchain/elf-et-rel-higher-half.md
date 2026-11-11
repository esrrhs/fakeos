# ELF ET_REL, relocations, and higher-half kernels (fakeos Must M4)

This documents fakecc’s **x86_64-linux relocatable object** contract so a
pure-C kernel (plus future asm stubs) can be linked with GNU ld /
`boot/linker.ld` or fakeos `user/ldfake.c`.

## Object file shape

`fakecc -c` writes **ELF64 `ET_REL`** with at least:

| Section     | Role                                      |
|-------------|-------------------------------------------|
| `.text`     | executable code (`SHF_ALLOC\|SHF_EXECINSTR`) |
| `.rodata`   | read-only data                            |
| `.data`     | initialized mutable data                  |
| `.bss`      | zero-filled (`SHT_NOBITS`)                |
| `.rela.text` / `.rela.data` | RELA fixups                 |

TLS (`.tdata` / `.tbss`), `.init_array` / `.fini_array`, and
`.note.GNU-stack` may also appear. Names match the usual GNU ld input
patterns `*(.text .text.*)`, `*(.rodata .rodata.*)`, etc.

### Boot / multiboot sections

fakeos `boot/linker.ld` collects:

```text
KEEP(*(.multiboot))
*(.boot.text) *(.boot.data) *(.boot.bss)
```

Those names are still produced by the dedicated bootstrap asm object
(`arch/x86_64/boot.asm`). C translation units emit the standard four
sections above; that is enough for the higher-half `.text`/`.rodata`/
`.data`/`.bss` rules. (A future `__attribute__((section(...)))` path may
place C into `.boot.*` / `.multiboot` without changing this contract.)

## Relocation types (aligned with `user/ldfake.c`)

fakecc codegen / object emit produce or preserve:

| Type              | Value | Typical use                                      |
|-------------------|------:|--------------------------------------------------|
| `R_X86_64_64`     |     1 | absolute 64-bit pointer in `.data` / arrays      |
| `R_X86_64_PC32`   |     2 | RIP-relative data / near call to local           |
| `R_X86_64_PLT32`  |     4 | `call` / `jmp` to external or PLT-bound symbol |
| `R_X86_64_GOTPCREL` |   9 | RIP-relative load of external data via GOT       |
| `R_X86_64_32`     |    10 | unsigned 32-bit absolute (asm stubs / rare C)    |
| `R_X86_64_32S`    |    11 | signed 32-bit absolute (local abs addressing)    |

The built-in linker applies the same set (see `src/link.c`). Objects from
nasm that only use `32` / `32S` remain linkable.

## Code model and `KERNEL_VMA`

fakeos links the kernel at:

```text
KERNEL_VMA = 0xFFFFFFFF80000000   /* top −2 GiB */
```

with LMA `0x100000` for the low `.boot` segment (`boot/linker.ld`).

### Default / `-mcmodel=small`

Historical fakecc default: **small** code model — RIP-relative for most
accesses; local absolute memory operands may use **`R_X86_64_32S`**.

### `-mcmodel=kernel`

Accepted spelling for the SysV **kernel** model (addresses in the negative
2 GiB of the canonical 64-bit space). On x86_64-linux this matches how
GNU ld + `boot/linker.ld` place `.text` at `KERNEL_VMA+…`:

- PC-relative (`PC32` / `PLT32` / `GOTPCREL`) needs no absolute VMA in the `.o`.
- `R_X86_64_32S` remains valid once the linker assigns symbol values in the
  negative 2 GiB (sign-extended 32-bit displacements).
- `R_X86_64_64` carries full absolute addresses where needed.

fakecc can perform the dual PT_LOAD / higher-half layout via
`--kernel-multiboot` or `-T boot/linker.ld` (subset parser; see
[`ld-script-subset.md`](ld-script-subset.md)). GNU ld / fakeos `ldfake`
remain valid alternatives. The `.o` files from `fakecc -c` are the stable
input.

## Verification

- Unit: `test_emit` cases `et_rel_standard_sections`, `et_rel_reloc_set_m4`
- Manual: `readelf -S` / `readelf -r` on `fakecc -c` output
- fakeos: link kernel objects with `boot/linker.ld` (existing host path)

## Related

- `../fakecc-support-requests.md` §2.1 **M4**, §2.2 **S1/S2**
- fakeos `boot/linker.ld`, `user/ldfake.c`

## Should S1 / S2: dual PT_LOAD + Multiboot ELF32

| Scene | Script / CLI | Base / PHDRs |
|-------|--------------|--------------|
| User | `-T user/user.ld` / `--image-base=` / default | `USER_VMA = 0x400000`; text=`PT_LOAD RX`, data=`PT_LOAD RW` |
| Kernel | `-T boot/linker.ld` / `--kernel-multiboot` | Low `.boot`/`.multiboot` at LMA `0x100000` + high `.text`/… at `KERNEL_VMA + LMA`; exports `_kernel_*` at link time; `ENTRY(_start)` |

**User path:** static linker image base `0x400000` (= `USER_VMA`); override with
`--image-base=ADDR`, `link_set_image_base()`, or `-T` / `--script=` on a user-shaped
script (`USER_VMA` / `PHDRS`).

**Kernel path (S1+S2):** `link_kernel_multiboot()` (host port of fakeos
`user/ldfake.c`) reads ELF64 `ET_REL` inputs, places sections per
`boot/linker.ld`, resolves `64`/`PC32`/`PLT32`/`GOTPCREL`/`32`/`32S`, and
writes a **phdr-only Multiboot ELF32** image (ELFCLASS32, `EM_386`, two
`PT_LOAD`s, Multiboot magic at file offset `0x1000`). No host `objcopy
-O elf32-i386` required. Passing `-T boot/linker.ld` parses the script subset
and enables this backend (same constants).

```text
fakecc -T boot/linker.ld -nostdlib boot.o kmain.o … -o fakeos.elf
fakecc --kernel-multiboot -nostdlib boot.o kmain.o … -o fakeos.elf
fakecc -T user/user.ld hello.c -o hello.elf
```

Script grammar limits: [`ld-script-subset.md`](ld-script-subset.md).

Verification: `test_link_kernel`, `test_ldscript`; fakeos `scripts/test_qemu.py`
structural checks (ELF32 + MB1 magic + dual PHDRs).

## Debugging (`-g`)

DWARF PC ranges use the linked VMA (`KERNEL_VMA` / `--image-base`). For
naked stubs and QEMU+gdb limits, see [`dwarf-qemu-gdb.md`](dwarf-qemu-gdb.md)
(fakeos Nice N3).

