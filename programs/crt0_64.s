/* tinyos x64 user program startup (crt0).
 *
 * The 64-bit counterpart of programs/crt0.s: it calls umain() and then performs
 * SYS_EXIT with the return value using the SYSCALL ABI (rax=number, rdi=arg0).
 * Programs are compiled position-independent and loaded at USER_CODE_VA. */
    .code64
    .section .text._start
    .global _start
    .extern umain
_start:
    call    umain
    movl    %eax, %edi          /* exit code = umain() return */
    movl    $1, %eax            /* SYS_EXIT */
    syscall
1:  jmp     1b                  /* unreachable */

    .section .note.GNU-stack,"",@progbits
