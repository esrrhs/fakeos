/* Minimal S5 example — replace the path with a real blob when linking OS. */
    .section .rodata
    .globl demo_blob_start
    .globl demo_blob_end
    .align 8
demo_blob_start:
    .incbin "README.md"
demo_blob_end:
