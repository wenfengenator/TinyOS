    .code64
    .section .text
    .global _start
/* Phase F: tty + process groups.  Verify TCGETS (isatty + a nonzero c_lflag),
 * setsid, getpgid, setpgid, and a TIOCSPGRP/TIOCGPGRP round-trip. */
_start:
    subq    $64, %rsp
    movl    $1, %edi                      /* ioctl(1, TCGETS, buf) */
    movl    $0x5401, %esi
    movq    %rsp, %rdx
    movl    $16, %eax
    syscall
    testq   %rax, %rax
    jnz     .Lfail
    movl    12(%rsp), %eax                /* c_lflag must be nonzero (ISIG|ICANON|ECHO) */
    testl   %eax, %eax
    jz      .Lfail
    movl    $112, %eax                    /* setsid() -> sid > 0 */
    syscall
    testq   %rax, %rax
    jle     .Lfail
    xorl    %edi, %edi                    /* getpgid(0) -> pgid > 0 */
    movl    $121, %eax
    syscall
    testq   %rax, %rax
    jle     .Lfail
    xorl    %edi, %edi                    /* setpgid(0, 0) -> 0 */
    xorl    %esi, %esi
    movl    $109, %eax
    syscall
    testq   %rax, %rax
    jnz     .Lfail
    movl    $39, %eax                     /* getpid -> pgrp at rsp+32 */
    syscall
    movl    %eax, 32(%rsp)
    movl    $1, %edi                      /* TIOCSPGRP(1, &pgrp) */
    movl    $0x5410, %esi
    leaq    32(%rsp), %rdx
    movl    $16, %eax
    syscall
    testq   %rax, %rax
    jnz     .Lfail
    movl    $1, %edi                      /* TIOCGPGRP(1, &got) at rsp+40 */
    movl    $0x540F, %esi
    leaq    40(%rsp), %rdx
    movl    $16, %eax
    syscall
    testq   %rax, %rax
    jnz     .Lfail
    movl    32(%rsp), %eax
    movl    40(%rsp), %ecx
    cmpl    %eax, %ecx                    /* set pgrp == got pgrp? */
    jne     .Lfail
    xorl    %edi, %edi                    /* exit_group(0) */
    movl    $231, %eax
    syscall
.Lfail:
    movl    $1, %edi
    movl    $231, %eax
    syscall
