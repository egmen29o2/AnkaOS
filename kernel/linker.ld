/* AnkaOS çekirdek bağlayıcı betiği: düz imaj, 0x10000'e yüklenir */
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)

SECTIONS
{
    . = 0x10000;
    __kernel_start = .;

    .text : { *(.text.entry) *(.text .text.*) }
    . = ALIGN(16);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(16);
    .data : { *(.data .data.*) }
    . = ALIGN(16);

    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
