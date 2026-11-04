# File embed / `.incbin` (fakeos Should S5)

## Gas `.incbin` (preferred for blobs)

```gas
    .section .rodata
    .globl user_init_start
    .globl user_init_end
    .align 8
user_init_start:
    .incbin "build/user/init.elf"
user_init_end:
```

`fakecc -c blob.S -o blob.o` places the file bytes in `.rodata` and exports
`*_start` / `*_end` as `STT_OBJECT` (size = distance to the next label or
section end). Paths are relative to the `.S` file’s directory, then cwd.

See also `examples/blob_incbin.S`.

## C-side table + count (m5_blob style)

```c
package kernel;
extern char blob_x_start;
extern char blob_x_end;
struct m5_blob_entry {
    const char *path;
    const char *start;
    const char *end;
};
struct m5_blob_entry m5_blob_table[] = {
    { "/bin/x", &blob_x_start, &blob_x_end },
};
unsigned long m5_blob_count =
    sizeof(m5_blob_table) / sizeof(m5_blob_table[0]);
```

Generate the `.S` with one `.incbin` per staged file (as `m5_stage.sh` does
today with nasm), or emit the table from C once symbols exist.
