    .code64
    .section .text
    .global _start
/* fork()+execve()+wait4() self-test for the scheduled process model.  The child
 * execve()s /exec.bin (which verifies its argv/envp and exit_group(0)); the parent
 * wait4()s it and exit_group(0) ONLY if the child exited 0 — so a working
 * fork/execve/wait4 makes run_linux_elf_frames return 0. */
_start:
    movl    $57, %eax                     /* fork */
    syscall
    testq   %rax, %rax
    jz      .Lchild
.Lparent:
    subq    $16, %rsp                     /* room for status */
    movq    $-1, %rdi                     /* wait4(-1, &status, 0, 0) */
    movq    %rsp, %rsi
    xorl    %edx, %edx
    xorl    %r10d, %r10d
    movl    $61, %eax
    syscall
    testq   %rax, %rax
    jle     .Lfail                        /* must reap a child pid > 0 */
    movl    (%rsp), %eax                  /* status */
    sarl    $8, %eax
    andl    $0xFF, %eax                   /* WEXITSTATUS(status) */
    testl   %eax, %eax                    /* child (exec'd /exec.bin) must exit 0 */
    jne     .Lfail
    xorl    %edi, %edi                    /* exit_group(0) — success */
    movl    $231, %eax
    syscall
.Lfail:
    movl    $1, %edi                      /* exit_group(1) — parent-side failure */
    movl    $231, %eax
    syscall
.Lchild:
    leaq    ppath(%rip), %rdi             /* execve("/exec.bin", argv, envp) */
    leaq    pargv(%rip), %rsi
    leaq    penvp(%rip), %rdx
    movl    $59, %eax
    syscall
    movl    $77, %edi                     /* exit_group(77) — execve returned => failed */
    movl    $231, %eax
    syscall

    .section .rodata
ppath:  .asciz "/exec.bin"
parg0:  .asciz "/exec.bin"
parg1:  .asciz "alpha"
penv0:  .asciz "TINYOS=1"
    .align 8
pargv:  .quad parg0, parg1, 0
penvp:  .quad penv0, 0
