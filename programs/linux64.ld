/* SPDX-License-Identifier: GPL-2.0-only */

ENTRY(_start)

PHDRS
{
    headers PT_PHDR PHDRS FLAGS(4);
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
}

SECTIONS
{
    . = 0x0000000000400000 + SIZEOF_HEADERS;
    .text : {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
    } :text

    /DISCARD/ : {
        *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr)
    }
}