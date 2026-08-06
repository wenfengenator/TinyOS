/* SPDX-License-Identifier: GPL-2.0-only */

# tinyos user program startup (crt0), linked at USER_BASE (0x40000000).
# Passes the exec argument pointer (mapped at 0x50000000) to umain(), then
# performs sys_exit(return value).
    .code32
    .section .text._start
    .global _start
    .extern umain
_start:
    pushl   $0x50000000         # argument string (see do_exec)
    call    umain
    addl    $4, %esp
    movl    %eax, %ebx          # exit code = umain() return
    movl    $1, %eax            # SYS_EXIT
    int     $0x80
1:  jmp     1b

    .section .note.GNU-stack,"",@progbits
