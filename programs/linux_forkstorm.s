/* SPDX-License-Identifier: GPL-2.0-only */

    .code64
    .section .text
    .global _start
/* Fork-storm: fork()+wait4() in a loop MORE times than the scheduler task table
 * (MAXTASK) has slots.  It only completes if finished task slots are reclaimed
 * and reused; otherwise fork() eventually returns -EAGAIN and the test fails.
 * r12 (callee-saved) holds the loop counter so it survives each syscall + fork. */
_start:
    movl    $20, %r12d                    /* iterations (> MAXTASK) */
.Lloop:
    movl    $57, %eax                     /* fork */
    syscall
    testq   %rax, %rax
    js      .Lfail                        /* fork < 0 => task table exhausted */
    jz      .Lchild
    subq    $16, %rsp                     /* parent: wait4(-1, &status, 0, 0) */
    movq    $-1, %rdi
    movq    %rsp, %rsi
    xorl    %edx, %edx
    xorl    %r10d, %r10d
    movl    $61, %eax
    syscall
    addq    $16, %rsp
    testq   %rax, %rax
    jle     .Lfail                        /* wait4 must reap a child */
    decl    %r12d
    jnz     .Lloop
    xorl    %edi, %edi                    /* all iterations OK => exit_group(0) */
    movl    $231, %eax
    syscall
.Lchild:
    xorl    %edi, %edi                    /* child => exit_group(0) */
    movl    $231, %eax
    syscall
.Lfail:
    movl    $1, %edi                      /* exit_group(1) */
    movl    $231, %eax
    syscall
