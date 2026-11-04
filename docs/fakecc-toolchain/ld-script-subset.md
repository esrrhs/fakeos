# Subset GNU ld scripts (`-T` / `--script=`) — fakeos S1

fakecc accepts a **small** GNU ld linker-script subset so fakeos can pass the
same paths used with host `ld`:

```text
fakecc -T boot/linker.ld --kernel-multiboot …   # redundant flag OK
fakecc -T boot/linker.ld -nostdlib boot.o … -o fakeos.elf
fakecc -T user/user.ld … -o hello.elf
fakecc --script=user/user.ld …
```

`-T path`, `-Tpath`, `--script=path`, and `--script path` are accepted.

## What is supported

| Construct | Notes |
|-----------|--------|
| `/* … */` comments | `//` line comments also skipped |
| `OUTPUT_FORMAT(...)` | Recorded; ignored for layout |
| `ENTRY(sym)` | Must be `_start` for kernel shape |
| `NAME = expr;` | Absolute assigns (`KERNEL_VMA`, `KERNEL_LMA`, `USER_VMA`, …) |
| `PHDRS { name PT_LOAD FLAGS(n); }` | User script; text=`5` (RX), data=`6` (RW) |
| `SECTIONS { … }` | Location counter `. =` / `. +=`; output sections with `ALIGN(n)`, `AT(expr)`, `KEEP(*(pat))`, `*(pat)`, `*(COMMON)`, `sym = .;`, optional `:phdr` |
| Expressions | Numbers (hex/dec, optional `K`/`M`), identifiers, `+`/`-`, `ADDR(sec)`, `ALIGN(n)`, `FLAGS(n)` |

## How fakecc uses the script (MVP)

fakecc does **not** yet run a general section-placement engine from the parse
tree. It **classifies** the script and configures existing backends:

| Shape | Detection | Backend |
|-------|-----------|---------|
| Kernel higher-half | `KERNEL_VMA` / `. += KERNEL_VMA` / `AT(ADDR(.) - KERNEL_VMA)` / `.boot` / `KEEP(*(.multiboot))` | Same as `--kernel-multiboot` → Multiboot ELF32 dual `PT_LOAD` |
| User | `USER_VMA` and/or `PHDRS` | `link_set_image_base(USER_VMA)` + existing ELF64 dual RX/RW `PT_LOAD` |

Constants must match the fakeos layouts today:

- Kernel: `KERNEL_VMA = 0xFFFFFFFF80000000`, `KERNEL_LMA = 0x100000`, `ENTRY(_start)`
- User: any non-zero `USER_VMA` (fakeos uses `0x400000`); PHDR FLAGS 5/6 when present

## Still deferred

- Arbitrary ld scripts (MEMORY, INSERT, OVERLAY, `/DISCARD/`, wildcards beyond
  the patterns above, full expression language, orphan sections, …)
- Driving a fully generalized layout engine from the parse tree (no hard-coded
  dual-segment / image-base backends)
- Honoring `-n` / `--nmagic` / `--gc-sections` from the script side

Use GNU ld or fakeos `ldfake` when you need script features outside this subset.

## Related

- `elf-et-rel-higher-half.md` (M4 / S1–S2)
- fakeos `boot/linker.ld`, `user/user.ld`
- `../fakecc-support-requests.md` §2.2 **S1**
