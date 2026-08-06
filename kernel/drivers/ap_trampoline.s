/* SPDX-License-Identifier: GPL-2.0-only */

# tinyos x64 AP startup trampoline (R8 SMP).
#
# Application processors come out of INIT-SIPI-SIPI in 16-bit real mode at
# physical (vector << 12) = 0x8000.  This blob is linked at 0x8000 and copied
# there by the BSP; it walks the CPU up to 64-bit long mode using the BSP's page
# tables, then jumps to the C entry ap_entry().
#
# The BSP fills a small parameter page at 0x9000 before sending SIPIs:
#   0x9000 : CR3 (PML4 physical, low 32 bits used here)
#   0x9008 : ap_entry (64-bit C entry address)
#   0x9010 : per-CPU stack base (each AP takes a 16 KiB slot)
#   0x9018 : atomic AP index counter (starts at 0)

    .code16
    .global _tramp_start
_tramp_start:
    ljmp    $0x0000, $reset16       # reload CS=0 so 16-bit offsets are 0x8000-based
reset16:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    lgdtl   gdtr                    # DS=0 -> operand linear addr = gdtr (0x80xx)
    movl    %cr0, %eax
    orl     $1, %eax                # CR0.PE
    movl    %eax, %cr0
    ljmpl   $0x08, $prot32          # load a 32-bit code segment

    .code32
prot32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movl    %cr4, %eax
    orl     $0x20, %eax             # CR4.PAE
    movl    %eax, %cr4
    movl    0x9000, %eax            # CR3 (PML4 base, < 4 GiB)
    movl    %eax, %cr3
    movl    $0xC0000080, %ecx       # IA32_EFER
    rdmsr
    orl     $0x100, %eax            # EFER.LME
    wrmsr
    movl    %cr0, %eax
    orl     $0x80000000, %eax       # CR0.PG -> enter long mode
    movl    %eax, %cr0
    ljmpl   $0x18, $long64          # load a 64-bit code segment

    .code64
long64:
    movw    $0x20, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movabs  $0x9018, %rbx           # atomic AP index
    movq    $1, %rax
    lock xaddq %rax, (%rbx)         # rax = old index (0-based)
    incq    %rax                    # 1-based slot
    imulq   $0x4000, %rax           # * 16 KiB
    movabs  $0x9010, %rbx
    addq    (%rbx), %rax            # + stack base
    movq    %rax, %rsp              # per-CPU stack top
    xorl    %ebp, %ebp
    movabs  $0x9008, %rbx
    movq    (%rbx), %rax            # ap_entry
    callq   *%rax
1:  hlt
    jmp     1b

    .balign 16
gdt:
    .quad   0x0000000000000000      # 0x00 null
    .quad   0x00CF9A000000FFFF      # 0x08 32-bit code
    .quad   0x00CF92000000FFFF      # 0x10 32-bit data
    .quad   0x00AF9A000000FFFF      # 0x18 64-bit code (L=1)
    .quad   0x00CF92000000FFFF      # 0x20 64-bit data
gdtr:
    .word   gdtr - gdt - 1
    .long   gdt                     # 32-bit base (linked at 0x80xx)

    .global _tramp_end
_tramp_end:
