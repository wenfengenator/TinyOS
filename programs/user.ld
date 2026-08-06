/* SPDX-License-Identifier: GPL-2.0-only */

/* Linker script for standalone tinyos user programs.
 * Programs are position-dependent, linked at USER_BASE; exec() maps a fresh
 * set of pages there in the new task's address space. */
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;          /* USER_BASE */

    .text : {
        *(.text._start)
        *(.text)
        *(.text.*)
    }
    .rodata : { *(.rodata) *(.rodata.*) }
    .data   : { *(.data) *(.data.*) }
    .bss    : { *(.bss) *(.bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr) }
}
