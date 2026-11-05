;/* fakeos - Low-level x86-64 CPU and Port I/O (Gas AT&T for fakecc) */
    .text
    .globl outb, inb, outw, inw, outl, inl
    .globl io_wait, cpu_halt, cpu_cli, cpu_sti, cpu_pause, cpu_enable_sse

outb:
    mov %di, %dx
    mov %esi, %eax
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

cpu_pause:
    pause
    ret

/* Match BSP boot.asm: CR4.OSFXSR|OSXMMEXCPT, clear CR0.EM, set CR0.MP. */
cpu_enable_sse:
    mov %cr4, %rax
    or $0x600, %rax
    mov %rax, %cr4
    mov %cr0, %rax
    and $0xfffffffb, %rax
    or $0x2, %rax
    mov %rax, %cr0
    ret
