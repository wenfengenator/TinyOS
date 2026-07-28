/* tinyos kernel linker script — flat binary loaded at 0x10000.
 * entry.s (.text.boot) is placed first so _start is exactly at 0x10000.
 * The C++ user layer is linked last (see Makefile object order). */
ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text.boot)
        *(.text)
        *(.text.*)
    }
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    .data : {
        *(.data)
        *(.data.*)
    }
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr)
    }
}
