/* smashrom.ld - vlink linker script for smashrom */

OUTPUT_FORMAT("binary")
OUTPUT_ARCH(m68k)
ENTRY(_reset)

MEMORY
{
    rom (rx)      : ORIGIN = 0x00f80000, LENGTH = 0x40000
    ram (rw)      : ORIGIN = 0x00030000, LENGTH = 0x80000
    chipram (rw)  : ORIGIN = 0x00010000, LENGTH = 0x10000
}

SECTIONS
{
    PROVIDE(___machtype = 0x0);
    .text :
    {
        *(.reset)
        _reset_cont = .;
        . = ALIGN(0x00000010);  /* 0x00f80010 */
        *(.reset_hi)
        *(.text)
        *(.vectors)
        _etext = .;
    } > rom

    .rodata ADDR(.text) + SIZEOF(.text) :
    {
        *(.rodata)
        _copy_to_ram_start = .;
        *(.text_to_ram)
        _copy_to_ram_end = .;
        . = ALIGN(2);
    } > rom

    .data :
    {
        . = ALIGN(2);
        __sdata_rom = LOADADDR(.data);
        __sdata = .;
        *(.data)
        . = ALIGN(2);
        _edata = .;
    } > ram AT> rom

/*
    .rodata ADDR(.text) + SIZEOF(.text) + SIZEOF(.data) :
    {
        LONG(0x12345678);
        . = ALIGN(0x40000);
    } > rom
*/

    .bsschip :
    {
        *(.bsschip)
        . = ALIGN(2);
    } > chipram AT >rom

    .bss (NOLOAD) :
    {
        _bss = .;
        *(.bss)
        . = ALIGN(2);
        _ebss = .;
    } > ram AT >rom

    ___data_size = SIZEOF(.data);
    ___bss_size = SIZEOF(.bss);

    /* Attempt to discard unwanted sections (doesn't seem to work) */
    /* /DISCARD/ : { *(*) } */
}
