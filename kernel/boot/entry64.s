/* SPDX-License-Identifier: GPL-2.0-only */

# tinyos x64 kernel entry trampoline (R3).
#
# The UEFI loader calls kstart(boot_info *) — SysV ABI, so the pointer arrives
# in RDI — immediately after ExitBootServices.  We mask interrupts (there is no
# IDT yet), install a known-good kernel stack, then call the C entry kmain64.
# This is the one bit of the x64 path that must be assembly.

    .code64
    .section .text
    .global kstart
    .hidden kstart
    .extern kmain64
kstart:
    cli                             # no IDT installed yet
    leaq    kstack_top(%rip), %rsp  # switch to the kernel stack
    andq    $-16, %rsp              # 16-byte align for the SysV ABI
    xorl    %ebp, %ebp              # terminate stack-frame chains
    call    kmain64                 # RDI still holds boot_info *
1:  hlt                             # kmain64 should not return
    jmp     1b

    .section .bss
    .balign 16
kstack:
    .skip   65536                   # 64 KiB kernel stack
kstack_top:

    .section .note.GNU-stack,"",@progbits
