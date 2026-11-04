; Embed the flat AP trampoline binary into the kernel image.
section .rodata
global ap_tramp_blob
global ap_tramp_blob_end
ap_tramp_blob:
    incbin "ap_tramp.bin"
ap_tramp_blob_end:

section .note.GNU-stack noalloc noexec nowrite progbits
