/* AnkaOS kullanıcı programı bağlayıcı betiği: düz imaj, USER_BASE'e yüklenir */
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    __image_start = .;
    .text : { *(.text.start) *(.text .text.*) }
    . = ALIGN(16);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(16);
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __image_end = .;
    __image_memsz = __image_end - __image_start;
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
