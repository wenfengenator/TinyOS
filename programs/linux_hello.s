/* SPDX-License-Identifier: GPL-2.0-only */

    .code64
    .section .text
    .global _start
_start:
    movq    %rsp, %r13
    cmpq    $1, (%rsp)
    jne     .Lbad_stack
    movq    8(%rsp), %rax
    cmpb    $'l', (%rax)
    jne     .Lbad_stack

    xorl    %r14d, %r14d
    xorl    %r15d, %r15d
    leaq    32(%r13), %rsi
.Laux_loop:
    movq    (%rsi), %rax
    testq   %rax, %rax
    je      .Laux_done
    cmpq    $3, %rax
    jne     .Laux_random
    movq    8(%rsi), %r14
.Laux_random:
    cmpq    $25, %rax
    jne     .Laux_next
    movq    8(%rsi), %r15
.Laux_next:
    addq    $16, %rsi
    jmp     .Laux_loop
.Laux_done:
    testq   %r14, %r14
    jz      .Lbad_stack
    cmpl    $6, (%r14)
    jne     .Lbad_stack
    testq   %r15, %r15
    jz      .Lbad_stack
    movq    (%r15), %rax
    orq     8(%r15), %rax
    jz      .Lbad_stack

    subq    $128, %rsp
    movabsq $0x544C532D54494E59, %rax
    movq    %rax, (%rsp)
    movl    $0x1002, %edi
    movq    %rsp, %rsi
    movl    $158, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movq    %fs:0, %rax
    movabsq $0x544C532D54494E59, %rcx
    cmpq    %rcx, %rax
    jne     .Lbad_stack
    movl    $0x1003, %edi
    leaq    8(%rsp), %rsi
    movl    $158, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movq    8(%rsp), %rax
    cmpq    %rsp, %rax
    jne     .Lbad_stack
    movl    $0x1002, %edi
    xorl    %esi, %esi
    movl    $158, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack

    leaq    16(%rsp), %rdi
    movl    $218, %eax
    syscall
    testq   %rax, %rax
    jle     .Lbad_stack
    movq    $1, 24(%rsp)
    movl    $2, %edi
    leaq    24(%rsp), %rsi
    leaq    32(%rsp), %rdx
    movl    $8, %r10d
    movl    $14, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    cmpq    $0, 32(%rsp)
    jne     .Lbad_stack
    movl    $1, %edi
    leaq    40(%rsp), %rsi
    movl    $228, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    leaq    56(%rsp), %rdi
    movl    $16, %esi
    xorl    %edx, %edx
    movl    $318, %eax
    syscall
    cmpq    $16, %rax
    jne     .Lbad_stack
    movq    56(%rsp), %rax
    orq     64(%rsp), %rax
    jz      .Lbad_stack
    movl    $1, %edi
    movl    $0x5413, %esi
    leaq    72(%rsp), %rdx
    movl    $16, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack

    leaq    libc_message(%rip), %rax
    movq    %rax, 80(%rsp)
    movq    $(libc_message_end - libc_message), 88(%rsp)
    movl    $1, %edi
    leaq    80(%rsp), %rsi
    movl    $1, %edx
    movl    $20, %eax
    syscall
    cmpq    $(libc_message_end - libc_message), %rax
    jne     .Lbad_stack
    addq    $128, %rsp

    xorl    %edi, %edi
    movl    $12, %eax
    syscall
    movq    %rax, %rbx
    leaq    0x20000(%rax), %rdi
    movl    $12, %eax
    syscall
    leaq    0x20000(%rbx), %rcx
    cmpq    %rcx, %rax
    jne     .Lbad_stack
    movb    $0x5A, (%rbx)
    movb    $0xA5, 0x1FFFF(%rbx)

    xorl    %edi, %edi
    movl    $0x20000, %esi
    movl    $3, %edx
    movl    $0x22, %r10d
    movq    $-1, %r8
    xorl    %r9d, %r9d
    movl    $9, %eax
    syscall
    testq   %rax, %rax
    js      .Lbad_stack
    movq    %rax, %rbx
    movb    $0x33, (%rbx)
    movb    $0xCC, 0x1FFFF(%rbx)

    movq    %rbx, %rdi
    movl    $0x1000, %esi
    movl    $1, %edx
    movl    $10, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movq    %rbx, %rdi
    movl    $0x1000, %esi
    movl    $3, %edx
    movl    $10, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movb    $0x66, (%rbx)

    movq    %rbx, %rdi
    movl    $0x20000, %esi
    movl    $11, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack

    movq    $-100, %rdi
    leaq    file_path(%rip), %rsi
    movl    $0x242, %edx
    movl    $0644, %r10d
    movl    $257, %eax
    syscall
    testq   %rax, %rax
    js      .Lbad_stack
    movq    %rax, %r12

    subq    $1024, %rsp
    xorl    %ecx, %ecx
.Lfill:
    movb    %cl, (%rsp,%rcx)
    incq    %rcx
    cmpq    $1024, %rcx
    jne     .Lfill
    movq    %r12, %rdi
    movq    %rsp, %rsi
    movl    $1024, %edx
    movl    $1, %eax
    syscall
    cmpq    $1024, %rax
    jne     .Lbad_stack

    movq    %r12, %rdi
    movl    $600, %esi
    xorl    %edx, %edx
    movl    $8, %eax
    syscall
    cmpq    $600, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movq    %rsp, %rsi
    movl    $32, %edx
    xorl    %eax, %eax
    syscall
    cmpq    $32, %rax
    jne     .Lbad_stack
    cmpb    $0x58, (%rsp)
    jne     .Lbad_stack
    cmpb    $0x77, 31(%rsp)
    jne     .Lbad_stack
    movq    %r12, %rdi
    movq    %rsp, %rsi
    movl    $5, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    cmpq    $1024, 48(%rsp)
    jne     .Lbad_stack
    movl    24(%rsp), %ecx
    andl    $0170000, %ecx
    cmpl    $0100000, %ecx
    jne     .Lbad_stack

    movq    %r12, %rdi
    movl    $100, %esi
    movl    $77, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movl    $700, %esi
    movl    $77, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movl    $100, %esi
    xorl    %edx, %edx
    movl    $8, %eax
    syscall
    cmpq    $100, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movq    %rsp, %rsi
    movl    $600, %edx
    xorl    %eax, %eax
    syscall
    cmpq    $600, %rax
    jne     .Lbad_stack
    xorl    %ecx, %ecx
.Lcheck_extend_zero:
    cmpb    $0, (%rsp,%rcx)
    jne     .Lbad_stack
    incq    %rcx
    cmpq    $600, %rcx
    jne     .Lcheck_extend_zero

    movq    %r12, %rdi
    movl    $900, %esi
    xorl    %edx, %edx
    movl    $8, %eax
    syscall
    cmpq    $900, %rax
    jne     .Lbad_stack
    movb    $0xA6, (%rsp)
    movq    %r12, %rdi
    movq    %rsp, %rsi
    movl    $1, %edx
    movl    $1, %eax
    syscall
    cmpq    $1, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movl    $700, %esi
    xorl    %edx, %edx
    movl    $8, %eax
    syscall
    cmpq    $700, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movq    %rsp, %rsi
    movl    $201, %edx
    xorl    %eax, %eax
    syscall
    cmpq    $201, %rax
    jne     .Lbad_stack
    xorl    %ecx, %ecx
.Lcheck_sparse_zero:
    cmpb    $0, (%rsp,%rcx)
    jne     .Lbad_stack
    incq    %rcx
    cmpq    $200, %rcx
    jne     .Lcheck_sparse_zero
    cmpb    $0xA6, 200(%rsp)
    jne     .Lbad_stack

    movq    %r12, %rdi
    movl    $1024, %esi
    movl    $77, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movl    $901, %esi
    xorl    %edx, %edx
    movl    $8, %eax
    syscall
    cmpq    $901, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movq    %rsp, %rsi
    movl    $123, %edx
    xorl    %eax, %eax
    syscall
    cmpq    $123, %rax
    jne     .Lbad_stack
    xorl    %ecx, %ecx
.Lcheck_tail_zero:
    cmpb    $0, (%rsp,%rcx)
    jne     .Lbad_stack
    incq    %rcx
    cmpq    $123, %rcx
    jne     .Lcheck_tail_zero
    movq    %r12, %rdi
    movl    $3, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    addq    $1024, %rsp

    movl    $1, %eax
    movl    $1, %edi
    leaq    truncate_message(%rip), %rsi
    movl    $(truncate_message_end - truncate_message), %edx
    syscall
    cmpq    $(truncate_message_end - truncate_message), %rax
    jne     .Lbad_stack

    subq    $1024, %rsp
    movq    %rsp, %rdi
    movl    $80, %esi
    movl    $79, %eax
    syscall
    cmpq    %rsp, %rax
    jne     .Lbad_stack
    cmpb    $'/', (%rsp)
    jne     .Lbad_stack

    movq    $-100, %rdi
    leaq    file_path(%rip), %rsi
    movq    %rsp, %rdx
    xorl    %r10d, %r10d
    movl    $262, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    cmpq    $1024, 48(%rsp)
    jne     .Lbad_stack

    movq    $-100, %rdi
    leaq    null_path(%rip), %rsi
    movl    $2, %edx
    movl    $257, %eax
    syscall
    testq   %rax, %rax
    js      .Lbad_stack
    movq    %rax, %r12
    movq    %r12, %rdi
    leaq    file_path(%rip), %rsi
    movl    $1, %edx
    movl    $1, %eax
    syscall
    cmpq    $1, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movq    %rsp, %rsi
    movl    $1, %edx
    xorl    %eax, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movq    %r12, %rdi
    movl    $3, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack

    movq    $-100, %rdi
    leaq    temp_path(%rip), %rsi
    movl    $0x242, %edx
    movl    $0644, %r10d
    movl    $257, %eax
    syscall
    testq   %rax, %rax
    js      .Lbad_stack
    movq    %rax, %rdi
    movl    $3, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movq    $-100, %rdi
    leaq    temp_path(%rip), %rsi
    xorl    %edx, %edx
    movl    $263, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    movq    $-100, %rdi
    leaq    temp_path(%rip), %rsi
    movq    %rsp, %rdx
    xorl    %r10d, %r10d
    movl    $262, %eax
    syscall
    cmpq    $-2, %rax
    jne     .Lbad_stack

    movq    $-100, %rdi
    leaq    dir_path(%rip), %rsi
    movl    $0755, %edx
    movl    $258, %eax
    syscall
    testq   %rax, %rax
    je      .Ldir_ready
    cmpq    $-17, %rax
    jne     .Lbad_stack
.Ldir_ready:
    movq    $-100, %rdi
    leaq    root_path(%rip), %rsi
    movl    $0x10000, %edx
    movl    $257, %eax
    syscall
    testq   %rax, %rax
    js      .Lbad_stack
    movq    %rax, %r12
    movq    %r12, %rdi
    movq    %rsp, %rsi
    movl    $1024, %edx
    movl    $217, %eax
    syscall
    testq   %rax, %rax
    jle     .Lbad_stack
    cmpw    $24, 16(%rsp)
    jb      .Lbad_stack
    movb    18(%rsp), %al
    cmpb    $4, %al
    je      .Ldirent_ok
    cmpb    $8, %al
    jne     .Lbad_stack
.Ldirent_ok:
    movq    %r12, %rdi
    movl    $3, %eax
    syscall
    testq   %rax, %rax
    jne     .Lbad_stack
    addq    $1024, %rsp

    movl    $1, %eax
    movl    $1, %edi
    leaq    vfs_message(%rip), %rsi
    movl    $(vfs_message_end - vfs_message), %edx
    syscall
    cmpq    $(vfs_message_end - vfs_message), %rax
    jne     .Lbad_stack

    movl    $1, %eax
    movl    $1, %edi
    leaq    fd_message(%rip), %rsi
    movl    $(fd_message_end - fd_message), %edx
    syscall
    cmpq    $(fd_message_end - fd_message), %rax
    jne     .Lbad_stack

    movl    $1, %eax
    movl    $1, %edi
    leaq    message(%rip), %rsi
    movl    $(message_end - message), %edx
    syscall
    cmpq    $(message_end - message), %rax
    jne     .Lbad_stack

    xorl    %edi, %edi
    movl    $231, %eax
    syscall

.Lbad_stack:
    movl    $1, %edi
    movl    $231, %eax
    syscall
1:  jmp     1b

message:
    .ascii  "[linux-abi] hello from static ELF64\n"
message_end:

fd_message:
    .ascii  "[linux-abi] fd offset I/O ok\n"
fd_message_end:

vfs_message:
    .ascii  "[linux-abi] stat + directory VFS ok\n"
vfs_message_end:

truncate_message:
    .ascii  "[linux-abi] ftruncate + sparse I/O ok\n"
truncate_message_end:

libc_message:
    .ascii  "[linux-abi] auxv + TLS + libc stubs ok\n"
libc_message_end:

file_path:
    .asciz  "/abiio.bin"
null_path:
    .asciz  "/dev/null"
temp_path:
    .asciz  "/abitmp.bin"
dir_path:
    .asciz  "/abidir"
root_path:
    .asciz  "/"

    .section .note.GNU-stack,"",@progbits
