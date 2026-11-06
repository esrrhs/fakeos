/* gas port of fakeos arch/x86_64/io.asm — fakecc -c io.S */
    .text
    .globl outb, inb, outw, inw, outl, inl, io_wait
    .globl cpu_halt, cpu_cli, cpu_sti

outb:
    mov %di, %dx
    mov %sil, %al
    out %al, %dx
    ret

inb:
    mov %di, %dx
    xor %eax, %eax
    in %dx, %al
    ret

outw:
    mov %di, %dx
    mov %si, %ax
    out %ax, %dx
    ret

inw:
    mov %di, %dx
    xor %eax, %eax
    in %dx, %ax
    ret

outl:
    mov %di, %dx
    mov %esi, %eax
    out %eax, %dx
    ret

inl:
    mov %di, %dx
    in %dx, %eax
    ret

io_wait:
    out %al, $0x80
    ret

cpu_halt:
    hlt
    ret

cpu_cli:
    cli
    ret

cpu_sti:
    sti
    ret
