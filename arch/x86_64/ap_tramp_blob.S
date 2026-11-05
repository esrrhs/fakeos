;/* Embed AP trampoline flat binary (Gas AT&T for fakecc) */
    .section .rodata
    .globl ap_tramp_blob, ap_tramp_blob_end
ap_tramp_blob:
    .incbin "ap_tramp.bin"
ap_tramp_blob_end:
