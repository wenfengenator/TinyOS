    .code64
    .section .text
    .global _start
/* Minimal real-Linux-ABI fork()+wait4() self-test.  Sets callee-saved rbx/rbp to
 * magic values before fork(); the child resumes on a fresh kernel-built frame and
 * must observe them inherited, proving the kernel copies the parent's full
 * register frame.  The parent wait4()s the child and exit_group(0) ONLY if the
 * child exited 7 — so a working fork/wait4 makes run_linux_elf_frames return 0. */
_start:
    movabsq $0x1234567890ABCDEF, %rbx     /* callee-saved magic (must survive fork) */
    movq    $0x1111, %rbp                 /* callee-saved magic                     */
    movl    $57, %eax                     /* fork                                   */
    syscall
    testq   %rax, %rax
    jz      .Lchild
.Lparent:
    movabsq $0x1234567890ABCDEF, %rcx     /* our own rbx must be intact             */
    cmpq    %rcx, %rbx
    jne     .Lfail
    subq    $16, %rsp                     /* room for status                        */
    movq    $-1, %rdi                     /* wait4(-1, &status, 0, 0)               */
    movq    %rsp, %rsi
    xorl    %edx, %edx
    xorl    %r10d, %r10d
    movl    $61, %eax
    syscall
    testq   %rax, %rax
    jle     .Lfail                        /* must reap a child pid > 0              */
    movl    (%rsp), %eax                  /* status                                 */
    sarl    $8, %eax
    andl    $0xFF, %eax                   /* WEXITSTATUS(status)                    */
    cmpl    $7, %eax
    jne     .Lfail
    xorl    %edi, %edi                    /* exit_group(0) — success                */
    movl    $231, %eax
    syscall
.Lfail:
    movl    $1, %edi                      /* exit_group(1) — parent-side failure    */
    movl    $231, %eax
    syscall
.Lchild:
    movabsq $0x1234567890ABCDEF, %rcx     /* inherited rbx?                         */
    cmpq    %rcx, %rbx
    jne     .Lcfail
    cmpq    $0x1111, %rbp                 /* inherited rbp?                         */
    jne     .Lcfail
    movl    $7, %edi                      /* exit_group(7) — child ran correctly    */
    movl    $231, %eax
    syscall
.Lcfail:
    movl    $88, %edi                     /* exit_group(88) — register inheritance broken */
    movl    $231, %eax
    syscall
