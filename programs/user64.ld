/* Linker script for tinyos x64 user programs.
 *
 * Programs are compiled -fPIC -fvisibility=hidden (RIP-relative, no GOT) and
 * linked flat at USER_CODE_VA, then objcopy'd to a raw image the kernel maps at
 * that same virtual address (see kernel/arch64.c). */
ENTRY(_start)

SECTIONS
{
    . = 0x0000700000000000;      /* USER_CODE_VA */

    .text : {
        *(.text._start)
        *(.text)
        *(.text.*)
    }
    .rodata : { *(.rodata) *(.rodata.*) }
    .data   : { *(.data) *(.data.*) }
    .bss    : { *(.bss) *(.bss.*) *(COMMON) }

    /DISCARD/ : {
        *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr)
        *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.interp)
    }
}
