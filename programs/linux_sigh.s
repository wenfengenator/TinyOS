/* SPDX-License-Identifier: GPL-2.0-only */

    .code64
    .section .text
    .global _start
/* Phase E.2: custom signal handler + rt_sigreturn.  Install a SIGINT handler,
 * send ourselves SIGINT (delivered at the kill() return).  The handler checks its
 * signo argument and overwrites the SAVED rax in its own signal frame (at 56(%rsp)
 * on entry), then returns via the restorer -> rt_sigreturn.  Execution resumes
 * after kill() with the doctored rax, proving delivery + frame layout + restorer
 * + rt_sigreturn.  Needs no writable data. */
_start:
    subq    $32, %rsp                     /* struct sigaction {handler,flags,restorer,mask} */
    leaq    handler(%rip), %rax
    movq    %rax, (%rsp)
    movq    $0x04000000, 8(%rsp)          /* sa_flags = SA_RESTORER */
    leaq    restorer(%rip), %rax
    movq    %rax, 16(%rsp)
    movq    $0, 24(%rsp)
    movl    $2, %edi                      /* SIGINT */
    movq    %rsp, %rsi
    xorl    %edx, %edx                    /* oldact = NULL */
    movl    $8, %r10d                     /* sigsetsize */
    movl    $13, %eax                     /* rt_sigaction */
    syscall
    testq   %rax, %rax
    jnz     .Lfail
    movl    $39, %eax                     /* getpid */
    syscall
    movl    %eax, %edi
    movl    $2, %esi                      /* SIGINT */
    movl    $62, %eax                     /* kill(self, SIGINT) */
    syscall
    cmpq    $0x1234, %rax                 /* handler doctored the resumed rax? */
    jne     .Lfail
    xorl    %edi, %edi                    /* exit_group(0) */
    movl    $231, %eax
    syscall
.Lfail:
    movl    $1, %edi
    movl    $231, %eax
    syscall
handler:                                  /* entered with rdi=signo, rsp=frame base */
    cmpl    $2, %edi                      /* signo == SIGINT? */
    jne     .Lhbad
    movq    $0x1234, 56(%rsp)             /* overwrite saved rax in the signal frame */
    ret                                   /* pops restorer -> rt_sigreturn */
.Lhbad:
    movq    $0x0BAD, 56(%rsp)
    ret
restorer:
    movl    $15, %eax                     /* rt_sigreturn */
    syscall
