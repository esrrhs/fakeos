OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x00100000;

SECTIONS
{
    . = KERNEL_LMA;

    /* 32-bit bootstrap section: runs in lower physical memory before paging is enabled */
    .boot : {
        KEEP(*(.multiboot))
        *(.boot.text)
        *(.boot.data)
        *(.boot.bss)
    }

    . += KERNEL_VMA;

    /* 64-bit kernel sections: linked at Higher-Half virtual address */
    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        _kernel_text_start = .;
        *(.text .text.*)
        _kernel_text_end = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        _kernel_rodata_start = .;
        *(.rodata .rodata.*)
        _kernel_rodata_end = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        _kernel_data_start = .;
        *(.data .data.*)
        _kernel_data_end = .;
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        _kernel_bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _kernel_bss_end = .;
    }

    _kernel_end = .;
}
