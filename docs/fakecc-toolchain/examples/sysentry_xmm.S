/* Minimal movdqa XMM save/restore pattern from fakeos sysentry.asm / desc.asm.
 * Full sysentry.S still needs .data dq + more; this proves the encoding. */
    .text
    .globl xmm_save16, xmm_restore16

xmm_save16:
    movdqa %xmm0,  0x08(%rsp)
    movdqa %xmm1,  0x18(%rsp)
    movdqa %xmm2,  0x28(%rsp)
    movdqa %xmm3,  0x38(%rsp)
    movdqa %xmm4,  0x48(%rsp)
    movdqa %xmm5,  0x58(%rsp)
    movdqa %xmm6,  0x68(%rsp)
    movdqa %xmm7,  0x78(%rsp)
    movdqa %xmm8,  0x88(%rsp)
    movdqa %xmm9,  0x98(%rsp)
    movdqa %xmm10, 0xa8(%rsp)
    movdqa %xmm11, 0xb8(%rsp)
    movdqa %xmm12, 0xc8(%rsp)
    movdqa %xmm13, 0xd8(%rsp)
    movdqa %xmm14, 0xe8(%rsp)
    movdqa %xmm15, 0xf8(%rsp)
    ret

xmm_restore16:
    movdqa 0x08(%rsp), %xmm0
    movdqa 0x18(%rsp), %xmm1
    movdqa 0x28(%rsp), %xmm2
    movdqa 0x38(%rsp), %xmm3
    movdqa 0x48(%rsp), %xmm4
    movdqa 0x58(%rsp), %xmm5
    movdqa 0x68(%rsp), %xmm6
    movdqa 0x78(%rsp), %xmm7
    movdqa 0x88(%rsp), %xmm8
    movdqa 0x98(%rsp), %xmm9
    movdqa 0xa8(%rsp), %xmm10
    movdqa 0xb8(%rsp), %xmm11
    movdqa 0xc8(%rsp), %xmm12
    movdqa 0xd8(%rsp), %xmm13
    movdqa 0xe8(%rsp), %xmm14
    movdqa 0xf8(%rsp), %xmm15
    ret
