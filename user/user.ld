OUTPUT_FORMAT(elf64-x86-64)
ENTRY(umain)

USER_VMA = 0x400000;

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = USER_VMA;

    .text ALIGN(4K) : {
        *(.text .text.*)
    } :text

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    } :text

    .data ALIGN(4K) : {
        *(.data .data.*)
    } :data

    .bss ALIGN(4K) : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
}
