/* SPDX-License-Identifier: GPL-2.0-only */

    .code64
    .section .text
    .global _start
/* pipe()+fork()+dup2() self-test.  The child redirects stdout(1) to the pipe's
 * write end and writes "hello"; the parent redirects stdin(0) to the read end,
 * reads it back, verifies "hello", and wait4()s the child.  exit_group(0) only on
 * full success (proving pipe(22), dup2(33), close(3) + blocking pipe I/O across
 * two scheduled processes).  A 32-byte frame: fds[2] at rsp, buf/status at rsp+16. */
_start:
    subq    $32, %rsp
    movq    %rsp, %rdi                    /* pipe(fds) -> rsp[0]=read, rsp[4]=write */
    movl    $22, %eax
    syscall
    testq   %rax, %rax
    jnz     .Lfail
    movl    (%rsp), %r12d                 /* read fd (callee-saved) */
    movl    4(%rsp), %r13d                /* write fd */
    movl    $57, %eax                     /* fork */
    syscall
    testq   %rax, %rax
    js      .Lfail
    jz      .Lchild
.Lparent:
    movl    %r13d, %edi                   /* close(write end) */
    movl    $3, %eax
    syscall
    movl    %r12d, %edi                   /* dup2(read end, 0) */
    xorl    %esi, %esi
    movl    $33, %eax
    syscall
    movl    %r12d, %edi                   /* close(read end) */
    movl    $3, %eax
    syscall
    xorl    %edi, %edi                    /* read(0, buf, 16) */
    leaq    16(%rsp), %rsi
    movl    $16, %edx
    xorl    %eax, %eax
    syscall
    cmpq    $5, %rax                      /* got 5 bytes? */
    jne     .Lfail
    movl    16(%rsp), %eax                /* "hell" */
    cmpl    $0x6C6C6568, %eax
    jne     .Lfail
    movzbl  20(%rsp), %eax                /* 'o' */
    cmpl    $0x6F, %eax
    jne     .Lfail
    movq    $-1, %rdi                     /* wait4(-1, &status, 0, 0) */
    leaq    16(%rsp), %rsi
    xorl    %edx, %edx
    xorl    %r10d, %r10d
    movl    $61, %eax
    syscall
    testq   %rax, %rax
    jle     .Lfail
    movl    16(%rsp), %eax
    sarl    $8, %eax
    andl    $0xFF, %eax                   /* child exit code */
    testl   %eax, %eax
    jne     .Lfail
    xorl    %edi, %edi                    /* exit_group(0) */
    movl    $231, %eax
    syscall
.Lchild:
    movl    %r12d, %edi                   /* close(read end) */
    movl    $3, %eax
    syscall
    movl    %r13d, %edi                   /* dup2(write end, 1) */
    movl    $1, %esi
    movl    $33, %eax
    syscall
    movl    %r13d, %edi                   /* close(write end) */
    movl    $3, %eax
    syscall
    movl    $1, %edi                      /* write(1, "hello", 5) */
    leaq    pmsg(%rip), %rsi
    movl    $5, %edx
    movl    $1, %eax
    syscall
    xorl    %edi, %edi                    /* exit_group(0) */
    movl    $231, %eax
    syscall
.Lfail:
    movl    $1, %edi
    movl    $231, %eax
    syscall

    .section .rodata
pmsg:   .ascii "hello"
