/* SPDX-License-Identifier: GPL-2.0-only */

# tinyos boot sector — 512 bytes, real mode -> 32-bit protected mode
# Assembled with GNU as (AT&T syntax).  BIOS loads this at 0x7c00.
#
# Responsibilities of this layer (the "bootloader"):
#   * load the kernel image from disk to 0x10000
#   * enable the A20 line
#   * install a temporary flat GDT and switch the CPU to 32-bit protected mode
#   * jump to the kernel entry point at 0x10000

    .code16
    .section .text
    .global _start

    .set KERNEL_SEG,  0x1000       # 0x1000:0000 = physical 0x10000
    .set KERNEL_SECT, 128          # sectors of kernel to load (64 KB; kernel+ramdisk)

_start:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $0x7c00, %sp
    movb    %dl, boot_drive        # BIOS leaves boot drive in DL
    sti

    movw    $msg_boot, %si
    call    bios_print

    # ---- load kernel: KERNEL_SECT sectors from CHS(0,0,2) -> 0x10000 ----
    movw    $KERNEL_SEG, %ax
    movw    %ax, %es
    xorw    %bx, %bx               # ES:BX = 0x1000:0000
    movb    $0x02, %ah             # int 0x13 / read sectors
    movb    $KERNEL_SECT, %al
    movb    $0x00, %ch             # cylinder 0
    movb    $0x02, %cl             # sector 2 (1-based; sector 1 is this boot sector)
    movb    $0x00, %dh             # head 0
    movb    boot_drive, %dl
    int     $0x13
    jc      disk_error

    # ---- enable A20 (fast gate, port 0x92) ----
    inb     $0x92, %al
    orb     $0x02, %al
    outb    %al, $0x92

    # ---- enter protected mode ----
    cli
    lgdt    gdtr
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    ljmp    $0x08, $pm_entry       # far jump reloads CS with 32-bit selector

disk_error:
    movw    $msg_disk, %si
    call    bios_print
1:  hlt
    jmp     1b

# bios_print: SI = NUL-terminated string (real mode, BIOS teletype)
bios_print:
    pushw   %ax
1:  lodsb
    testb   %al, %al
    jz      2f
    movb    $0x0e, %ah
    int     $0x10
    jmp     1b
2:  popw    %ax
    ret

    .code32
pm_entry:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    movl    $0x90000, %esp
    ljmp    $0x08, $0x10000        # hand off to the kernel

# ---- data ----
boot_drive:
    .byte   0
msg_boot:
    .asciz  "tinyos boot: loading kernel; entering protected mode...\r\n"
msg_disk:
    .asciz  "tinyos boot: DISK ERROR\r\n"

    .align  8
gdt:
    .quad   0x0000000000000000     # null
    .quad   0x00CF9A000000FFFF     # 0x08 flat 32-bit code
    .quad   0x00CF92000000FFFF     # 0x10 flat 32-bit data
gdt_end:
gdtr:
    .word   gdt_end - gdt - 1
    .long   gdt

    # pad to 510 bytes and add the boot signature
    .org    510
    .byte   0x55, 0xaa
