/* SPDX-License-Identifier: GPL-2.0-only */

/* ACPI S3 waking-vector trampoline, copied to physical 0x7000.
 * Firmware enters here in 16-bit real mode.  Parameters at 0x7F00 are filled
 * immediately before S3 entry:
 *   +0x00  saved CR3 (must be below 4 GiB)
 *   +0x08  resume stack top
 *   +0x10  64-bit resume continuation
 */

    .code16
    .global _s3_start
_s3_start:
    cli
    /* Mode-independent "firmware entered the trampoline" marker (phase 1): the
     * mov-AL-imm8 / out-imm8-AL opcodes below encode identically in real,
     * 32-bit protected, and long mode, so this records entry regardless of the
     * CPU mode the firmware hands us.  If a later mode-specific path crashes, the
     * retained phase stays 1, proving the firmware DID re-enter our vector. */
    movb    $0x81, %al             # CMOS reg 0x01, NMI masked
    outb    %al, $0x70
    movb    $0xC1, %al             # phase 1 = entered (any mode)
    outb    %al, $0x71
    /* ACPI requires the legacy waking vector in real mode; OVMF instead keeps
     * PE/paging active.  This mode-neutral prefix distinguishes those paths. */
    smsw    %ax
    testb   $1, %al
    jnz     s3_long
    ljmp    $0x0000, $s3_real
s3_real:
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movb    $2, 0x7F18             # firmware entered legacy wake vector
    movb    $0x81, %al             # CMOS alarm-seconds register, NMI masked
    outb    %al, $0x70
    movb    $0xC2, %al             # ACPI RTC "don't care" value + phase 2
    outb    %al, $0x71
    # Re-enable the A20 gate: firmware S3 resume can hand off with A20 masked,
    # which corrupts the page-table walk (CR3 and tables live above 1 MiB) the
    # instant CR0.PG is set below -> triple fault between phase 2 and phase 3.
    # Fast-A20 via port 0x92, preserving bit0 (fast reset = 0).
    inb     $0x92, %al
    orb     $0x02, %al
    andb    $0xFE, %al
    outb    %al, $0x92
    lgdtl   s3_gdtr
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    ljmpl   $0x08, $s3_prot

    .code64
s3_long:
    movabs  $0x7F18, %rax
    movb    $3, (%rax)             # firmware entered preserved-long-mode path
    movb    $0x81, %al
    outb    %al, $0x70
    movb    $0xC3, %al             # persistent phase 3 if continuation fails
    outb    %al, $0x71
    movabs  $0x7F00, %rax
    movq    (%rax), %rax
    movq    %rax, %cr3
    movabs  $s3_gdtr, %rax
    lgdt    (%rax)
    movw    $0x20, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movabs  $0x7F08, %rax
    movq    (%rax), %rsp
    andq    $-16, %rsp
    xorl    %ebp, %ebp
    movabs  $0x7F10, %rax
    movq    (%rax), %rax
    jmpq    *%rax

    .code32
s3_prot:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movb    $0x81, %al             # phase 5: 32-bit protected mode reached
    outb    %al, $0x70
    movb    $0xC5, %al
    outb    %al, $0x71
    movl    %cr4, %eax
    orl     $0x20, %eax
    movl    %eax, %cr4
    movl    0x7F00, %eax
    movl    %eax, %cr3
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $0x100, %eax
    wrmsr
    movb    $0x81, %al             # phase 6: PAE+CR3+LME set, about to enable paging
    outb    %al, $0x70
    movb    $0xC6, %al
    outb    %al, $0x71
    movl    %cr0, %eax
    orl     $0x80000000, %eax
    movl    %eax, %cr0
    movb    $0x81, %al             # phase 7: paging on (IA-32e compat), about to far-jump to 64-bit
    outb    %al, $0x70
    movb    $0xC7, %al
    outb    %al, $0x71
    ljmpl   $0x18, $s3_long

    .balign 16
s3_gdt:
    .quad   0x0000000000000000
    .quad   0x00CF9A000000FFFF
    .quad   0x00CF92000000FFFF
    .quad   0x00AF9A000000FFFF
    .quad   0x00CF92000000FFFF
s3_gdtr:
    .word   s3_gdtr - s3_gdt - 1
    .long   s3_gdt

    .global _s3_end
_s3_end:

    .section .note.GNU-stack,"",@progbits
